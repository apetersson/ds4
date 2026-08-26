#!/usr/bin/env python3
"""Verify the exact packaged DSpark receiver/support catalog pairs.

The verifier reads only GGUF metadata/directories unless hash verification is
requested (the CLI default).  It never materializes or rewrites tensor data.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable


GGUF_HEADER_SIZE = 24
GGUF_DEFAULT_ALIGNMENT = 32
GGUF_VALUE_STRING = 8
GGUF_VALUE_ARRAY = 9
GGUF_SCALAR_FORMATS = {
    0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f",
    7: "?", 10: "Q", 11: "q", 12: "d",
}

# GGML type -> (elements per block, bytes per block, stable display name).
GGML_TYPES = {
    0: (1, 4, "F32"),
    1: (1, 2, "F16"),
    8: (32, 34, "Q8_0"),
    10: (256, 84, "Q2_K"),
    16: (256, 66, "IQ2_XXS"),
    26: (1, 4, "I32"),
    39: (32, 17, "MXFP4"),
}

RECEIVER_EXPERT_RE = re.compile(
    r"^blk\.(\d+)\.ffn_(gate|up|down)_exps\.weight$"
)
SUPPORT_EXPERT_RE = re.compile(
    r"^mtp\.(\d+)\.ffn_(gate|up|down)_exps\.weight$"
)


@dataclass(frozen=True)
class TensorInfo:
    name: str
    dims: tuple[int, ...]
    ggml_type: int
    rel_offset: int
    n_bytes: int


@dataclass(frozen=True)
class GGUFInfo:
    path: Path
    version: int
    metadata: dict[str, object]
    tensors: tuple[TensorInfo, ...]
    alignment: int
    data_offset: int


@dataclass(frozen=True)
class PairExpectation:
    selector: str
    profile: str
    receiver_tensor_count: int
    receiver_type_counts: dict[int, int]
    receiver_mxfp4_layers: frozenset[int]
    support_tensor_count: int
    support_type_counts: dict[int, int]
    support_mxfp4_roles: frozenset[tuple[int, str]]
    support_stage_counts: dict[int, int]


EXACT_PAIRS = (
    PairExpectation(
        selector="Headroom128-IQ2_XXS",
        profile="Headroom128",
        receiver_tensor_count=1328,
        receiver_type_counts={0: 492, 1: 359, 8: 345, 10: 43, 16: 86, 26: 3},
        receiver_mxfp4_layers=frozenset(),
        support_tensor_count=81,
        support_type_counts={0: 34, 1: 7, 8: 31, 10: 3, 16: 6},
        support_mxfp4_roles=frozenset(),
        support_stage_counts={0: 26, 1: 24, 2: 31},
    ),
    PairExpectation(
        selector="Quality128-IQ2_XXS_XL",
        profile="Quality128",
        receiver_tensor_count=1328,
        receiver_type_counts={0: 492, 1: 359, 8: 345, 10: 33, 16: 66, 26: 3, 39: 30},
        receiver_mxfp4_layers=frozenset({10, 14, 30, 34, 37, 38, 39, 40, 41, 42}),
        support_tensor_count=81,
        support_type_counts={0: 34, 1: 7, 8: 31, 16: 6, 39: 3},
        support_mxfp4_roles=frozenset({(0, "down"), (1, "down"), (2, "down")}),
        support_stage_counts={0: 26, 1: 24, 2: 31},
    ),
)


class VerificationError(ValueError):
    pass


def _read_exact(src: BinaryIO, n_bytes: int) -> bytes:
    data = src.read(n_bytes)
    if len(data) != n_bytes:
        raise VerificationError("short read while parsing GGUF")
    return data


def _read_u32(src: BinaryIO) -> int:
    return struct.unpack("<I", _read_exact(src, 4))[0]


def _read_u64(src: BinaryIO) -> int:
    return struct.unpack("<Q", _read_exact(src, 8))[0]


def _read_string(src: BinaryIO) -> str:
    length = _read_u64(src)
    if length > 1 << 30:
        raise VerificationError(f"unreasonable GGUF string length {length}")
    try:
        return _read_exact(src, length).decode("utf-8")
    except UnicodeDecodeError as exc:
        raise VerificationError("invalid UTF-8 in GGUF string") from exc


def _read_value(src: BinaryIO, value_type: int, depth: int = 0) -> object:
    if depth > 8:
        raise VerificationError("GGUF metadata arrays are nested too deeply")
    if value_type == GGUF_VALUE_STRING:
        return _read_string(src)
    if value_type == GGUF_VALUE_ARRAY:
        item_type = _read_u32(src)
        count = _read_u64(src)
        if count > 1 << 28:
            raise VerificationError(f"unreasonable GGUF array length {count}")
        return [_read_value(src, item_type, depth + 1) for _ in range(count)]
    fmt = GGUF_SCALAR_FORMATS.get(value_type)
    if fmt is None:
        raise VerificationError(f"unsupported GGUF metadata type {value_type}")
    return struct.unpack("<" + fmt, _read_exact(src, struct.calcsize(fmt)))[0]


def _pad_to(value: int, alignment: int) -> int:
    if alignment <= 0 or alignment & (alignment - 1):
        raise VerificationError(f"invalid GGUF alignment {alignment}")
    return (value + alignment - 1) & ~(alignment - 1)


def _tensor_nbytes(dims: Iterable[int], ggml_type: int) -> int:
    type_info = GGML_TYPES.get(ggml_type)
    if type_info is None:
        raise VerificationError(f"unsupported GGML tensor type {ggml_type}")
    block_elements, block_bytes, _ = type_info
    elements = 1
    for dim in dims:
        if dim <= 0:
            raise VerificationError(f"invalid tensor dimension {dim}")
        elements *= dim
    if elements % block_elements:
        raise VerificationError(
            f"{elements} tensor elements are not divisible by type-{ggml_type} "
            f"block size {block_elements}"
        )
    return elements // block_elements * block_bytes


def parse_gguf(path: Path) -> GGUFInfo:
    with path.open("rb") as src:
        if _read_exact(src, 4) != b"GGUF":
            raise VerificationError(f"{path} is not a GGUF file")
        version = _read_u32(src)
        tensor_count = _read_u64(src)
        kv_count = _read_u64(src)
        if version != 3:
            raise VerificationError(f"{path}: expected GGUF v3, got v{version}")
        if tensor_count > 1 << 20 or kv_count > 1 << 20:
            raise VerificationError(f"{path}: unreasonable GGUF directory counts")

        metadata: dict[str, object] = {}
        for _ in range(kv_count):
            key = _read_string(src)
            if key in metadata:
                raise VerificationError(f"{path}: duplicate metadata key {key}")
            metadata[key] = _read_value(src, _read_u32(src))

        tensors: list[TensorInfo] = []
        names: set[str] = set()
        for _ in range(tensor_count):
            name = _read_string(src)
            if name in names:
                raise VerificationError(f"{path}: duplicate tensor {name}")
            names.add(name)
            n_dims = _read_u32(src)
            if n_dims < 1 or n_dims > 4:
                raise VerificationError(f"{path}: tensor {name} has {n_dims} dimensions")
            dims = tuple(_read_u64(src) for _ in range(n_dims))
            ggml_type = _read_u32(src)
            rel_offset = _read_u64(src)
            tensors.append(
                TensorInfo(name, dims, ggml_type, rel_offset,
                           _tensor_nbytes(dims, ggml_type))
            )
        alignment = int(metadata.get("general.alignment", GGUF_DEFAULT_ALIGNMENT))
        data_offset = _pad_to(src.tell(), alignment)

    file_size = path.stat().st_size
    previous_end = 0
    for tensor in sorted(tensors, key=lambda item: item.rel_offset):
        if tensor.rel_offset % alignment:
            raise VerificationError(
                f"{path}: tensor {tensor.name} offset is not {alignment}-byte aligned"
            )
        if tensor.rel_offset < previous_end:
            raise VerificationError(f"{path}: overlapping tensor {tensor.name}")
        previous_end = tensor.rel_offset + tensor.n_bytes
        if data_offset + previous_end > file_size:
            raise VerificationError(f"{path}: tensor {tensor.name} exceeds file size")

    return GGUFInfo(path, version, metadata, tuple(tensors), alignment, data_offset)


def _sha256_file_and_payload(path: Path, data_offset: int) -> tuple[str, str]:
    full_digest = hashlib.sha256()
    payload_digest = hashlib.sha256()
    with path.open("rb") as src:
        remaining_header = data_offset
        while remaining_header:
            chunk = src.read(min(16 * 1024 * 1024, remaining_header))
            if not chunk:
                raise VerificationError(f"{path}: short read before tensor data")
            full_digest.update(chunk)
            remaining_header -= len(chunk)
        while chunk := src.read(16 * 1024 * 1024):
            full_digest.update(chunk)
            payload_digest.update(chunk)
    return full_digest.hexdigest(), payload_digest.hexdigest()


def _type_counts(info: GGUFInfo) -> dict[int, int]:
    return dict(sorted(Counter(t.ggml_type for t in info.tensors).items()))


def _type_names(counts: dict[int, int]) -> dict[str, int]:
    return {GGML_TYPES[k][2]: v for k, v in counts.items()}


def _require_equal(actual: object, expected: object, label: str) -> None:
    if actual != expected:
        raise VerificationError(f"{label}: expected {expected!r}, got {actual!r}")


def _load_sidecar(root: Path, gguf_rel: str) -> tuple[Path, dict[str, object]]:
    gguf_path = root / gguf_rel
    sidecar = gguf_path.with_suffix(".metadata.json")
    if not sidecar.is_file():
        raise VerificationError(f"missing metadata sidecar {sidecar}")
    return sidecar, json.loads(sidecar.read_text(encoding="utf-8"))


def _validate_file_record(
    root: Path,
    rel_path: str,
    catalog_bytes: int,
    catalog_sha: str,
    verify_hashes: bool,
) -> tuple[GGUFInfo, dict[str, object], dict[str, object]]:
    path = root / rel_path
    if not path.is_file():
        raise VerificationError(f"missing catalog artifact {path}")
    sidecar_path, sidecar = _load_sidecar(root, rel_path)
    info = parse_gguf(path)
    size = path.stat().st_size

    _require_equal(size, catalog_bytes, f"{rel_path} catalog byte count")
    _require_equal(sidecar.get("output"), rel_path, f"{sidecar_path} output")
    _require_equal(sidecar.get("output_bytes"), size, f"{sidecar_path} output_bytes")
    _require_equal(sidecar.get("output_sha256"), catalog_sha,
                   f"{sidecar_path} output_sha256")
    _require_equal(sidecar.get("tensor_count"), len(info.tensors),
                   f"{sidecar_path} tensor_count")
    _require_equal(sidecar.get("alignment"), info.alignment,
                   f"{sidecar_path} alignment")
    _require_equal(sidecar.get("output_data_offset"), info.data_offset,
                   f"{sidecar_path} output_data_offset")
    _require_equal(sidecar.get("tensor_data_bytes"), size - info.data_offset,
                   f"{sidecar_path} tensor_data_bytes")
    for key, value in sidecar.get("metadata", {}).items():
        _require_equal(info.metadata.get(key), value, f"{rel_path} metadata {key}")

    hashes: dict[str, object] = {"verified": verify_hashes}
    if verify_hashes:
        full_sha, payload_sha = _sha256_file_and_payload(path, info.data_offset)
        _require_equal(full_sha, catalog_sha, f"{rel_path} SHA-256")
        _require_equal(payload_sha, sidecar.get("tensor_data_sha256"),
                       f"{rel_path} tensor-data SHA-256")
        hashes.update(full_sha256=full_sha, tensor_data_sha256=payload_sha)
    return info, sidecar, hashes


def _validate_receiver(info: GGUFInfo, expected: PairExpectation) -> dict[str, object]:
    _require_equal(info.metadata.get("general.architecture"), "deepseek4",
                   f"{info.path} architecture")
    _require_equal(info.metadata.get("ds4.profile"), expected.profile,
                   f"{info.path} profile")
    _require_equal(len(info.tensors), expected.receiver_tensor_count,
                   f"{info.path} tensor count")
    counts = _type_counts(info)
    _require_equal(counts, expected.receiver_type_counts,
                   f"{info.path} tensor type counts")

    routed: dict[tuple[int, str], int] = {}
    for tensor in info.tensors:
        match = RECEIVER_EXPERT_RE.match(tensor.name)
        if match:
            routed[(int(match.group(1)), match.group(2))] = tensor.ggml_type
    expected_roles = {(layer, role) for layer in range(43)
                      for role in ("gate", "up", "down")}
    _require_equal(set(routed), expected_roles, f"{info.path} routed expert structure")
    mxfp4_layers = {
        layer for layer in range(43)
        if all(routed[(layer, role)] == 39 for role in ("gate", "up", "down"))
    }
    _require_equal(mxfp4_layers, set(expected.receiver_mxfp4_layers),
                   f"{info.path} preserved MXFP4 layers")
    for layer in range(43):
        if layer in mxfp4_layers:
            continue
        _require_equal(routed[(layer, "gate")], 16, f"receiver layer {layer} gate type")
        _require_equal(routed[(layer, "up")], 16, f"receiver layer {layer} up type")
        _require_equal(routed[(layer, "down")], 10, f"receiver layer {layer} down type")
    return {"tensor_count": len(info.tensors), "tensor_types": _type_names(counts),
            "mxfp4_layers": sorted(mxfp4_layers)}


def _validate_support(info: GGUFInfo, expected: PairExpectation) -> dict[str, object]:
    required_metadata = {
        "general.architecture": "deepseek4-dspark",
        "ds4.profile": expected.profile,
        "dspark.block_size": 5,
        "dspark.markov_rank": 256,
        "dspark.noise_token_id": 128799,
        "dspark.target_layer_ids": [40, 41, 42],
        "dspark.stage_count": 3,
        "dspark.n_layers": 3,
    }
    for key, value in required_metadata.items():
        _require_equal(info.metadata.get(key), value, f"{info.path} metadata {key}")
    _require_equal(len(info.tensors), expected.support_tensor_count,
                   f"{info.path} tensor count")
    counts = _type_counts(info)
    _require_equal(counts, expected.support_type_counts,
                   f"{info.path} tensor type counts")

    stages = Counter()
    routed: dict[tuple[int, str], int] = {}
    for tensor in info.tensors:
        parts = tensor.name.split(".")
        if len(parts) < 3 or parts[0] != "mtp" or not parts[1].isdigit():
            raise VerificationError(f"{info.path}: non-DSpark tensor {tensor.name}")
        stages[int(parts[1])] += 1
        match = SUPPORT_EXPERT_RE.match(tensor.name)
        if match:
            routed[(int(match.group(1)), match.group(2))] = tensor.ggml_type
    _require_equal(dict(stages), expected.support_stage_counts,
                   f"{info.path} per-stage tensor counts")
    expected_roles = {(stage, role) for stage in range(3)
                      for role in ("gate", "up", "down")}
    _require_equal(set(routed), expected_roles, f"{info.path} routed expert structure")
    mxfp4_roles = {role for role, ggml_type in routed.items() if ggml_type == 39}
    _require_equal(mxfp4_roles, set(expected.support_mxfp4_roles),
                   f"{info.path} preserved MXFP4 roles")
    for stage in range(3):
        _require_equal(routed[(stage, "gate")], 16, f"support stage {stage} gate type")
        _require_equal(routed[(stage, "up")], 16, f"support stage {stage} up type")
        expected_down = 39 if (stage, "down") in mxfp4_roles else 10
        _require_equal(routed[(stage, "down")], expected_down,
                       f"support stage {stage} down type")

    names = {tensor.name for tensor in info.tensors}
    for required in (
        "mtp.0.main_proj.weight",
        "mtp.2.confidence_head.proj.weight",
        "mtp.2.markov_head.markov_w1.weight",
        "mtp.2.markov_head.markov_w2.weight",
    ):
        if required not in names:
            raise VerificationError(f"{info.path}: missing required tensor {required}")
    return {
        "tensor_count": len(info.tensors),
        "tensor_types": _type_names(counts),
        "stage_counts": dict(sorted(stages.items())),
        "mxfp4_roles": [f"mtp.{stage}.{role}" for stage, role in sorted(mxfp4_roles)],
        "metadata": {key: info.metadata[key] for key in required_metadata},
    }


def verify_catalog(
    root: Path,
    expectations: Iterable[PairExpectation] = EXACT_PAIRS,
    verify_hashes: bool = True,
) -> dict[str, object]:
    root = root.resolve()
    variants_path = root / "variants.json"
    if not variants_path.is_file():
        raise VerificationError(f"missing {variants_path}")
    catalog = json.loads(variants_path.read_text(encoding="utf-8"))
    variants = {entry["selector"]: entry for entry in catalog.get("variants", [])}
    results: list[dict[str, object]] = []

    for expected in expectations:
        if expected.selector not in variants:
            raise VerificationError(f"variants.json does not discover {expected.selector}")
        entry = variants[expected.selector]
        _require_equal(entry.get("profile"), expected.profile,
                       f"{expected.selector} catalog profile")
        receiver_rel = str(entry["main"])
        support_rel = str(entry["dspark"])
        receiver, _, receiver_hashes = _validate_file_record(
            root, receiver_rel, int(entry["main_bytes"]), str(entry["main_sha256"]),
            verify_hashes,
        )
        support, _, support_hashes = _validate_file_record(
            root, support_rel, int(entry["dspark_bytes"]), str(entry["dspark_sha256"]),
            verify_hashes,
        )
        receiver_result = _validate_receiver(receiver, expected)
        support_result = _validate_support(support, expected)
        _require_equal(receiver.metadata.get("ds4.profile"),
                       support.metadata.get("ds4.profile"),
                       f"{expected.selector} receiver/support profile pairing")
        results.append({
            "selector": expected.selector,
            "profile": expected.profile,
            "receiver": {"path": receiver_rel, "bytes": receiver.path.stat().st_size,
                         "hashes": receiver_hashes, **receiver_result},
            "support": {"path": support_rel, "bytes": support.path.stat().st_size,
                        "hashes": support_hashes, **support_result},
        })
    return {"status": "PASS", "catalog_root": str(root), "pairs": results}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog_root", type=Path,
                        help="catalog directory containing variants.json")
    parser.add_argument("--skip-hashes", action="store_true",
                        help="inspect metadata and tensor structure without streaming payloads")
    parser.add_argument("--json", action="store_true", help="emit JSON evidence")
    args = parser.parse_args(argv)
    try:
        report = verify_catalog(args.catalog_root, verify_hashes=not args.skip_hashes)
    except (OSError, KeyError, TypeError, json.JSONDecodeError, VerificationError) as exc:
        if args.json:
            print(json.dumps({"status": "FAIL", "error": str(exc)}, indent=2))
        else:
            print(f"DSpark catalog verification: FAIL: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        for pair in report["pairs"]:
            print(
                f"{pair['selector']}: PASS; receiver={pair['receiver']['tensor_count']} "
                f"support={pair['support']['tensor_count']} tensors; "
                f"types={pair['support']['tensor_types']}"
            )
        print("DSpark catalog verification: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
