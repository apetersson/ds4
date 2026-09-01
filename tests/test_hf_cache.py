#!/usr/bin/env python3

import os
import socket
import subprocess
import tempfile
import threading
import time
import unicodedata
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests" / "test_hf_cache_probe"
SHA = "0123456789abcdef0123456789abcdef01234567"
SECRET = "cache-test-secret"


def cache_identity_path(value):
    encoded = value.encode("utf-8").hex()
    return Path(*(encoded[offset:offset + 64]
                  for offset in range(0, len(encoded), 64)))


def payload_for(path):
    if path.endswith("H-sha256-multiblock.gguf"):
        size = 131073
        return (path.encode("utf-8") + b"#" * size)[:size]
    sizes = {
        "receiver.gguf": 19,
        "tower.safetensors": 17,
        "projector.safetensors": 21,
        "config.json": 16,
        ".gguf": None,
    }
    if "mmproj-" in path:
        size = 18
    elif "dspark-" in path:
        size = 20
    else:
        size = next(size for suffix, size in sizes.items()
                    if size is not None and path.endswith(suffix))
    return (path.encode("utf-8") + b"#" * size)[:size]


class FakeArtifactHub(BaseHTTPRequestHandler):
    requests = []
    interrupt_path = None
    interrupted = False
    fail_path = None
    delay = 0
    range_supported = True
    cas_endpoint = None

    def log_message(self, _format, *_args):
        pass

    @classmethod
    def reset(cls):
        cls.requests = []
        cls.interrupt_path = None
        cls.interrupted = False
        cls.fail_path = None
        cls.delay = 0
        cls.range_supported = True

    def do_GET(self):
        path = unquote(urlsplit(self.path).path)
        range_header = self.headers.get("Range")
        authorization = self.headers.get("Authorization")
        type(self).requests.append((path, range_header, authorization))

        prefixes = (
            f"/owner/repo/resolve/{SHA}/",
            f"/Owner/Repo/resolve/{SHA}/",
        )
        prefix = next((candidate for candidate in prefixes
                       if path.startswith(candidate)), None)
        if prefix:
            repo_path = path[len(prefix):]
            if repo_path == type(self).fail_path:
                self.send_error(503, "deterministic failure")
                return
            self.send_response(302)
            self.send_header(
                "Location",
                f"{type(self).cas_endpoint}/cas/{quote(repo_path)}?signed=xet-lfs",
            )
            self.end_headers()
            return
        if not path.startswith("/cas/"):
            self.send_error(404)
            return

        repo_path = path[len("/cas/"):]
        data = payload_for(repo_path)
        if type(self).delay:
            time.sleep(type(self).delay)
        if (repo_path == type(self).interrupt_path and
                not type(self).interrupted and not range_header):
            type(self).interrupted = True
            split = len(data) // 2
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data[:split])
            self.wfile.flush()
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.connection.close()
            return

        offset = 0
        if range_header and type(self).range_supported:
            self.assert_range(range_header, len(data))
            offset = int(range_header[6:-1])
            self.send_response(206)
            self.send_header("Content-Range",
                             f"bytes {offset}-{len(data) - 1}/{len(data)}")
        else:
            self.send_response(200)
        body = data[offset:]
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def assert_range(self, value, length):
        if not (value.startswith("bytes=") and value.endswith("-")):
            self.send_error(400)
            raise AssertionError(value)
        offset = int(value[6:-1])
        if not 0 <= offset < length:
            self.send_error(416)
            raise AssertionError(value)


class CacheTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cas_server = ThreadingHTTPServer(("127.0.0.1", 0), FakeArtifactHub)
        cls.cas_thread = threading.Thread(
            target=cls.cas_server.serve_forever, daemon=True)
        cls.cas_thread.start()
        FakeArtifactHub.cas_endpoint = (
            f"http://localhost:{cls.cas_server.server_port}")
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), FakeArtifactHub)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.endpoint = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.cas_server.shutdown()
        cls.cas_server.server_close()
        cls.cas_thread.join()

    def setUp(self):
        FakeArtifactHub.reset()

    def probe_command(self, cache, selector="Headroom128", modes="text"):
        return [str(PROBE), str(cache), self.endpoint, selector, modes, "3000"]

    def run_probe(self, cache, selector="Headroom128", modes="text",
                  extra_env=None):
        env = os.environ.copy()
        for name in ("HF_TOKEN", "HF_TOKEN_PATH", "HF_HOME", "XDG_CACHE_HOME"):
            env.pop(name, None)
        if extra_env:
            env.update(extra_env)
        return subprocess.run(
            self.probe_command(cache, selector, modes), cwd=ROOT, env=env,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False,
        )

    @staticmethod
    def values(result, prefix):
        return [line.split("=", 1)[1] for line in result.stdout.splitlines()
                if line.startswith(prefix + "=")]

    @staticmethod
    def requested_paths():
        prefix = f"/owner/repo/resolve/{SHA}/"
        return [path[len(prefix):] for path, _, _ in FakeArtifactHub.requests
                if path.startswith(prefix)]

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("status=ok", result.stdout)

    def test_download_progress_reports_role_rate_eta_and_verification(self):
        with tempfile.TemporaryDirectory() as cache:
            result = self.run_probe(cache)
            self.assert_ok(result)
            self.assertIn("HF download [1/1] receiver:", result.stderr)
            self.assertIn("100.0%", result.stderr)
            self.assertIn("/s, ETA", result.stderr)
            self.assertIn("HF verify [1/1] receiver: checking SHA-256",
                          result.stderr)
            self.assertIn("HF cached [1/1] receiver: verified and ready",
                          result.stderr)

    def test_runtime_role_selection_is_exact_and_mmproj_stays_metadata_only(self):
        cases = (
            ("text", {"H-receiver.gguf"}),
            ("vision", {"H-receiver.gguf", "H-tower.safetensors",
                         "H-projector.safetensors", "H-config.json"}),
            ("text,dspark", {"H-receiver.gguf", "dspark-H.gguf"}),
            ("vision,dspark", {"H-receiver.gguf", "H-tower.safetensors",
                                "H-projector.safetensors", "H-config.json",
                                "dspark-H.gguf"}),
            ("text,materialize", {"H-receiver.gguf", "mmproj-H.gguf"}),
        )
        for modes, basenames in cases:
            with self.subTest(modes=modes), tempfile.TemporaryDirectory() as cache:
                FakeArtifactHub.reset()
                result = self.run_probe(cache, modes=modes)
                self.assert_ok(result)
                self.assertEqual({Path(path).name for path in self.requested_paths()},
                                 basenames)
                self.assertFalse(any(path.startswith("Quality128/")
                                     for path in self.requested_paths()))
                plans = self.values(result, "plan")
                mmproj = next(value for value in plans
                              if value.startswith("llama_cpp_mmproj|"))
                self.assertEqual(mmproj.split("|")[2],
                                 "1" if "materialize" in modes else "0")

    def test_quality_selector_never_fetches_headroom_or_unrelated_paths(self):
        with tempfile.TemporaryDirectory() as cache:
            result = self.run_probe(cache, selector="Quality128", modes="vision")
            self.assert_ok(result)
            paths = self.requested_paths()
            self.assertTrue(paths)
            self.assertTrue(all(path.startswith("Quality128/") for path in paths))
            self.assertFalse(any("mmproj" in path or "dspark" in path
                                 for path in paths))

    def test_cache_precedence_spaces_and_identity_sidecars(self):
        with tempfile.TemporaryDirectory() as base:
            configured = Path(base, "configured cache with spaces")
            hf_home = Path(base, "ignored hf home")
            result = self.run_probe(
                configured, extra_env={"HF_HOME": str(hf_home)})
            self.assert_ok(result)
            self.assertEqual(self.values(result, "cache_root"), [str(configured)])
            plan = self.values(result, "plan")[0].split("|")
            destination = Path(plan[4])
            parts = destination.relative_to(configured).parts
            repo_index = parts.index("repos")
            snapshot_index = parts.index("snapshots")
            self.assertEqual("".join(parts[repo_index + 1:snapshot_index]),
                             "owner/repo".encode("utf-8").hex())
            self.assertIn(SHA, str(destination))
            metadata = Path(str(destination) + ".ds4-meta").read_text()
            self.assertIn("repository=owner/repo\n", metadata)
            self.assertIn(f"revision={SHA}\n", metadata)
            self.assertIn("selector=Headroom128\n", metadata)
            self.assertIn("role=receiver\n", metadata)

            FakeArtifactHub.reset()
            env_result = self.run_probe(
                "-", extra_env={"HF_HOME": str(hf_home),
                                "XDG_CACHE_HOME": str(Path(base, "ignored xdg")),
                                "HOME": str(Path(base, "ignored home"))})
            self.assert_ok(env_result)
            self.assertEqual(self.values(env_result, "cache_root"),
                             [str(hf_home / "ds4")])

            FakeArtifactHub.reset()
            xdg = Path(base, "xdg cache with spaces")
            xdg_result = self.run_probe(
                "-", extra_env={"XDG_CACHE_HOME": str(xdg),
                                "HOME": str(Path(base, "ignored home"))})
            self.assert_ok(xdg_result)
            self.assertEqual(self.values(xdg_result, "cache_root"),
                             [str(xdg / "huggingface" / "ds4")])

            FakeArtifactHub.reset()
            home = Path(base, "default home with spaces")
            home_result = self.run_probe("-", extra_env={"HOME": str(home)})
            self.assert_ok(home_result)
            self.assertEqual(self.values(home_result, "cache_root"),
                             [str(home / ".cache" / "huggingface" / "ds4")])

    def test_case_and_unicode_cache_identities_remain_distinct_offline(self):
        cases = (
            ("text", "owner/repo", "Headroom128/H-receiver.gguf"),
            ("text,repo-case", "Owner/Repo", "Headroom128/H-receiver.gguf"),
            ("text,artifact-case", "owner/repo", "Headroom128/h-receiver.gguf"),
            ("text,artifact-composed", "owner/repo",
             "Headroom128/caf\u00e9-receiver.gguf"),
            ("text,artifact-decomposed", "owner/repo",
             "Headroom128/cafe\u0301-receiver.gguf"),
        )
        with tempfile.TemporaryDirectory() as cache:
            destinations = []
            for modes, repository, repo_path in cases:
                with self.subTest(mode="populate", modes=modes):
                    FakeArtifactHub.reset()
                    result = self.run_probe(cache, modes=modes)
                    self.assert_ok(result)
                    receiver = next(
                        value.split("|") for value in self.values(result, "plan")
                        if value.startswith("receiver|")
                    )
                    self.assertEqual(receiver[1], repo_path)
                    destination = Path(receiver[4])
                    self.assertTrue(destination.is_file())
                    destinations.append(destination.relative_to(cache).as_posix())
                    metadata = Path(str(destination) + ".ds4-meta").read_text()
                    self.assertIn(f"repository={repository}\n", metadata)
                    self.assertIn(f"path={repo_path}\n", metadata)

            normalized = {
                unicodedata.normalize("NFD", path).casefold()
                for path in destinations
            }
            self.assertEqual(len(normalized), len(cases))

            for modes, _repository, _repo_path in cases:
                with self.subTest(mode="offline", modes=modes):
                    FakeArtifactHub.reset()
                    result = self.run_probe(cache, modes=f"{modes},offline")
                    self.assert_ok(result)
                    self.assertEqual(FakeArtifactHub.requests, [])

    def test_interrupted_redirected_transfer_resumes_without_public_partial(self):
        repo_path = "Headroom128/H-receiver.gguf"
        FakeArtifactHub.interrupt_path = repo_path
        with tempfile.TemporaryDirectory() as cache:
            first = self.run_probe(cache)
            self.assertEqual(first.returncode, 2, first.stdout + first.stderr)
            plan = self.values(first, "plan")[0].split("|")
            destination = Path(plan[4])
            partial = Path(str(destination) + ".part")
            self.assertFalse(destination.exists())
            self.assertEqual(partial.stat().st_size, 9)
            self.assertIn("repository='owner/repo'", first.stdout)
            self.assertIn(f"revision='{SHA}'", first.stdout)
            self.assertIn("selector='Headroom128'", first.stdout)
            self.assertIn("role='receiver'", first.stdout)
            self.assertIn("expected_size=19", first.stdout)
            self.assertIn(f"destination='{destination}'", first.stdout)

            second = self.run_probe(cache)
            self.assert_ok(second)
            self.assertIn("resuming at 9 B", second.stderr)
            self.assertEqual(destination.read_bytes(), payload_for(repo_path))
            self.assertFalse(partial.exists())
            self.assertTrue(any(value == "bytes=9-" for _, value, _
                                in FakeArtifactHub.requests))

    def test_range_ignored_after_interruption_restarts_safely(self):
        repo_path = "Headroom128/H-receiver.gguf"
        FakeArtifactHub.interrupt_path = repo_path
        with tempfile.TemporaryDirectory() as cache:
            self.assertEqual(self.run_probe(cache).returncode, 2)
            FakeArtifactHub.range_supported = False
            second = self.run_probe(cache)
            self.assert_ok(second)
            destination = Path(self.values(second, "plan")[0].split("|")[4])
            self.assertEqual(destination.read_bytes(), payload_for(repo_path))
            self.assertTrue(any(value == "bytes=9-" for _, value, _
                                in FakeArtifactHub.requests))

    def test_concurrent_launches_share_one_transfer_and_offline_reuses_cache(self):
        FakeArtifactHub.delay = 0.2
        with tempfile.TemporaryDirectory() as cache:
            env = os.environ.copy()
            for name in ("HF_TOKEN", "HF_TOKEN_PATH", "HF_HOME", "XDG_CACHE_HOME"):
                env.pop(name, None)
            first = subprocess.Popen(
                self.probe_command(cache), cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            second = subprocess.Popen(
                self.probe_command(cache), cwd=ROOT, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            outputs = [first.communicate(timeout=10), second.communicate(timeout=10)]
            self.assertEqual([first.returncode, second.returncode], [0, 0], outputs)
            self.assertEqual(self.requested_paths(),
                             ["Headroom128/H-receiver.gguf"])
            cache_hits = ["result=receiver|1" in stdout for stdout, _ in outputs]
            self.assertEqual(sorted(cache_hits), [False, True])

            before = list(FakeArtifactHub.requests)
            offline = self.run_probe(cache, modes="text,offline")
            self.assert_ok(offline)
            self.assertIn("result=receiver|1", offline.stdout)
            self.assertEqual(FakeArtifactHub.requests, before)

    def test_failure_context_is_complete_and_credentials_never_leak(self):
        FakeArtifactHub.fail_path = "Headroom128/H-receiver.gguf"
        with tempfile.TemporaryDirectory() as cache:
            result = self.run_probe(cache, extra_env={"HF_TOKEN": SECRET})
            combined = result.stdout + result.stderr
            self.assertEqual(result.returncode, 2, combined)
            for expected in ("repository='owner/repo'", f"revision='{SHA}'",
                             "selector='Headroom128'", "role='receiver'",
                             "expected_size=19", "destination='"):
                self.assertIn(expected, combined)
            self.assertNotIn(SECRET, combined)
            initial = [authorization for path, _, authorization
                       in FakeArtifactHub.requests if path.startswith("/owner/")]
            self.assertEqual(initial, [f"Bearer {SECRET}"])

    def test_credentials_are_not_forwarded_to_cross_host_lfs_xet_redirects(self):
        with tempfile.TemporaryDirectory() as cache:
            result = self.run_probe(cache, extra_env={"HF_TOKEN": SECRET})
            self.assert_ok(result)
            initial = [authorization for path, _, authorization
                       in FakeArtifactHub.requests if path.startswith("/owner/")]
            redirected = [authorization for path, _, authorization
                          in FakeArtifactHub.requests if path.startswith("/cas/")]
            self.assertEqual(initial, [f"Bearer {SECRET}"])
            self.assertEqual(redirected, [None])
            self.assertNotIn(SECRET, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
