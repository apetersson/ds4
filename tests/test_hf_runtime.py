#!/usr/bin/env python3

import hashlib
import json
import os
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
            variant["receiver"]["path"]: b"byte-identical-receiver-gguf-fixture",
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

    def test_both_binaries_handoff_the_verified_receiver_before_model_open(self):
        env = os.environ.copy()
        env["HF_ENDPOINT"] = self.endpoint
        for name in ("HF_TOKEN", "HF_TOKEN_PATH", "HF_HOME", "XDG_CACHE_HOME"):
            env.pop(name, None)
        for binary, extra, expected_roles in (
            ("ds4", ["--inspect"], "verified_roles=[receiver] vision=inactive"),
            ("ds4-server", [],
             "verified_roles=[receiver,ds4_vision.tower,ds4_vision.projector,ds4_vision.config] vision=inactive"),
        ):
            with self.subTest(binary=binary), tempfile.TemporaryDirectory() as cache:
                result = subprocess.run(
                    [str(ROOT / binary), "--cpu", "--hf", "owner/repo",
                     "--hf-cache-dir", cache, *extra],
                    cwd=ROOT,
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=10,
                    check=False,
                )
                output = result.stdout + result.stderr
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn("repository='owner/repo'", output)
                self.assertIn(f"revision='{SHA}'", output)
                self.assertIn("selector='Headroom128-IQ2_XXS'", output)
                self.assertIn("receiver='Headroom128-IQ2_XXS/", output)
                self.assertIn(expected_roles, output)
                self.assertIn("model is not a GGUF file", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
