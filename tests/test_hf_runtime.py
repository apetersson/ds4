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
            variant["ds4_vision"]["tower"]["path"]: b"verified-tower",
            variant["ds4_vision"]["projector"]["path"]: b"verified-projector",
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
        RuntimeHubHandler.payloads = payloads
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
        self.cache = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.cache.cleanup()

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
        with tempfile.TemporaryDirectory() as cache:
            env["DS4_LOCK_FILE"] = str(Path(cache) / "ds4.lock")
            result = subprocess.run(
                [str(ROOT / "ds4-server"), "--cpu", "--hf", "owner/repo",
                 "--hf-cache-dir", cache],
                cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=10, check=False,
            )
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("repository='owner/repo'", output)
        self.assertIn(f"revision='{SHA}'", output)
        self.assertIn("verified_roles=[receiver,ds4_vision.tower,ds4_vision.projector,ds4_vision.config] vision=inactive", output)
        self.assertIn("dspark=not-requested", output)
        self.assertIn("required tensor is missing: token_embd.weight", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
