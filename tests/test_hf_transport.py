#!/usr/bin/env python3

import json
import os
import socket
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests" / "test_hf_transport_probe"
SHA_A = "0123456789abcdef0123456789abcdef01234567"
SHA_B = "89abcdef0123456789abcdef0123456789abcdef"
SHA_TAG = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
SECRET = "deterministic-test-secret"


class FakeHubHandler(BaseHTTPRequestHandler):
    current_sha = SHA_A
    requests = []

    def log_message(self, _format, *_args):
        pass

    def send_json(self, status, payload, error_code=None):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if error_code:
            self.send_header("X-Error-Code", error_code)
        self.end_headers()
        try:
            self.wfile.write(body)
        except BrokenPipeError:
            pass

    def do_GET(self):
        path = unquote(urlsplit(self.path).path)
        authorization = self.headers.get("Authorization")
        type(self).requests.append((path, authorization))

        if path == "/api/models/owner/timeout":
            time.sleep(0.35)
            self.send_json(200, {"sha": SHA_A})
        elif path == "/api/models/owner/private":
            self.send_json(403, {"error": "gated"}, "GatedRepo")
        elif path == "/api/models/owner/missing":
            self.send_json(404, {"error": "missing"}, "RepoNotFound")
        elif path == "/api/models/owner/auth":
            self.send_json(401, {"error": "bad token"})
        elif path == "/api/models/owner/malformed":
            self.send_json(200, {"nested": {"sha": SHA_A}})
        elif path == "/api/models/owner/protected":
            if authorization == f"Bearer {SECRET}":
                self.send_json(200, {"sha": SHA_A})
            else:
                self.send_json(401, {"error": "authentication required"})
        elif path == "/api/models/owner/repo":
            self.send_json(200, {"sha": type(self).current_sha, "siblings": []})
        elif path == "/api/models/owner/repo/revision/main":
            self.send_json(200, {"sha": type(self).current_sha})
        elif path == "/api/models/owner/repo/revision/v1.0":
            self.send_json(200, {"sha": SHA_TAG})
        elif path == "/api/models/owner/repo/revision/refs/pr/17":
            self.send_json(200, {"sha": SHA_TAG})
        elif path == f"/api/models/owner/repo/revision/{SHA_A}":
            self.send_json(200, {"sha": SHA_A})
        elif path == "/api/models/owner/repo/revision/missing":
            self.send_json(404, {"error": "missing revision"}, "RevisionNotFound")
        else:
            self.send_json(404, {"error": "unexpected path"}, "RepoNotFound")


class TransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.empty_home = tempfile.TemporaryDirectory()
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), FakeHubHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.endpoint = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.empty_home.cleanup()

    def setUp(self):
        FakeHubHandler.current_sha = SHA_A
        FakeHubHandler.requests = []

    def run_probe(self, repo="owner/repo", revision="-", timeout_ms=1000,
                  extra_env=None, endpoint=None):
        env = os.environ.copy()
        for name in ("HF_TOKEN", "HF_TOKEN_PATH", "HF_HOME", "XDG_CACHE_HOME"):
            env.pop(name, None)
        env["HOME"] = self.empty_home.name
        env["HF_ENDPOINT"] = endpoint or self.endpoint
        if extra_env:
            env.update(extra_env)
        return subprocess.run(
            [str(PROBE), repo, revision, str(timeout_ms)],
            cwd=ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    @staticmethod
    def values(result):
        return dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)

    def test_default_revision_is_immutable_and_all_urls_are_pinned(self):
        first = self.run_probe()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        values = self.values(first)
        self.assertEqual(values["commit"], SHA_A)
        self.assertEqual(
            values["manifest_url"],
            f"{self.endpoint}/owner/repo/resolve/{SHA_A}/variants.json",
        )
        self.assertEqual(
            values["receiver_url"],
            f"{self.endpoint}/owner/repo/resolve/{SHA_A}/nested/model.gguf",
        )

        FakeHubHandler.current_sha = SHA_B
        second = self.run_probe()
        self.assertEqual(self.values(second)["commit"], SHA_B)
        self.assertIn(f"/resolve/{SHA_A}/", values["manifest_url"])
        self.assertIn(f"/resolve/{SHA_A}/", values["receiver_url"])
        self.assertTrue(all(path.startswith("/api/models/") for path, _ in FakeHubHandler.requests))

    def test_branch_tag_commit_like_revision_and_slashes_resolve_canonically(self):
        for revision, expected in (("main", SHA_A), ("v1.0", SHA_TAG),
                                   ("refs/pr/17", SHA_TAG), (SHA_A, SHA_A)):
            with self.subTest(revision=revision):
                result = self.run_probe(revision=revision)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.values(result)["commit"], expected)

    def test_hf_token_and_existing_hf_home_auth_do_not_leak(self):
        by_environment = self.run_probe(
            repo="owner/protected", extra_env={"HF_TOKEN": SECRET}
        )
        self.assertEqual(by_environment.returncode, 0, by_environment.stdout + by_environment.stderr)
        self.assertNotIn(SECRET, by_environment.stdout + by_environment.stderr)

        with tempfile.TemporaryDirectory() as hf_home:
            Path(hf_home, "token").write_text(SECRET + "\n", encoding="utf-8")
            by_existing_auth = self.run_probe(
                repo="owner/protected", extra_env={"HF_HOME": hf_home}
            )
        self.assertEqual(by_existing_auth.returncode, 0,
                         by_existing_auth.stdout + by_existing_auth.stderr)
        self.assertNotIn(SECRET, by_existing_auth.stdout + by_existing_auth.stderr)
        protected = [entry for entry in FakeHubHandler.requests
                     if entry[0] == "/api/models/owner/protected"]
        self.assertEqual([header for _, header in protected],
                         [f"Bearer {SECRET}", f"Bearer {SECRET}"])
        self.assertTrue(all(SECRET not in path for path, _ in FakeHubHandler.requests))

    def test_configured_xdg_cache_does_not_fall_back_to_legacy_home_token(self):
        with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as xdg:
            legacy = Path(home, ".cache", "huggingface")
            legacy.mkdir(parents=True)
            Path(legacy, "token").write_text(SECRET + "\n", encoding="utf-8")
            result = self.run_probe(
                repo="owner/protected",
                extra_env={"HOME": home, "XDG_CACHE_HOME": xdg},
            )
        self.assertEqual(self.values(result)["status"], "authentication_failed")
        self.assertNotIn(SECRET, result.stdout + result.stderr)
        self.assertEqual(FakeHubHandler.requests,
                         [("/api/models/owner/protected", None)])

    def test_failure_classes_have_distinct_statuses_and_safe_diagnostics(self):
        cases = (
            ("owner/private", "-", 1000, "private_or_gated"),
            ("owner/missing", "-", 1000, "repository_not_found"),
            ("owner/repo", "missing", 1000, "revision_not_found"),
            ("owner/auth", "-", 1000, "authentication_failed"),
            ("owner/malformed", "-", 1000, "malformed_response"),
            ("owner/timeout", "-", 75, "timeout"),
        )
        diagnostics = set()
        for repo, revision, timeout_ms, expected in cases:
            with self.subTest(expected=expected):
                result = self.run_probe(repo, revision, timeout_ms,
                                        extra_env={"HF_TOKEN": SECRET})
                values = self.values(result)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(values["status"], expected)
                self.assertNotIn(SECRET, result.stdout + result.stderr)
                diagnostics.add(values["diagnostic"])
        self.assertEqual(len(diagnostics), len(cases))

        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        unused_port = sock.getsockname()[1]
        sock.close()
        network = self.run_probe(endpoint=f"http://127.0.0.1:{unused_port}", timeout_ms=200)
        self.assertEqual(self.values(network)["status"], "network_failed")

    def test_endpoint_is_used_for_resolution_manifest_and_artifact_urls(self):
        result = self.run_probe(revision="v1.0")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        values = self.values(result)
        self.assertTrue(values["manifest_url"].startswith(self.endpoint + "/"))
        self.assertTrue(values["receiver_url"].startswith(self.endpoint + "/"))
        paths = [path for path, _ in FakeHubHandler.requests]
        self.assertEqual(paths, [
            "/api/models/owner/repo",
            "/api/models/owner/repo/revision/v1.0",
        ])


if __name__ == "__main__":
    unittest.main(verbosity=2)
