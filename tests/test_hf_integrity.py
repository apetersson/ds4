#!/usr/bin/env python3

import os
import socket
import subprocess
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path

from test_hf_cache import FakeArtifactHub, PROBE, ROOT, SHA, payload_for


class IntegrityTests(unittest.TestCase):
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

    def command(self, cache, modes="text", selector="Headroom128"):
        return [str(PROBE), str(cache), self.endpoint, selector, modes, "3000"]

    def run_probe(self, cache, modes="text", selector="Headroom128"):
        env = os.environ.copy()
        for name in ("HF_TOKEN", "HF_TOKEN_PATH", "HF_HOME", "XDG_CACHE_HOME"):
            env.pop(name, None)
        return subprocess.run(
            self.command(cache, modes, selector), cwd=ROOT, env=env,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False,
        )

    @staticmethod
    def plan(result, role):
        prefix = f"plan={role}|"
        line = next(line for line in result.stdout.splitlines()
                    if line.startswith(prefix))
        fields = line.split("=", 1)[1].split("|")
        return {"role": fields[0], "repo_path": fields[1],
                "requested": fields[2] == "1", "bytes": int(fields[3]),
                "destination": Path(fields[4])}

    def assert_ok(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("status=ok", result.stdout)

    def assert_integrity_failure(self, result, original_bytes):
        combined = result.stdout + result.stderr
        self.assertEqual(result.returncode, 2, combined)
        self.assertIn("no same-directory fallback is permitted", combined)
        self.assertIn("non-destructive recovery command:", combined)
        self.assertIn("mv --", combined)
        self.assertNotIn("rm --", combined)
        destination = self.plan(result, "receiver")["destination"]
        self.assertEqual(destination.read_bytes(), original_bytes)

    def test_every_requested_role_is_byte_and_sha_verified(self):
        with tempfile.TemporaryDirectory() as cache:
            ds4 = self.run_probe(cache, modes="vision,dspark")
            self.assert_ok(ds4)
            for role in ("receiver", "ds4_vision.tower",
                         "ds4_vision.projector", "ds4_vision.config",
                         "dspark"):
                entry = self.plan(ds4, role)
                self.assertTrue(entry["requested"], role)
                self.assertEqual(entry["destination"].read_bytes(),
                                 payload_for(entry["repo_path"]))

            llama = self.run_probe(cache, modes="text,materialize")
            self.assert_ok(llama)
            mmproj = self.plan(llama, "llama_cpp_mmproj")
            self.assertTrue(mmproj["requested"])
            self.assertEqual(mmproj["destination"].read_bytes(),
                             payload_for(mmproj["repo_path"]))

    def test_multiblock_sha256_matches_independent_fixture_digest(self):
        with tempfile.TemporaryDirectory() as cache:
            result = self.run_probe(cache, modes="text,sha-multiblock")
            self.assert_ok(result)
            receiver = self.plan(result, "receiver")
            self.assertEqual(receiver["bytes"], 131073)
            self.assertEqual(receiver["destination"].read_bytes(),
                             payload_for(receiver["repo_path"]))

    def test_uppercase_manifest_hash_verifies_valid_artifact(self):
        with tempfile.TemporaryDirectory() as cache:
            result = self.run_probe(cache, modes="text,uppercase-hash")
            self.assert_ok(result)
            receiver = self.plan(result, "receiver")
            self.assertEqual(receiver["destination"].read_bytes(),
                             payload_for(receiver["repo_path"]))

    def test_truncated_cache_fails_closed_and_is_preserved(self):
        with tempfile.TemporaryDirectory() as cache:
            valid = self.run_probe(cache)
            self.assert_ok(valid)
            destination = self.plan(valid, "receiver")["destination"]
            metadata = Path(str(destination) + ".ds4-meta")
            original_metadata = metadata.read_bytes()
            truncated = payload_for("Headroom128/H-receiver.gguf")[:-1]
            replacement = destination.with_name(destination.name + ".replacement")
            replacement.write_bytes(truncated)
            os.replace(replacement, destination)
            before = list(FakeArtifactHub.requests)
            failed = self.run_probe(cache, modes="text,offline")
            self.assert_integrity_failure(failed, truncated)
            self.assertEqual(FakeArtifactHub.requests, before)
            marker = "non-destructive recovery command: "
            command = (failed.stdout + failed.stderr).split(marker, 1)[1].splitlines()[0]
            prior_quarantine = destination.with_name(
                destination.name + ".untrusted.PRIOR")
            prior_quarantine.mkdir()
            (prior_quarantine / "evidence").write_bytes(b"prior evidence")
            recovered = subprocess.run(
                ["/bin/sh", "-c", command], cwd=ROOT, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            self.assertEqual(recovered.returncode, 0,
                             recovered.stdout + recovered.stderr)
            self.assertFalse(destination.exists())
            self.assertFalse(metadata.exists())
            quarantines = [
                path for path in destination.parent.glob(
                    destination.name + ".untrusted.*")
                if path != prior_quarantine
            ]
            self.assertEqual(len(quarantines), 1)
            self.assertEqual((quarantines[0] / destination.name).read_bytes(),
                             truncated)
            self.assertEqual(
                (quarantines[0] / metadata.name).read_bytes(), original_metadata)
            self.assertEqual((prior_quarantine / "evidence").read_bytes(),
                             b"prior evidence")
            reacquired = self.run_probe(cache)
            self.assert_ok(reacquired)
            self.assertEqual(destination.read_bytes(),
                             payload_for("Headroom128/H-receiver.gguf"))
            self.assertEqual((quarantines[0] / destination.name).read_bytes(),
                             truncated)

    def test_same_size_corruption_never_uses_same_directory_decoy(self):
        with tempfile.TemporaryDirectory() as cache:
            valid = self.run_probe(cache)
            self.assert_ok(valid)
            destination = self.plan(valid, "receiver")["destination"]
            corrupt = b"X" * 19
            replacement = destination.with_name(destination.name + ".replacement")
            replacement.write_bytes(corrupt)
            os.replace(replacement, destination)
            decoy = destination.with_name("plausible-sibling.gguf")
            decoy.write_bytes(payload_for("Headroom128/H-receiver.gguf"))
            failed = self.run_probe(cache, modes="text,offline")
            self.assert_integrity_failure(failed, corrupt)
            self.assertEqual(decoy.read_bytes(),
                             payload_for("Headroom128/H-receiver.gguf"))

    def test_cross_variant_substitution_is_rejected(self):
        with tempfile.TemporaryDirectory() as cache:
            headroom = self.run_probe(cache)
            quality = self.run_probe(cache, selector="Quality128")
            self.assert_ok(headroom)
            self.assert_ok(quality)
            destination = self.plan(headroom, "receiver")["destination"]
            substituted = payload_for("Quality128/Q-receiver.gguf")
            replacement = destination.with_name(destination.name + ".replacement")
            replacement.write_bytes(substituted)
            os.replace(replacement, destination)
            failed = self.run_probe(cache, modes="text,offline")
            self.assert_integrity_failure(failed, substituted)

    def test_mmproj_cannot_be_confused_with_raw_vision_role(self):
        with tempfile.TemporaryDirectory() as cache:
            valid = self.run_probe(cache, modes="text,materialize")
            self.assert_ok(valid)
            mmproj = self.plan(valid, "llama_cpp_mmproj")
            raw_role_bytes = payload_for("Headroom128/H-tower.safetensors") + b"!"
            self.assertEqual(len(raw_role_bytes), mmproj["bytes"])
            replacement = mmproj["destination"].with_name(
                mmproj["destination"].name + ".replacement")
            replacement.write_bytes(raw_role_bytes)
            os.replace(replacement, mmproj["destination"])
            failed = self.run_probe(cache, modes="text,materialize,offline")
            combined = failed.stdout + failed.stderr
            self.assertEqual(failed.returncode, 2, combined)
            self.assertIn("role='llama_cpp_mmproj'", combined)
            self.assertIn("no same-directory fallback is permitted", combined)
            self.assertEqual(mmproj["destination"].read_bytes(), raw_role_bytes)

    def test_symlinks_special_files_and_snapshot_escape_are_rejected(self):
        with self.subTest("destination symlink"), \
                tempfile.TemporaryDirectory() as cache, \
                tempfile.TemporaryDirectory() as outside:
            valid = self.run_probe(cache)
            self.assert_ok(valid)
            destination = self.plan(valid, "receiver")["destination"]
            destination.unlink()
            target = Path(outside, "target.gguf")
            target.write_bytes(payload_for("Headroom128/H-receiver.gguf"))
            destination.symlink_to(target)
            failed = self.run_probe(cache, modes="text,offline")
            self.assertEqual(failed.returncode, 2, failed.stdout + failed.stderr)
            self.assertTrue(destination.is_symlink())

        with self.subTest("special file"), tempfile.TemporaryDirectory() as cache:
            destination = (Path(cache) / "repos" / "owner%2Frepo" /
                           "snapshots" / SHA / "Headroom128" /
                           "H-receiver.gguf")
            destination.parent.mkdir(parents=True)
            os.mkfifo(destination)
            failed = self.run_probe(cache, modes="text,offline")
            self.assertEqual(failed.returncode, 2, failed.stdout + failed.stderr)
            self.assertTrue(destination.exists())

        with self.subTest("snapshot symlink escape"), \
                tempfile.TemporaryDirectory() as cache, \
                tempfile.TemporaryDirectory() as outside:
            snapshots = Path(cache) / "repos" / "owner%2Frepo" / "snapshots"
            snapshots.mkdir(parents=True)
            (snapshots / SHA).symlink_to(outside, target_is_directory=True)
            failed = self.run_probe(cache)
            combined = failed.stdout + failed.stderr
            self.assertEqual(failed.returncode, 2, combined)
            self.assertIn("without symlinks", combined)
            self.assertEqual(list(Path(outside).iterdir()), [])

    def test_manifest_selected_plan_mutation_is_rejected_before_network(self):
        with tempfile.TemporaryDirectory() as cache:
            result = self.run_probe(cache, modes="text,mutate-plan")
            combined = result.stdout + result.stderr
            self.assertEqual(result.returncode, 2, combined)
            self.assertIn("manifest-mutated sealed plan", combined)
            self.assertEqual(FakeArtifactHub.requests, [])

    def test_concurrent_replacement_cannot_cross_verified_fd_handoff(self):
        with tempfile.TemporaryDirectory() as cache:
            valid = self.run_probe(cache)
            self.assert_ok(valid)
            destination = self.plan(valid, "receiver")["destination"]
            env = os.environ.copy()
            process = subprocess.Popen(
                self.command(cache, "text,offline,verified-hold"),
                cwd=ROOT, env=env, text=True, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                bufsize=1,
            )
            prefix = []
            while True:
                line = process.stdout.readline()
                self.assertNotEqual(line, "", "probe exited before verified handoff")
                prefix.append(line)
                if line.strip() == "verified_ready=receiver":
                    break
            replacement = destination.with_name(destination.name + ".replacement")
            replacement.write_bytes(b"R" * 19)
            os.replace(replacement, destination)
            remaining, stderr = process.communicate("\n", timeout=10)
            output = "".join(prefix) + remaining + stderr
            self.assertEqual(process.returncode, 0, output)
            self.assertIn(f"held_first_byte={ord('H')}", output)
            self.assertIn("replacement=rejected", output)
            self.assertIn("non-destructive recovery command:", output)
            self.assertIn("mv --", output)
            self.assertEqual(destination.read_bytes(), b"R" * 19)

    def test_resolver_has_no_repository_code_execution_primitive(self):
        source = (ROOT / "ds4_hf.c").read_text(encoding="utf-8")
        for primitive in ("system(", "popen(", "execve(", "execl(",
                          "dlopen(", "posix_spawn("):
            self.assertNotIn(primitive, source)
        for forbidden_key in ("python", "auto_map", "shell", "command",
                              "executable", "interpreter"):
            self.assertIn(f'"{forbidden_key}"', source)


if __name__ == "__main__":
    unittest.main(verbosity=2)
