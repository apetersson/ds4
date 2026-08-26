#!/usr/bin/env python3

import hashlib
import json
import os
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
PROBE = Path(os.environ.get(
    "DS4_HF_RUNTIME_PROBE", ROOT / "tests" / "test_hf_runtime_probe"
))
FIXTURE = ROOT / "tests" / "fixtures" / "hf" / "variants-v2.json"
SHA = "0123456789abcdef0123456789abcdef01234567"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def build_safetensors(tensors, metadata=None):
    header = {}
    if metadata is not None:
        header["__metadata__"] = metadata
    for name, shape in tensors.items():
        header[name] = {"dtype": "BF16", "shape": shape,
                        "data_offsets": [0, 0]}
    encoded = json.dumps(header, separators=(",", ":")).encode("utf-8")
    encoded += b" " * (-len(encoded) % 8)
    return struct.pack("<Q", len(encoded)) + encoded


def build_vision_tower():
    return build_safetensors({
        "model.sam_model.patch_embed.proj.weight": [768, 3, 16, 16],
        "model.sam_model.pos_embed": [1, 64, 64, 768],
        "model.sam_model.neck.0.weight": [256, 768, 1, 1],
        "model.qwen2_model.model.model.layers.0.self_attn.q_proj.weight":
            [896, 896],
        "model.qwen2_model.model.model.layers.23.self_attn.q_proj.weight":
            [896, 896],
        "model.qwen2_model.model.model.norm.weight": [896],
    })


def build_vision_projector(proj0_shape=(4096, 896)):
    return build_safetensors({
        "proj.0.weight": list(proj0_shape),
        "proj.2.weight": [4096, 4096],
        "view_seperator": [4096],
    }, {
        "encoder_dim": "896",
        "hidden": "4096",
        "image_token_id": "129279",
    })


def build_tokenizer_gguf():
    """Build a metadata-only DeepSeek4 GGUF accepted by --dump-tokens."""
    uint32 = 4
    float32 = 6
    boolean = 7
    string = 8
    array = 9

    def gguf_string(value):
        encoded = value.encode("utf-8")
        return struct.pack("<Q", len(encoded)) + encoded

    entries = []

    def scalar(key, kind, value):
        if kind == uint32:
            payload = struct.pack("<I", value)
        elif kind == float32:
            payload = struct.pack("<f", value)
        elif kind == boolean:
            payload = struct.pack("<B", value)
        elif kind == string:
            payload = gguf_string(value)
        else:
            raise AssertionError(kind)
        entries.append(gguf_string(key) + struct.pack("<I", kind) + payload)

    def values(key, kind, items):
        payload = struct.pack("<IQ", kind, len(items))
        if kind == uint32:
            payload += b"".join(struct.pack("<I", item) for item in items)
        elif kind == float32:
            payload += b"".join(struct.pack("<f", item) for item in items)
        elif kind == string:
            payload += b"".join(gguf_string(item) for item in items)
        else:
            raise AssertionError(kind)
        entries.append(gguf_string(key) + struct.pack("<I", array) + payload)

    shape = {
        "deepseek4.block_count": 43,
        "deepseek4.embedding_length": 4096,
        "deepseek4.vocab_size": 129280,
        "deepseek4.attention.head_count": 64,
        "deepseek4.attention.head_count_kv": 1,
        "deepseek4.attention.key_length": 512,
        "deepseek4.attention.value_length": 512,
        "deepseek4.rope.dimension_count": 64,
        "deepseek4.attention.q_lora_rank": 1024,
        "deepseek4.attention.output_lora_rank": 1024,
        "deepseek4.attention.output_group_count": 8,
        "deepseek4.expert_count": 256,
        "deepseek4.expert_used_count": 6,
        "deepseek4.expert_feed_forward_length": 2048,
        "deepseek4.expert_shared_count": 1,
        "deepseek4.hash_layer_count": 3,
        "deepseek4.attention.sliding_window": 128,
        "deepseek4.attention.indexer.head_count": 64,
        "deepseek4.attention.indexer.key_length": 128,
        "deepseek4.attention.indexer.top_k": 512,
        "deepseek4.hyper_connection.count": 4,
        "deepseek4.hyper_connection.sinkhorn_iterations": 20,
    }
    for key, value in shape.items():
        scalar(key, uint32, value)
    for key, value in {
        "deepseek4.rope.freq_base": 10000.0,
        "deepseek4.attention.compress_rope_freq_base": 160000.0,
        "deepseek4.expert_weights_scale": 1.5,
        "deepseek4.attention.layer_norm_rms_epsilon": 1.0e-6,
        "deepseek4.hyper_connection.epsilon": 1.0e-6,
    }.items():
        scalar(key, float32, value)
    scalar("deepseek4.expert_weights_norm", boolean, True)
    ratios = [0, 0] + [4 if layer % 2 == 0 else 128 for layer in range(2, 43)]
    values("deepseek4.attention.compress_ratios", uint32, ratios)
    values("deepseek4.swiglu_clamp_exp", float32, [10.0] * 43)
    tokens = [
        "<｜begin▁of▁sentence｜>", "<｜end▁of▁sentence｜>",
        "<｜User｜>", "<｜Assistant｜>", "<think>", "</think>",
        "｜DSML｜", "p", "a", "r", "i", "t", "y",
    ]
    values("tokenizer.ggml.tokens", string, tokens)
    values("tokenizer.ggml.merges", string, [])
    return b"GGUF" + struct.pack("<IQQ", 3, 0, len(entries)) + b"".join(entries)


