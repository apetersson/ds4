#!/usr/bin/env python3

import hashlib
import json
import os
import subprocess
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests" / "fixtures" / "hf" / "variants-v2.json"
GOLDEN = ROOT / "tests" / "fixtures" / "hf"
PROBE = ROOT / "tests" / "test_hf_diagnostics_probe"
SHA = "0123456789abcdef0123456789abcdef01234567"
LOCK = "/tmp/ds4-ds00108-hf-diagnostics.lock"


PAYLOADS = {
    "receiver": b"receiver00",
    "ds4_vision.tower": b"tower",
    "ds4_vision.projector": b"project",
    "ds4_vision.config": b"cfg",
    "llama_cpp_mmproj": b"mmproj-data",
    "dspark": b"dspark-support",
}


def artifact_roles(variant):
    yield "receiver", variant["receiver"]
    yield "ds4_vision.tower", variant["ds4_vision"]["tower"]
    yield "ds4_vision.projector", variant["ds4_vision"]["projector"]
    yield "ds4_vision.config", variant["ds4_vision"]["config"]
    yield "llama_cpp_mmproj", variant["llama_cpp_mmproj"]
    if "dspark" in variant:
        yield "dspark", variant["dspark"]


class HubHandler(BaseHTTPRequestHandler):
    requests = []
    manifest = b""
    payloads = {}

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


class HFDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        manifest = json.loads(FIXTURE.read_text(encoding="utf-8"))
        manifest["repository"] = "owner/repo"
        payloads = {}
        for variant in manifest["variants"]:
            for role, artifact in artifact_roles(variant):
                payload = PAYLOADS[role]
                artifact["bytes"] = len(payload)
                artifact["sha256"] = hashlib.sha256(payload).hexdigest()
                payloads[artifact["path"]] = payload
        HubHandler.manifest = json.dumps(
            manifest, separators=(",", ":"), sort_keys=True
        ).encode()
        cls.full_manifest = manifest
        cls.full_manifest_bytes = HubHandler.manifest
        HubHandler.payloads = payloads
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), HubHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever,
                                      daemon=True)
        cls.thread.start()
        cls.endpoint = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        HubHandler.requests = []
        HubHandler.manifest = self.full_manifest_bytes
        self.cache = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.cache.cleanup()

    def run_binary(self, binary, *args, endpoint=None):
        env = os.environ.copy()
        for name in ("HF_TOKEN", "HF_TOKEN_PATH", "HF_HOME",
                     "XDG_CACHE_HOME"):
            env.pop(name, None)
        env["HF_ENDPOINT"] = endpoint or self.endpoint
        env["DS4_LOCK_FILE"] = LOCK
        return subprocess.run(
            [str(ROOT / binary), "--hf",
             "owner/repo:Headroom128-IQ2_XXS",
             "--hf-cache-dir", self.cache.name, *args],
            cwd=ROOT, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=10, check=False,
        )

    def test_dry_run_json_and_human_match_goldens_without_artifacts(self):
        for option, golden in (
            ((), "diagnostics-dry-run-human.txt"),
            (("--json",), "diagnostics-dry-run.json"),
        ):
            with self.subTest(golden=golden):
                HubHandler.requests = []
                result = self.run_binary("ds4", "--hf-dry-run", *option)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    result.stdout,
                    (GOLDEN / golden).read_text(encoding="utf-8"),
                )
                self.assertEqual(HubHandler.requests, [
                    "/api/models/owner/repo",
                    f"/owner/repo/resolve/{SHA}/variants.json",
                ])

    def test_listing_is_metadata_only_and_matches_json_and_human_goldens(self):
        outputs = {}
        for option, golden in (
            (("--json",), "diagnostics-list.json"),
            ((), "diagnostics-list-human.txt"),
        ):
            with self.subTest(golden=golden):
                HubHandler.requests = []
                result = self.run_binary("ds4", "--list-hf-variants", *option)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    result.stdout,
                    (GOLDEN / golden).read_text(encoding="utf-8"),
                )
                self.assertEqual(HubHandler.requests, [
                    "/api/models/owner/repo",
                    f"/owner/repo/resolve/{SHA}/variants.json",
                ])
                outputs[golden] = result.stdout

        result_stdout = outputs["diagnostics-list.json"]
        payload = json.loads(result_stdout)
        self.assertEqual(payload["schema_version"], 1)
        self.assertEqual(payload["mode"], "list-hf-variants")
        self.assertEqual(len(payload["variants"]), 2)
        variant = payload["variants"][0]
        self.assertIn("precision", variant["receiver"])
        self.assertIn("runtime_compatibility", variant["receiver"])
        self.assertIn("declared_capabilities", variant)
        self.assertTrue(variant["manifest_selection"])
        self.assertTrue(
            variant["llama_cpp_heuristics"]["primary_filename_match"]
        )
        self.assertTrue(
            variant["llama_cpp_heuristics"]["sibling_layout_match"]
        )
        for listed in payload["variants"]:
            receiver = listed["receiver"]["path"].lower()
            self.assertFalse(any(marker in receiver for marker in (
                "mmproj", "dspark", "support",
            )))

    def test_concurrent_metadata_publication_reuses_one_immutable_snapshot(self):
        env = os.environ.copy()
        env["HF_ENDPOINT"] = self.endpoint
        env["DS4_LOCK_FILE"] = LOCK
        command = [
            str(ROOT / "ds4"), "--hf",
            "owner/repo:Headroom128-IQ2_XXS", "--hf-cache-dir",
            self.cache.name, "--list-hf-variants", "--json",
        ]
        processes = [
            subprocess.Popen(command, cwd=ROOT, env=env, text=True,
                             stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
            for _ in range(2)
        ]
        results = [process.communicate(timeout=10) for process in processes]
        for process, (stdout, stderr) in zip(processes, results):
            self.assertEqual(process.returncode, 0, stderr)
            self.assertEqual(json.loads(stdout)["revision"], SHA)

    def test_runtime_totals_never_mix_raw_vision_and_mmproj(self):
        server = self.run_binary("ds4-server", "--hf-dry-run", "--json")
        self.assertEqual(server.returncode, 0, server.stderr)
        totals = json.loads(server.stdout)["totals"]
        self.assertEqual(totals, {
            "transfer_bytes": 25,
            "selected_runtime_weight_bytes": 22,
            "receiver_only_bytes": 10,
            "ds4_receiver_vision_bytes": 22,
            "ds4_receiver_vision_dspark_bytes": 36,
            "llama_cpp_receiver_mmproj_bytes": 21,
        })
        with_dspark = self.run_binary(
            "ds4-server", "--hf-dry-run", "--dspark", "--json"
        )
        self.assertEqual(with_dspark.returncode, 0, with_dspark.stderr)
        totals = json.loads(with_dspark.stdout)["totals"]
        self.assertEqual(totals["transfer_bytes"], 39)
        self.assertEqual(totals["selected_runtime_weight_bytes"], 36)
        self.assertEqual(totals["llama_cpp_receiver_mmproj_bytes"], 21)

    def test_variant_without_dspark_reports_unavailable_bundle_total(self):
        manifest = json.loads(json.dumps(self.full_manifest))
        del manifest["variants"][0]["dspark"]
        HubHandler.manifest = json.dumps(
            manifest, separators=(",", ":"), sort_keys=True
        ).encode()
        for option, golden in (
            (("--json",), "diagnostics-no-dspark.json"),
            ((), "diagnostics-no-dspark-human.txt"),
        ):
            with self.subTest(golden=golden):
                result = self.run_binary("ds4-server", "--hf-dry-run", *option)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(
                    result.stdout,
                    (GOLDEN / golden).read_text(encoding="utf-8"),
                )
        listing = self.run_binary("ds4", "--list-hf-variants")
        self.assertEqual(listing.returncode, 0, listing.stderr)
        self.assertEqual(
            listing.stdout,
            (GOLDEN / "diagnostics-list-no-dspark-human.txt").read_text(
                encoding="utf-8"
            ),
        )

    def test_offline_uses_only_complete_verified_requested_snapshot(self):
        online = self.run_binary("ds4", "--list-hf-variants", "--json")
        self.assertEqual(online.returncode, 0, online.stderr)
        HubHandler.requests = []
        offline_list = self.run_binary(
            "ds4", "--list-hf-variants", "--offline", "--json",
            endpoint="http://127.0.0.1:1",
        )
        self.assertEqual(offline_list.returncode, 0, offline_list.stderr)
        self.assertEqual(json.loads(offline_list.stdout)["metadata_source"],
                         "cache")
        self.assertEqual(HubHandler.requests, [])

        missing = self.run_binary(
            "ds4", "--hf-dry-run", "--offline", "--json",
            endpoint="http://127.0.0.1:1",
        )
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("complete verified snapshot", missing.stderr)
        self.assertEqual(HubHandler.requests, [])

        populate_env = os.environ.copy()
        populate_env["DS4_LOCK_FILE"] = LOCK
        populated = subprocess.run(
            [str(PROBE), self.endpoint, self.cache.name,
             "populate-server-dspark"],
            cwd=ROOT, env=populate_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=10, check=False,
        )
        self.assertEqual(populated.returncode, 0, populated.stderr)
        HubHandler.requests = []
        complete = self.run_binary(
            "ds4-server", "--hf-dry-run", "--dspark", "--hf-offline",
            "--json", endpoint="http://127.0.0.1:1",
        )
        self.assertEqual(complete.returncode, 0, complete.stderr)
        payload = json.loads(complete.stdout)
        self.assertEqual(payload["metadata_source"], "cache")
        selected = {entry["role"]: entry for entry in payload["files"]}
        self.assertEqual(selected["receiver"]["cache"], "cached")
        self.assertEqual(selected["ds4_vision.tower"]["cache"], "cached")
        self.assertEqual(selected["dspark"]["cache"], "cached")
        self.assertFalse(selected["llama_cpp_mmproj"]["selected"])
        self.assertEqual(HubHandler.requests, [])

        invalid_mmproj = subprocess.run(
            [str(PROBE), self.endpoint, self.cache.name,
             "populate-invalid-mmproj"],
            cwd=ROOT, env=populate_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=10, check=False,
        )
        self.assertEqual(invalid_mmproj.returncode, 0,
                         invalid_mmproj.stderr)
        HubHandler.requests = []
        receiver_only = self.run_binary(
            "ds4", "--hf-dry-run", "--hf-offline", "--json",
            endpoint="http://127.0.0.1:1",
        )
        self.assertEqual(receiver_only.returncode, 0, receiver_only.stderr)
        files = {entry["role"]: entry
                 for entry in json.loads(receiver_only.stdout)["files"]}
        self.assertEqual(files["receiver"]["cache"], "cached")
        self.assertEqual(files["llama_cpp_mmproj"]["cache"], "missing")
        self.assertEqual(HubHandler.requests, [])

        invalid_dspark = subprocess.run(
            [str(PROBE), self.endpoint, self.cache.name, "corrupt-dspark"],
            cwd=ROOT, env=populate_env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=10, check=False,
        )
        self.assertEqual(invalid_dspark.returncode, 0,
                         invalid_dspark.stderr)
        HubHandler.requests = []
        requested_invalid = self.run_binary(
            "ds4-server", "--hf-dry-run", "--dspark", "--hf-offline",
            "--json", endpoint="http://127.0.0.1:1",
        )
        self.assertNotEqual(requested_invalid.returncode, 0)
        self.assertIn("complete verified immutable role snapshot",
                      requested_invalid.stderr)
        self.assertEqual(HubHandler.requests, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
