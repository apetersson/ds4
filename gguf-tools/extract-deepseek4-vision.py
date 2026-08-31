#!/usr/bin/env python3
"""Extract the lossless DeepSeek V4 Vision-Exp encoder/aligner sidecar."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from safetensors import safe_open
from safetensors.torch import save_file


def is_vision_tensor(name: str) -> bool:
    return (
        name.startswith("vision.")
        or name.startswith("aligner.")
        or name in {"image_start", "image_end", "image_pad", "image_newline"}
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--hf", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    index_path = args.hf / "model.safetensors.index.json"
    index = json.loads(index_path.read_text(encoding="utf-8"))
    weight_map = index["weight_map"]
    selected = sorted(name for name in weight_map if is_vision_tensor(name))
    if not selected:
        raise SystemExit("checkpoint contains no Vision-Exp tensors")

    by_shard: dict[str, list[str]] = {}
    for name in selected:
        by_shard.setdefault(weight_map[name], []).append(name)

    tensors = {}
    for shard_name, names in sorted(by_shard.items()):
        with safe_open(args.hf / shard_name, framework="pt", device="cpu") as shard:
            for name in names:
                tensors[name] = shard.get_tensor(name)

    expected_controls = {"image_start", "image_end", "image_pad", "image_newline"}
    missing = expected_controls.difference(tensors)
    if missing or not any(name.startswith("vision.blocks.31.") for name in tensors):
        raise SystemExit(f"incomplete Vision-Exp tensor set; missing={sorted(missing)}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    save_file(
        tensors,
        args.out,
        metadata={
            "format": "deepseek-v4-vision-exp-native-v1",
            "source": str(args.hf.resolve()),
            "tensor_count": str(len(tensors)),
            "precision": "source-native-lossless",
        },
    )
    print(f"wrote {args.out} ({len(tensors)} tensors)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