class RuntimeHubHandler(BaseHTTPRequestHandler):
    requests = []
    payloads = {}
    manifest = b""

    def log_message(self, _format, *_args):
        pass

    def send_bytes(self, status, body, content_type="application/octet-stream"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = unquote(urlsplit(self.path).path)
        type(self).requests.append(path)
        if path == "/api/models/owner/repo":
            self.send_bytes(200, json.dumps({"sha": SHA}).encode(), "application/json")
            return
        prefix = f"/owner/repo/resolve/{SHA}/"
        if path == prefix + "variants.json":
            self.send_bytes(200, type(self).manifest, "application/json")
            return
        payload = type(self).payloads.get(path.removeprefix(prefix))
        if payload is None:
            self.send_bytes(404, b"missing")
        else:
            self.send_bytes(200, payload)


class RuntimeWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        manifest = json.loads(FIXTURE.read_text(encoding="utf-8"))
        manifest["repository"] = "owner/repo"
        manifest["variants"] = manifest["variants"][:1]
        variant = manifest["variants"][0]
        payloads = {
            variant["receiver"]["path"]: build_tokenizer_gguf(),
            variant["ds4_vision"]["tower"]["path"]: build_vision_tower(),
            variant["ds4_vision"]["projector"]["path"]:
                build_vision_projector(),
            variant["ds4_vision"]["config"]["path"]: b'{"verified":true}',
        }
        for artifact in (
            variant["receiver"],
            variant["ds4_vision"]["tower"],
            variant["ds4_vision"]["projector"],
            variant["ds4_vision"]["config"],
        ):
            data = payloads[artifact["path"]]
            artifact["bytes"] = len(data)
            artifact["sha256"] = sha256(data)
        cls.good_payloads = payloads
        cls.good_manifest = manifest
        RuntimeHubHandler.payloads = dict(payloads)
        RuntimeHubHandler.manifest = json.dumps(manifest).encode("utf-8")
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), RuntimeHubHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.endpoint = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        RuntimeHubHandler.requests = []
        RuntimeHubHandler.payloads = dict(self.good_payloads)
        RuntimeHubHandler.manifest = json.dumps(self.good_manifest).encode("utf-8")
        self.cache = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.cache.cleanup()

    def reset_cache(self):
        self.cache.cleanup()
        self.cache = tempfile.TemporaryDirectory()

    def run_probe(self, mode):
        env = os.environ.copy()
        for name in ("HF_TOKEN", "HF_TOKEN_PATH", "HF_HOME", "XDG_CACHE_HOME"):
            env.pop(name, None)
        proc = subprocess.Popen(
            [str(PROBE), self.endpoint, self.cache.name, mode],
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        lines = []
        while True:
            line = proc.stdout.readline()
            self.assertNotEqual(line, "", proc.stderr.read())
            lines.append(line.rstrip("\n"))
            if line.rstrip("\n") == "READY":
                break
        requests_at_ready = list(RuntimeHubHandler.requests)
        time.sleep(0.25)
        self.assertEqual(RuntimeHubHandler.requests, requests_at_ready,
                         "HF network request occurred after startup preparation")
        stdout_tail, stderr = proc.communicate(timeout=5)
        self.assertEqual(proc.returncode, 0, "\n".join(lines) + stdout_tail + stderr)
        lines.extend(stdout_tail.splitlines())
        values = dict(line.split("=", 1) for line in lines if "=" in line)
        return values, requests_at_ready

    def run_probe_failure(self, mode):
        env = os.environ.copy()
        for name in ("HF_TOKEN", "HF_TOKEN_PATH", "HF_HOME", "XDG_CACHE_HOME"):
            env.pop(name, None)
        return subprocess.run(
            [str(PROBE), self.endpoint, self.cache.name, mode],
            cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=10, check=False,
        )

    def test_cli_receiver_handoff_is_byte_identical_and_ignores_vision(self):
        values, requests = self.run_probe("cli")
        self.assertEqual(values["repository"], "owner/repo")
        self.assertEqual(values["revision"], SHA)
        self.assertEqual(values["selector"], "Headroom128-IQ2_XXS")
        self.assertTrue(values["receiver"].endswith(".gguf"))
        self.assertEqual(values["receiver_equal"], "true")
        self.assertEqual(values["vision_verified"], "false")
        self.assertEqual(values["verified_roles"], "receiver")
        self.assertFalse(any("DeepEncoder" in path or "Projector" in path or
                             "upstream-config" in path for path in requests))

    def test_server_verifies_complete_vision_bundle_then_reuses_cache(self):
        first, requests = self.run_probe("server")
        self.assertEqual(first["receiver_equal"], "true")
        self.assertEqual(first["vision_verified"], "true")
        self.assertEqual(first["vision_compatible"], "true")
        self.assertEqual(first["vision_mismatch_rejected"], "true")
        self.assertEqual(first["image_token_id"], "129279")
        self.assertEqual(
            first["verified_roles"],
            "receiver,ds4_vision.tower,ds4_vision.projector,ds4_vision.config",
        )
        self.assertTrue(any("DeepEncoder" in path for path in requests))
        self.assertTrue(any("Projector" in path for path in requests))
        self.assertTrue(any("upstream-config" in path for path in requests))

        RuntimeHubHandler.requests = []
        second, cached_requests = self.run_probe("server")
        self.assertEqual(second, first)
        self.assertEqual(cached_requests, [
            "/api/models/owner/repo",
            f"/owner/repo/resolve/{SHA}/variants.json",
        ])

        RuntimeHubHandler.requests = []
        offline, offline_requests = self.run_probe("server-offline")
        self.assertEqual(offline, first)
        self.assertEqual(offline_requests, [],
                         "offline runtime performed an HF request")

    def test_no_vision_and_explicit_override_download_receiver_only(self):
        for mode in ("no-vision", "explicit"):
            with self.subTest(mode=mode):
                RuntimeHubHandler.requests = []
                values, requests = self.run_probe(mode)
                self.assertEqual(values["vision_verified"], "false")
                self.assertEqual(values["verified_roles"], "receiver")
                self.assertFalse(any("DeepEncoder" in path or
                                     "Projector" in path or
                                     "upstream-config" in path
                                     for path in requests))

    def test_incomplete_catalog_bundle_names_exact_missing_role(self):
        for role in ("tower", "projector", "config"):
            with self.subTest(role=role):
                RuntimeHubHandler.requests = []
                manifest = json.loads(json.dumps(self.good_manifest))
                del manifest["variants"][0]["ds4_vision"][role]
                RuntimeHubHandler.manifest = json.dumps(manifest).encode("utf-8")
                result = self.run_probe_failure("server")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"missing exact role ds4_vision.{role}", result.stderr)
                self.assertFalse(
                    any(path.endswith(".gguf") for path in RuntimeHubHandler.requests),
                    "receiver was acquired before manifest role validation",
                )

    def test_missing_and_mismatched_catalog_artifacts_name_exact_role(self):
        variant = self.good_manifest["variants"][0]
        for role in ("tower", "projector"):
            artifact = variant["ds4_vision"][role]
            with self.subTest(role=role, failure="missing"):
                self.reset_cache()
                RuntimeHubHandler.requests = []
                RuntimeHubHandler.payloads = dict(self.good_payloads)
                RuntimeHubHandler.payloads.pop(artifact["path"])
                missing = self.run_probe_failure("server")
                self.assertNotEqual(missing.returncode, 0)
                self.assertIn(f"role='ds4_vision.{role}'", missing.stderr)

            with self.subTest(role=role, failure="hash"):
                self.reset_cache()
                RuntimeHubHandler.requests = []
                RuntimeHubHandler.payloads = dict(self.good_payloads)
                original = RuntimeHubHandler.payloads[artifact["path"]]
                corrupt = bytes([original[0] ^ 0xff]) + original[1:]
                RuntimeHubHandler.payloads[artifact["path"]] = corrupt
                mismatch = self.run_probe_failure("server")
                self.assertNotEqual(mismatch.returncode, 0)
                self.assertIn(f"role='ds4_vision.{role}'", mismatch.stderr)
                self.assertIn("SHA-256", mismatch.stderr)

    def test_semantically_mismatched_vision_roles_fail_after_hash_verification(self):
        cases = (
            ("wrong-role", "tower", build_vision_projector(),
             "incompatible safetensors semantic header"),
            ("wrong-shape", "projector",
             build_vision_projector(proj0_shape=(896, 4096)),
             "incompatible safetensors semantic header"),
            ("oversized-header", "tower",
             struct.pack("<Q", 1024 * 1024 + 1),
             "invalid bounded safetensors header"),
        )
        for case, role, payload, expected in cases:
            with self.subTest(case=case, role=role):
                self.reset_cache()
                RuntimeHubHandler.requests = []
                manifest = json.loads(json.dumps(self.good_manifest))
                artifact = manifest["variants"][0]["ds4_vision"][role]
                artifact["bytes"] = len(payload)
                artifact["sha256"] = sha256(payload)
                RuntimeHubHandler.manifest = json.dumps(manifest).encode("utf-8")
                RuntimeHubHandler.payloads = dict(self.good_payloads)
                RuntimeHubHandler.payloads[artifact["path"]] = payload

                result = self.run_probe_failure("server")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    f"catalog vision role 'ds4_vision.{role}' has an "
                    f"{expected}",
                    result.stderr,
                )

    def test_hf_and_local_model_paths_produce_identical_deterministic_output(self):
        env = os.environ.copy()
        env["HF_ENDPOINT"] = self.endpoint
        for name in ("HF_TOKEN", "HF_TOKEN_PATH", "HF_HOME", "XDG_CACHE_HOME"):
            env.pop(name, None)
        with tempfile.TemporaryDirectory() as cache:
            env["DS4_LOCK_FILE"] = str(Path(cache) / "ds4.lock")
            common = ["--dump-tokens", "-p", "parity"]
            by_hf = subprocess.run(
                [str(ROOT / "ds4"), "--hf", "owner/repo",
                 "--hf-cache-dir", cache, *common],
                cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=10, check=False,
            )
            self.assertEqual(by_hf.returncode, 0, by_hf.stdout + by_hf.stderr)
            self.assertIn("repository='owner/repo'", by_hf.stderr)
            self.assertIn(f"revision='{SHA}'", by_hf.stderr)
            self.assertIn("selector='Headroom128-IQ2_XXS'", by_hf.stderr)
            self.assertIn("verified_roles=[receiver] vision=inactive", by_hf.stderr)
            self.assertIn("dspark=not-requested", by_hf.stderr)

            receiver_path = next(Path(cache).rglob("*.gguf"))
            by_local_model = subprocess.run(
                [str(ROOT / "ds4"), "--model", str(receiver_path), *common],
                cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=10, check=False,
            )
            self.assertEqual(by_local_model.returncode, 0,
                             by_local_model.stdout + by_local_model.stderr)
            self.assertEqual(by_hf.stdout, by_local_model.stdout)
            self.assertEqual(by_local_model.stderr, "")

    def test_server_handoffs_verified_receiver_before_weight_binding(self):
        env = os.environ.copy()
        env["HF_ENDPOINT"] = self.endpoint
        with tempfile.TemporaryDirectory() as cache, \
                tempfile.NamedTemporaryFile(suffix=".py") as encoder:
            env["DS4_LOCK_FILE"] = str(Path(cache) / "ds4.lock")
            result = subprocess.run(
                [str(ROOT / "ds4-server"), "--cpu", "--hf", "owner/repo",
                 "--hf-cache-dir", cache,
                 "--vision-python", "/bin/sh",
                 "--vision-encoder", encoder.name],
                cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=10, check=False,
            )
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("repository='owner/repo'", output)
        self.assertIn(f"revision='{SHA}'", output)
        self.assertIn("verified_roles=[receiver,ds4_vision.tower,ds4_vision.projector,ds4_vision.config] vision=verified-pending-receiver", output)
        self.assertIn("dspark=not-requested", output)
        self.assertIn("required tensor is missing: token_embd.weight", output)

    def test_vision_runtime_paths_fail_before_receiver_binding(self):
        env = os.environ.copy()
        env["HF_ENDPOINT"] = self.endpoint
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            python = root / "python"
            python.write_text("#!/bin/sh\n", encoding="utf-8")
            python.chmod(0o600)
            encoder = root / "encoder.py"
            encoder.write_text("# fixture\n", encoding="utf-8")
            encoder_dir = root / "encoder-dir"
            encoder_dir.mkdir()
            cases = (
                ("missing-python", [], "vision role python is missing"),
                ("non-executable-python",
                 ["--vision-python", str(python),
                  "--vision-encoder", str(encoder)],
                 "vision role python is not a trusted executable regular file"),
                ("non-regular-encoder",
                 ["--vision-python", "/bin/sh",
                  "--vision-encoder", str(encoder_dir)],
                 "vision role encoder is not a readable regular file"),
            )
            for name, extra, expected in cases:
                with self.subTest(case=name), tempfile.TemporaryDirectory() as cache:
                    env["DS4_LOCK_FILE"] = str(Path(cache) / "ds4.lock")
                    result = subprocess.run(
                        [str(ROOT / "ds4-server"), "--cpu", "--hf",
                         "owner/repo", "--hf-cache-dir", cache, *extra],
                        cwd=ROOT, env=env, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        timeout=10, check=False,
                    )
                    output = result.stdout + result.stderr
                    self.assertNotEqual(result.returncode, 0, output)
                    self.assertIn(expected, output)
                    self.assertNotIn("required tensor is missing", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
