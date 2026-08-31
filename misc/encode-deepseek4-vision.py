#!/usr/bin/env python3
"""Encode one image with the native DeepSeek V4 Vision-Exp tower.

The tower, aligner, and visual control embeddings remain in their checkpoint
dtypes. Only the DS4VEMB1 interchange rows are expanded to F32.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import math
import struct
import time
from functools import lru_cache
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import unquote_to_bytes

import numpy as np
import torch
import torch.nn.functional as F
from PIL import Image, ImageOps
from safetensors.torch import load_file
from torch import nn

MAGIC = b"DS4VEMB1"
VERSION = 1
FLAG_VISUAL_ROUTING = 1 << 1
HEADER = struct.Struct("<8sIIIIiiiI32s")
IMAGE_TOKEN_ID = 129279
HIDDEN_SIZE = 4096
MAX_IMAGE_BYTES = 64 * 1024 * 1024

IMAGE_START, IMAGE_PAD, IMAGE, IMAGE_NEW_LINE, IMAGE_END = range(5)
COMPRESS_PAD_TO = 4


@lru_cache(8)
def vision_cos_sin(n_h: int, n_w: int, dim: int, theta: float):
    inv = 1.0 / (theta ** (torch.arange(0, dim, 2, dtype=torch.float32) / dim))
    hpos = torch.arange(n_h).unsqueeze(1).expand(n_h, n_w)
    wpos = torch.arange(n_w).unsqueeze(0).expand(n_h, n_w)
    freqs = torch.stack([hpos, wpos], dim=-1).reshape(-1, 2, 1).float() * inv
    freqs = freqs.flatten(1)
    return freqs.cos().unsqueeze(1), freqs.sin().unsqueeze(1)


def apply_rotary(x: torch.Tensor, cos: torch.Tensor, sin: torch.Tensor):
    dtype = x.dtype
    x1, x2 = x.float().chunk(2, dim=-1)
    return torch.cat([x1 * cos - x2 * sin, x2 * cos + x1 * sin], dim=-1).to(dtype)


class RMSNorm(nn.Module):
    def __init__(self, dim: int, eps: float = 1e-6):
        super().__init__()
        self.eps = eps
        self.weight = nn.Parameter(torch.ones(dim, dtype=torch.float32))

    def forward(self, x: torch.Tensor):
        dtype = x.dtype
        y = x.float()
        y = y * torch.rsqrt(y.square().mean(-1, keepdim=True) + self.eps)
        return (self.weight * y).to(dtype)


class Attention(nn.Module):
    def __init__(self, cfg):
        super().__init__()
        self.n_heads = cfg.vision_n_heads
        self.head_dim = cfg.vision_dim // cfg.vision_n_heads
        self.wqkv = nn.Linear(cfg.vision_dim, 3 * cfg.vision_dim)
        self.wo = nn.Linear(cfg.vision_dim, cfg.vision_dim)

    def forward(self, x, cos, sin):
        n = x.shape[0]
        q, k, v = (
            t.view(n, self.n_heads, self.head_dim)
            for t in self.wqkv(x).chunk(3, dim=-1)
        )
        q = apply_rotary(q, cos, sin)
        k = apply_rotary(k, cos, sin)
        out = F.scaled_dot_product_attention(
            q.transpose(0, 1), k.transpose(0, 1), v.transpose(0, 1)
        )
        return self.wo(out.transpose(0, 1).reshape(n, -1))


class MLP(nn.Module):
    def __init__(self, cfg):
        super().__init__()
        self.w1 = nn.Linear(cfg.vision_dim, 2 * cfg.vision_inter_dim, bias=False)
        self.w2 = nn.Linear(cfg.vision_inter_dim, cfg.vision_dim, bias=False)

    def forward(self, x):
        gate, up = self.w1(x).chunk(2, dim=-1)
        return self.w2(F.silu(gate) * up)


class Block(nn.Module):
    def __init__(self, cfg):
        super().__init__()
        self.norm1 = RMSNorm(cfg.vision_dim)
        self.attn = Attention(cfg)
        self.norm2 = RMSNorm(cfg.vision_dim)
        self.mlp = MLP(cfg)

    def forward(self, x, cos, sin):
        x = x + self.attn(self.norm1(x), cos, sin)
        return x + self.mlp(self.norm2(x))


class ViT(nn.Module):
    def __init__(self, cfg):
        super().__init__()
        self.rope_dim = cfg.vision_dim // cfg.vision_n_heads // 2
        self.rope_theta = cfg.vision_rope_theta
        self.patch_embed = nn.Module()
        self.patch_embed.proj = nn.Linear(
            3 * cfg.vision_patch_size**2, cfg.vision_dim
        )
        self.blocks = nn.ModuleList([Block(cfg) for _ in range(cfg.vision_n_layers)])
        self.norm = RMSNorm(cfg.vision_dim)

    def forward(self, patches, n_h, n_w):
        x = self.patch_embed.proj(patches.flatten(1))
        cos, sin = vision_cos_sin(n_h, n_w, self.rope_dim, self.rope_theta)
        cos = cos.to(x.device)
        sin = sin.to(x.device)
        for block in self.blocks:
            x = block(x, cos, sin)
        return self.norm(x)


class Aligner(nn.Module):
    def __init__(self, cfg):
        super().__init__()
        self.ratio = cfg.vision_downsample_ratio
        self.w1 = nn.Linear(cfg.vision_dim * self.ratio**2, cfg.hidden_size)
        self.w2 = nn.Linear(cfg.hidden_size, cfg.hidden_size)

    def forward(self, x, n_h, n_w):
        r = self.ratio
        x = x.view(n_h, n_w, -1).permute(2, 0, 1)
        x = F.pad(x, (0, -n_w % r, 0, -n_h % r))
        x = F.unfold(x.unsqueeze(0), r, stride=r).squeeze(0).transpose(0, 1)
        return self.w2(F.gelu(self.w1(x)))


def grid_tokens(height, width, patch, ratio):
    h = math.ceil((height // patch) / ratio)
    w = math.ceil((width // patch) / ratio)
    count = h * (w + 1) + 2
    if h % 2 == 1:
        count += w + 1
    count += (h + 1) // 2 * (w + 1) % 2 * 2
    return h, w, count


def solve_resize(height, width, patch, ratio, budget):
    aspect = height / width
    max_w_f = math.sqrt((budget - 2) / aspect + 0.25) - 0.5
    max_h_f = max_w_f * aspect
    if max_w_f < 1.0:
        max_w = 1
        max_h = (budget - 2) // 2
        max_h -= max_h % 2
        best_w, best_h = max_w * patch * ratio, max_h * patch * ratio
    elif max_h_f < 2.0:
        max_h = 2
        max_w = (budget - 2) // max_h - 1
        best_w, best_h = max_w * patch * ratio, max_h * patch * ratio
    else:
        max_w, max_h = math.floor(max_w_f), math.floor(max_h_f)
        max_h -= max_h % 2
        beta = min(max_w * patch * ratio / width, max_h * patch * ratio / height)
        best_w = math.floor(width * beta / patch) * patch
        best_h = math.floor(height * beta / patch) * patch
    h, w, count = grid_tokens(best_h, best_w, patch, ratio)
    return h, w, best_h, best_w, count


def safe_resize(height, width, best_h, best_w, patch, ratio, max_tokens):
    limit = max_tokens - (COMPRESS_PAD_TO - 1)
    h, w, count = grid_tokens(best_h, best_w, patch, ratio)
    budget = limit
    while count > limit:
        h, w, best_h, best_w, count = solve_resize(
            height, width, patch, ratio, budget
        )
        budget -= 1
    return h, w, best_h, best_w


def preprocess(image: Image.Image, cfg):
    patch = cfg.vision_patch_size
    width, height = image.size
    if width > height * cfg.vision_max_wh_ratio:
        width = height * cfg.vision_max_wh_ratio
    if 0 < width * height < cfg.vision_min_pixels:
        scale = math.sqrt(cfg.vision_min_pixels / (width * height))
        width, height = int(width * scale), int(height * scale)
    best_w, best_h = math.ceil(width / patch) * patch, math.ceil(height / patch) * patch
    llm_h, llm_w, best_h, best_w = safe_resize(
        height,
        width,
        best_h,
        best_w,
        patch,
        cfg.vision_downsample_ratio,
        cfg.vision_max_n_token,
    )
    vit_h, vit_w = best_h // patch, best_w // patch
    if image.width >= cfg.vision_max_wh_ratio * image.height:
        image = image.resize((best_w, best_h))
    else:
        image = ImageOps.pad(image, (best_w, best_h), color=(127, 127, 127))
    x = torch.from_numpy(np.asarray(image, dtype=np.float32).copy()).permute(2, 0, 1) / 255
    x = ((x - 0.5) / 0.5).to(torch.bfloat16)
    patches = (
        x.reshape(3, vit_h, patch, vit_w, patch)
        .permute(1, 3, 0, 2, 4)
        .reshape(vit_h * vit_w, 3, patch, patch)
    )
    return patches, vit_h, vit_w, llm_h, llm_w


def image_block(llm_h: int, llm_w: int, start_pos: int):
    compress_pad = COMPRESS_PAD_TO - 1 - start_pos % COMPRESS_PAD_TO
    pad_h = llm_h % 2
    rows, row_len = llm_h + pad_h, llm_w + 1
    pad_last = rows // 2 * row_len % 2 * 2
    types = torch.tensor(
        ([IMAGE] * llm_w + [IMAGE_NEW_LINE]) * llm_h
        + [IMAGE_PAD] * (row_len * pad_h),
        dtype=torch.int64,
    )
    order = torch.arange(rows * row_len).view(rows // 2, 2, row_len).transpose(1, 2).reshape(-1)
    image_idx = torch.full((rows * row_len,), -1, dtype=torch.int64)
    image_idx.view(rows, row_len)[:llm_h, :llm_w] = torch.arange(llm_h * llm_w).view(llm_h, llm_w)
    perm = image_idx[order]
    perm = perm[perm >= 0]
    types = torch.cat(
        [
            torch.full((compress_pad,), IMAGE_PAD, dtype=torch.int64),
            torch.tensor([IMAGE_START]),
            types[order],
            torch.full((pad_last,), IMAGE_PAD, dtype=torch.int64),
            torch.tensor([IMAGE_END]),
        ]
    )
    return types, perm


def read_image_url(value: str) -> bytes:
    if not value.startswith("data:image/"):
        raise ValueError("only data:image URLs are accepted")
    header, sep, payload = value.partition(",")
    if not sep:
        raise ValueError("malformed image data URL")
    data = base64.b64decode(payload, validate=";base64" in header.lower()) \
        if ";base64" in header.lower() else unquote_to_bytes(payload)
    if not data or len(data) > MAX_IMAGE_BYTES:
        raise ValueError("image is empty or exceeds the size limit")
    return data


def load_models(tower_path: Path, config_path: Path, device: torch.device):
    raw = json.loads(config_path.read_text(encoding="utf-8"))
    cfg = SimpleNamespace(**raw)
    state = load_file(str(tower_path), device="cpu")
    vision = ViT(cfg)
    aligner = Aligner(cfg)
    vision.load_state_dict(
        {name.removeprefix("vision."): value for name, value in state.items() if name.startswith("vision.")},
        strict=True,
    )
    aligner.load_state_dict(
        {name.removeprefix("aligner."): value for name, value in state.items() if name.startswith("aligner.")},
        strict=True,
    )
    controls = torch.stack(
        [state["image_start"], state["image_pad"], state["image_pad"], state["image_newline"], state["image_end"]]
    )
    for module in (vision, aligner):
        module.requires_grad_(False)
        module.to(device=device, dtype=torch.bfloat16).eval()
    return cfg, vision, aligner, controls.to(device=device, dtype=torch.bfloat16)


def write_payload(path: Path, routes: np.ndarray, rows: np.ndarray, digest: bytes):
    routes = np.ascontiguousarray(routes, dtype="<i4")
    rows = np.ascontiguousarray(rows, dtype="<f4")
    header = HEADER.pack(
        MAGIC,
        VERSION,
        HEADER.size,
        rows.shape[0],
        rows.shape[1],
        FLAG_VISUAL_ROUTING,
        -1,
        -1,
        0,
        digest,
    )
    with path.open("wb") as handle:
        handle.write(header)
        handle.write(routes.tobytes())
        handle.write(rows.tobytes())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image-url-file", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tower", required=True, type=Path)
    parser.add_argument("--adapter", required=True, type=Path, help="Vision-Exp config.json")
    parser.add_argument("--start-pos", type=int, default=0)
    parser.add_argument("--device", choices=("auto", "mps", "cpu"), default="auto")
    args = parser.parse_args()

    device_name = args.device
    if device_name == "auto":
        device_name = "mps" if torch.backends.mps.is_available() else "cpu"
    device = torch.device(device_name)
    started = time.perf_counter()
    image_bytes = read_image_url(args.image_url_file.read_text(encoding="utf-8"))
    image = Image.open(io.BytesIO(image_bytes)).convert("RGB")
    cfg, vision, aligner, controls = load_models(args.tower, args.adapter, device)
    patches, vit_h, vit_w, llm_h, llm_w = preprocess(image, cfg)
    types, perm = image_block(llm_h, llm_w, args.start_pos)
    with torch.inference_mode():
        encoded = aligner(vision(patches.to(device), vit_h, vit_w), vit_h, vit_w)
        rows = controls[types.to(device)].clone()
        rows[types.to(device) == IMAGE] = encoded[perm.to(device)]
        if device.type == "mps":
            torch.mps.synchronize()
        rows_np = rows.float().cpu().numpy()
    if rows_np.shape[0] > cfg.vision_max_n_token or rows_np.shape[1] != HIDDEN_SIZE:
        raise RuntimeError(f"unexpected Vision-Exp output shape {rows_np.shape}")
    if not np.isfinite(rows_np).all():
        raise RuntimeError("encoder produced non-finite rows")
    routes = np.full((rows_np.shape[0],), IMAGE_TOKEN_ID, dtype=np.int32)
    digest = hashlib.sha256(image_bytes).digest()
    write_payload(args.output, routes, rows_np, digest)
    metadata = {
        "format": "DS4VEMB1",
        "variant": "deepseek-v4-flash-vision-exp-native",
        "image_sha256": digest.hex(),
        "tower": str(args.tower.resolve()),
        "config": str(args.adapter.resolve()),
        "tokens": int(rows_np.shape[0]),
        "start_pos": args.start_pos,
        "vit_grid": [vit_h, vit_w],
        "llm_grid": [llm_h, llm_w],
        "device": str(device),
        "elapsed_s": time.perf_counter() - started,
    }
    Path(str(args.output) + ".json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(metadata))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
