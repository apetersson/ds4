#!/usr/bin/env python3

import copy
import hashlib
import json
import os
import struct
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

from test_hf_runtime import build_tokenizer_gguf


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "hf" / "variants-v2.json"
SHA = "0123456789abcdef0123456789abcdef01234567"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def build_dspark_gguf(name):
    """Build a tiny inspectable GGUF whose tensor names identify DSpark."""
    uint32 = 4
    string = 8
    array = 9

    def gguf_string(value):
        encoded = value.encode("utf-8")
        return struct.pack("<Q", len(encoded)) + encoded

    entries = []

    def scalar(key, kind, value):
        payload = (struct.pack("<I", value) if kind == uint32
                   else gguf_string(value))
        entries.append(gguf_string(key) + struct.pack("<I", kind) + payload)

    scalar("general.name", string, name)
    scalar("deepseek4.dspark.block_size", uint32, 5)
    scalar("deepseek4.dspark.markov_rank", uint32, 8)
    scalar("deepseek4.dspark.noise_token_id", uint32, 1)
    target_layers = struct.pack("<IQI", uint32, 1, 0)
    entries.append(gguf_string("deepseek4.dspark.target_layer_ids") +
                   struct.pack("<I", array) + target_layers)

    names = [
        "mtp.0.main_proj.weight",
        "mtp.2.markov_head.markov_w1.weight",
        "mtp.2.confidence_head.proj.weight",
    ]
    tensors = []
    for index, tensor_name in enumerate(names):
        tensors.append(
            gguf_string(tensor_name) + struct.pack("<IQIQ", 1, 1, 0, index * 4)
        )

    header = b"GGUF" + struct.pack("<IQQ", 3, len(tensors), len(entries))
    metadata = header + b"".join(entries) + b"".join(tensors)
    padding = b"\0" * ((32 - len(metadata) % 32) % 32)
    return metadata + padding + b"\0" * (4 * len(tensors))


class HubHandler(BaseHTTPRequestHandler):
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
            self.send_bytes(200, json.dumps({"sha": SHA}).encode(),
                            "application/json")
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


class HFDsparkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), HubHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever,
                                      daemon=True)
        cls.thread.start()
        cls.endpoint = f"http://127.0.0.1:{cls.server.server_port}"
        cls.receiver = build_tokenizer_gguf()
        cls.target_dspark = build_dspark_gguf("selected-target-support")
        cls.other_dspark = build_dspark_gguf("cross-variant-support")

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        self.cache = tempfile.TemporaryDirectory()
        self.manifest = copy.deepcopy(json.loads(FIXTURE.read_text("utf-8")))
        self.manifest["repository"] = "owner/repo"
        target = self.manifest["variants"][0]
        target["receiver"]["bytes"] = len(self.receiver)
        target["receiver"]["sha256"] = sha256(self.receiver)
        target["dspark"]["bytes"] = len(self.target_dspark)
        target["dspark"]["sha256"] = sha256(self.target_dspark)
        other = self.manifest["variants"][1]
        other["dspark"]["bytes"] = len(self.other_dspark)
        other["dspark"]["sha256"] = sha256(self.other_dspark)
        self.target = target
        self.other = other
        self.payloads = {
            target["receiver"]["path"]: self.receiver,
            target["dspark"]["path"]: self.target_dspark,
            other["dspark"]["path"]: self.other_dspark,
            target["directory"] + "/dspark-decoy-Q4.gguf":
                build_dspark_gguf("same-directory-decoy"),
        }
        self.publish()

    def tearDown(self):
        self.cache.cleanup()

    def publish(self):
        HubHandler.requests = []
        HubHandler.payloads = dict(self.payloads)
        HubHandler.manifest = json.dumps(self.manifest).encode()

    def env(self):
        env = os.environ.copy()
        for name in ("HF_TOKEN", "HF_TOKEN_PATH", "HF_HOME",
                     "XDG_CACHE_HOME"):
            env.pop(name, None)
        env["HF_ENDPOINT"] = self.endpoint
        env["DS4_LOCK_FILE"] = str(Path(self.cache.name) / "ds4.lock")
        return env

    def run_ds4(self, *args, timeout=10):
        return subprocess.run(
            [str(ROOT / "ds4"), *args], cwd=ROOT, env=self.env(), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout,
            check=False,
        )

    def hf_args(self):
        return ["--hf", "owner/repo:Headroom128-IQ2_XXS",
                "--hf-cache-dir", self.cache.name]

    def artifact_requests(self):
        prefix = f"/owner/repo/resolve/{SHA}/"
        return [path.removeprefix(prefix) for path in HubHandler.requests
                if path.startswith(prefix) and not path.endswith("variants.json")]

    def test_target_only_parity_does_not_download_or_map_dspark(self):
        common = ["--dump-tokens", "-p", "parity"]
        by_hf = self.run_ds4(*self.hf_args(), *common)
        self.assertEqual(by_hf.returncode, 0, by_hf.stdout + by_hf.stderr)
        self.assertEqual(self.artifact_requests(),
                         [self.target["receiver"]["path"]])
        self.assertIn("verified_roles=[receiver]", by_hf.stderr)
        self.assertIn("dspark=not-requested support=none", by_hf.stderr)

        receiver_path = next(
            path for path in Path(self.cache.name).rglob("*.gguf")
            if "dspark-" not in path.name
        )
        local = self.run_ds4("--model", str(receiver_path), *common)
        self.assertEqual(local.returncode, 0, local.stdout + local.stderr)
        self.assertEqual(by_hf.stdout, local.stdout)

    def test_exact_selected_companion_starts_amid_decoys(self):
        result = self.run_ds4("--cpu", "--inspect", *self.hf_args(),
                              "--dspark")
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertEqual(self.artifact_requests(), [
            self.target["receiver"]["path"], self.target["dspark"]["path"],
        ])
        requests = self.artifact_requests()
        self.assertNotIn(self.other["dspark"]["path"], requests)
        self.assertFalse(any("dspark-decoy-Q4.gguf" in path
                             for path in requests))
        self.assertIn("verified_roles=[receiver,dspark]", output)
        self.assertIn("dspark=requested support=catalog", output)
        self.assertIn("support model (DSpark, stages=3)", output)

    def test_absent_dspark_fails_before_receiver_acquisition(self):
        self.target.pop("dspark")
        self.publish()
        result = self.run_ds4(*self.hf_args(), "--dspark", "--dump-tokens",
                              "-p", "unused")
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("selected variant has no DSpark role", output)
        self.assertEqual(self.artifact_requests(), [])
        self.assertNotIn("HF repository=", output)

    def test_dspark_hash_mismatch_fails_before_receiver_mapping(self):
        self.payloads[self.target["dspark"]["path"]] = \
            self.target_dspark[:-1] + b"x"
        self.publish()
        result = self.run_ds4("--cpu", "--inspect", *self.hf_args(),
                              "--dspark")
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("downloaded bytes fail manifest SHA-256", output)
        self.assertNotIn("HF repository=", output)
        self.assertNotIn("model:", output)

    def test_cross_variant_dspark_substitution_fails_closed(self):
        self.payloads[self.target["dspark"]["path"]] = self.other_dspark
        self.publish()
        result = self.run_ds4("--cpu", "--inspect", *self.hf_args(),
                              "--dspark")
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("downloaded bytes fail manifest", output)
        self.assertEqual(self.artifact_requests(), [
            self.target["receiver"]["path"], self.target["dspark"]["path"],
        ])
        self.assertNotIn("HF repository=", output)

    def test_ssd_streaming_is_rejected_before_network(self):
        result = self.run_ds4(*self.hf_args(), "--dspark", "--ssd-streaming")
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("not compatible with --ssd-streaming", output)
        self.assertEqual(HubHandler.requests, [])

    def test_explicit_mtp_precedence_skips_catalog_support(self):
        explicit = Path(self.cache.name) / "explicit-dspark.gguf"
        explicit.write_bytes(build_dspark_gguf("explicit-local-support"))
        result = self.run_ds4("--cpu", "--inspect", *self.hf_args(),
                              "--dspark", "--mtp", str(explicit))
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertEqual(self.artifact_requests(),
                         [self.target["receiver"]["path"]])
        self.assertIn("verified_roles=[receiver]", output)
        self.assertIn("dspark=requested support=explicit-mtp", output)
        self.assertIn("support model (DSpark, stages=3)", output)

    def test_existing_dspark_controls_reach_discovered_startup(self):
        result = self.run_ds4(
            "--cpu", "--inspect", *self.hf_args(),
            "--dspark-confidence", "0.4", "--dspark-strict",
            "--mtp-exact-sampling", "--mtp-draft", "5",
            "--mtp-margin", "2.0",
        )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("verified_roles=[receiver,dspark]", output)
        self.assertIn("support model (DSpark, stages=3)", output)

    def test_server_receives_verified_catalog_support_before_model_binding(self):
        result = subprocess.run(
            [str(ROOT / "ds4-server"), "--cpu", *self.hf_args(),
             "--no-vision", "--dspark"],
            cwd=ROOT, env=self.env(), text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=10, check=False,
        )
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0, output)
        self.assertIn("verified_roles=[receiver,dspark]", output)
        self.assertIn("dspark=requested support=catalog", output)
        self.assertIn("required tensor is missing: token_embd.weight", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
