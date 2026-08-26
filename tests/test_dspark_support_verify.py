#!/usr/bin/env python3
"""Unit tests for the exact DSpark catalog verifier."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "gguf-tools" / "verify_dspark_support.py"
SPEC = importlib.util.spec_from_file_location("verify_dspark_support", MODULE_PATH)
assert SPEC and SPEC.loader
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)


def _string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("<Q", len(encoded)) + encoded


def _value(value: object) -> tuple[int, bytes]:
    if isinstance(value, str):
        return 8, _string(value)
    if isinstance(value, list):
        return 9, struct.pack("<IQ", 4, len(value)) + b"".join(
            struct.pack("<I", item) for item in value
        )
    if isinstance(value, int):
        return 4, struct.pack("<I", value)
    raise TypeError(value)


def _tensor_size(dims: tuple[int, ...], ggml_type: int) -> int:
    return VERIFY._tensor_nbytes(dims, ggml_type)


def _write_gguf(path: Path, metadata: dict[str, object],
                tensors: list[tuple[str, tuple[int, ...], int]]) -> int:
    kv = bytearray()
    for key, value in metadata.items():
        value_type, payload = _value(value)
        kv += _string(key) + struct.pack("<I", value_type) + payload

    directory = bytearray()
    payload = bytearray()
    for name, dims, ggml_type in tensors:
        while len(payload) % 32:
            payload.append(0)
        directory += _string(name)
        directory += struct.pack("<I", len(dims))
        directory += b"".join(struct.pack("<Q", dim) for dim in dims)
        directory += struct.pack("<IQ", ggml_type, len(payload))
        payload += bytes([ggml_type & 0xFF]) * _tensor_size(dims, ggml_type)

    header = b"GGUF" + struct.pack("<IQQ", 3, len(tensors), len(metadata))
    data_offset = (len(header) + len(kv) + len(directory) + 31) & ~31
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + kv + directory + bytes(data_offset - len(header) - len(kv) - len(directory)) + payload)
    return data_offset


def _fill_types(tensors: list[tuple[str, tuple[int, ...], int]],
                expected: dict[int, int], prefix: str) -> None:
    existing = Counter(item[2] for item in tensors)
    for ggml_type, count in expected.items():
        dims = (256,) if ggml_type in (10, 16) else ((32,) if ggml_type in (8, 39) else (1,))
        for index in range(count - existing[ggml_type]):
            tensors.append((f"{prefix}.{ggml_type}.{index}.weight", dims, ggml_type))


def _receiver_tensors(expected: object) -> list[tuple[str, tuple[int, ...], int]]:
    tensors = []
    for layer in range(43):
        mxfp4 = layer in expected.receiver_mxfp4_layers
        tensors.extend((
            (f"blk.{layer}.ffn_gate_exps.weight", (256,), 39 if mxfp4 else 16),
            (f"blk.{layer}.ffn_up_exps.weight", (256,), 39 if mxfp4 else 16),
            (f"blk.{layer}.ffn_down_exps.weight", (256,), 39 if mxfp4 else 10),
        ))
    _fill_types(tensors, expected.receiver_type_counts, "receiver.filler")
    return tensors


def _support_tensors(expected: object) -> list[tuple[str, tuple[int, ...], int]]:
    tensors = []
    for stage in range(3):
        tensors.extend((
            (f"mtp.{stage}.ffn_gate_exps.weight", (256,), 16),
            (f"mtp.{stage}.ffn_up_exps.weight", (256,), 16),
            (f"mtp.{stage}.ffn_down_exps.weight", (256,),
             39 if (stage, "down") in expected.support_mxfp4_roles else 10),
        ))
    tensors.extend((
        ("mtp.0.main_proj.weight", (32,), 8),
        ("mtp.2.confidence_head.proj.weight", (1,), 0),
        ("mtp.2.markov_head.markov_w1.weight", (32,), 8),
        ("mtp.2.markov_head.markov_w2.weight", (32,), 8),
    ))
    existing = Counter(item[2] for item in tensors)
    remaining_by_stage = Counter({stage: expected.support_stage_counts[stage] for stage in range(3)})
    remaining_by_stage.subtract(int(item[0].split(".")[1]) for item in tensors)
    for ggml_type, total in expected.support_type_counts.items():
        dims = (256,) if ggml_type in (10, 16) else ((32,) if ggml_type in (8, 39) else (1,))
        for index in range(total - existing[ggml_type]):
            stage = min(stage for stage in range(3) if remaining_by_stage[stage] > 0)
            tensors.append((f"mtp.{stage}.filler_{ggml_type}_{index}.weight", dims, ggml_type))
            remaining_by_stage[stage] -= 1
    return tensors


def _sha256(path: Path, offset: int = 0) -> str:
    return hashlib.sha256(path.read_bytes()[offset:]).hexdigest()


def _add_artifact(root: Path, rel: str, profile: str, architecture: str,
                  tensors: list[tuple[str, tuple[int, ...], int]],
                  support: bool) -> dict[str, object]:
    path = root / rel
    metadata = {
        "general.architecture": architecture,
        "general.name": f"Synthetic {profile}",
        "general.alignment": 32,
        "general.description": "synthetic test fixture",
        "ds4.profile": profile,
    }
    if support:
        metadata.update({
            "dspark.block_size": 5,
            "dspark.markov_rank": 256,
            "dspark.noise_token_id": 128799,
            "dspark.target_layer_ids": [40, 41, 42],
            "dspark.stage_count": 3,
            "dspark.n_layers": 3,
        })
    data_offset = _write_gguf(path, metadata, tensors)
    sidecar = {
        "output": rel,
        "output_bytes": path.stat().st_size,
        "output_sha256": _sha256(path),
        "tensor_data_bytes": path.stat().st_size - data_offset,
        "tensor_data_sha256": _sha256(path, data_offset),
        "tensor_count": len(tensors),
        "alignment": 32,
        "source_data_offset": data_offset,
        "output_data_offset": data_offset,
        "metadata": {
            "general.name": metadata["general.name"],
            "general.description": metadata["general.description"],
            "ds4.profile": profile,
        },
    }
    path.with_suffix(".metadata.json").write_text(json.dumps(sidecar), encoding="utf-8")
    return {"bytes": path.stat().st_size, "sha": sidecar["output_sha256"]}


class CatalogVerifierTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.expected = VERIFY.EXACT_PAIRS[1]
        main_rel = "Quality/main.gguf"
        support_rel = "Quality/support.gguf"
        main = _add_artifact(self.root, main_rel, self.expected.profile, "deepseek4",
                             _receiver_tensors(self.expected), False)
        support = _add_artifact(self.root, support_rel, self.expected.profile,
                                "deepseek4-dspark", _support_tensors(self.expected), True)
        variants = {
            "variants": [{
                "selector": self.expected.selector,
                "profile": self.expected.profile,
                "main": main_rel,
                "main_bytes": main["bytes"],
                "main_sha256": main["sha"],
                "dspark": support_rel,
                "dspark_bytes": support["bytes"],
                "dspark_sha256": support["sha"],
            }]
        }
        (self.root / "variants.json").write_text(json.dumps(variants), encoding="utf-8")

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def test_exact_pair_passes_structure_metadata_and_hash_checks(self) -> None:
        report = VERIFY.verify_catalog(self.root, (self.expected,), verify_hashes=True)
        self.assertEqual(report["status"], "PASS")
        pair = report["pairs"][0]
        self.assertEqual(pair["receiver"]["mxfp4_layers"], [10, 14, 30, 34, 37, 38, 39, 40, 41, 42])
        self.assertEqual(pair["support"]["mxfp4_roles"], ["mtp.0.down", "mtp.1.down", "mtp.2.down"])

    def test_cross_profile_support_is_rejected(self) -> None:
        support = self.root / "Quality" / "support.gguf"
        raw = bytearray(support.read_bytes())
        needle = b"Quality128"
        offset = raw.rindex(needle)
        raw[offset:offset + len(needle)] = b"Wrong_____"
        support.write_bytes(raw)
        with self.assertRaisesRegex(VERIFY.VerificationError, "profile"):
            VERIFY.verify_catalog(self.root, (self.expected,), verify_hashes=False)

    def test_payload_hash_mismatch_is_rejected(self) -> None:
        support = self.root / "Quality" / "support.gguf"
        raw = bytearray(support.read_bytes())
        raw[-1] ^= 0xFF
        support.write_bytes(raw)
        with self.assertRaisesRegex(VERIFY.VerificationError, "SHA-256"):
            VERIFY.verify_catalog(self.root, (self.expected,), verify_hashes=True)


if __name__ == "__main__":
    unittest.main()
