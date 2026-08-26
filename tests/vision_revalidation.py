#!/usr/bin/env python3
"""Replay the complete DS-002.03 direct-server and vanilla-Pi matrix."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


BASE = "27eb58a0aec6829f80b5eaaba710fc344ed4384f"
BRANCH = "worker/DS-002.03-vision-revalidate"
CATALOG = Path(
    "/Volumes/Samsung_4TB/models/DeepSeek-V4-Flash-0731-Abliterated-Vision"
)
PI = Path("/Users/andreas/.nvm/versions/node/v24.14.0/bin/pi")
SERVER = Path(__file__).resolve().parents[1] / "ds4-server"
TOWER = CATALOG / "Vision-BF16" / "DeepEncoderV2-BF16.safetensors"
PROJECTOR = (
    CATALOG / "Vision-BF16" / "DeepSeek-V4-0731-Projector-BF16.safetensors"
)
PYTHON = Path("/private/tmp/dsv4-vision-venv/bin/python")
ENCODER = CATALOG / "tools" / "encode_flycockpit.py"
MUTATOR = CATALOG / "tools" / "mutate_ds4v.py"
PI_CATALOG = CATALOG / "runtime" / "pi" / "models.json"
IMAGE_257 = Path(
    "/Volumes/Samsung_4TB/models/Qwen3.6-27B-DFlash/assets/speedup.png"
)
IMAGE_769 = CATALOG / "eval" / "fixtures" / "orion_dashboard.png"
SPAN_257_REAL = CATALOG / "eval" / "speedup-safe.fly.ds4v"
SPAN_257_ZERO = Path("/private/tmp/ds00203-speedup.zero.ds4v")
SPAN_257_SHUFFLE = Path("/private/tmp/ds00203-speedup.shuffle.ds4v")
SPAN_769_REAL = CATALOG / "eval" / "orion_dashboard.fly.ds4v"
SPAN_769_ZERO = CATALOG / "eval" / "orion_dashboard.zero.ds4v"
SPAN_769_SHUFFLE = CATALOG / "eval" / "orion_dashboard.shuffle.ds4v"
ROUTE_TOKEN = "<｜image｜>"
QUESTION_257 = (
    "Read this chart. Give its title, the three legend series, and the highest "
    "plotted speedup value."
)
QUESTION_769 = (
    "Read the dashboard precisely: title; throughput, latency, and memory values; "
    "the four request-flow nodes in order; and the status."
)
TEXT_PROMPT = (
    "Reply with exactly DS4 VISION TEXT OK and no other text."
)
TITLE_PROMPT = "What exact title is shown? Answer with only the title."

EXPECTED_HASHES = {
    str(TOWER): "9dcf6803d4c6b63acc4008bc2409e599a2ab6e3886e241f1727f61550c300df5",
    str(PROJECTOR): "77f8be7a44a93aeec05f7294d51d72bed2dc4328770ba214186bcc671480db77",
    str(IMAGE_257): "0f1b16c20ccce188749a0e9054d596d35986395c55cbe2ab6b79eb75c51c8045",
    str(IMAGE_769): "78a1d7b63e9e8f8ea363a16350e503845f656e598016a727b312b91ce96a2f11",
    str(SPAN_257_REAL): "6686c5c4b291a0b9dfec8d0e21cea5ec0a9ae6259e1c98963277d65a114e3a3c",
    str(SPAN_257_ZERO): "ba54633596401fe647c54e6ce0128f97c6b7ae45ef163345657425e7225286e9",
    str(SPAN_257_SHUFFLE): "2b9cda7b10396601a01da305427150a6729a2768ac1c565b9e52c71c5b433248",
    str(SPAN_769_REAL): "a8979593707d5a10ccaa40806de554e743bd69f307416c8813097a95acfff14a",
    str(SPAN_769_ZERO): "5a065e9445f973c328c8cea52ecc8be85c586eb1f54387cdb7c6e76c5ca2b54e",
    str(SPAN_769_SHUFFLE): "fa67dc8fe51b2008837b29155779b12b16735ffc61f26fa54470582c10b2f26b",
    str(PI_CATALOG): "f70750f6f83aba14e998e00dd29ea43cf7427df0351c052edaf40ce56ab72c68",
}

RECEIVERS = {
    "headroom": {
        "path": CATALOG
        / "Headroom128-IQ2_XXS"
        / "DeepSeek-V4-Flash-0731-Abliterated-Vision-Headroom128-IQ2_XXS.gguf",
        "size": 86_720_111_776,
        "sha256": "162e2b5e245ca0927282111064c8dfbd58894cabd51958322161814eb9addbb6",
        "quality_flag": False,
    },
    "quality": {
        "path": CATALOG
        / "Quality128-IQ2_XXS_XL"
        / "DeepSeek-V4-Flash-0731-Abliterated-Vision-Quality128-IQ2_XXS_XL.gguf",
        "size": 102_826_239_136,
        "sha256": "18a1a22d31941ce823f698ccf7fe6f4de78b6e05854bba22570c5b6cf6c1ad55",
        "quality_flag": True,
    },
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command_record(argv: list[str], **kwargs: object) -> dict[str, object]:
    started = time.monotonic()
    result = subprocess.run(argv, text=True, capture_output=True, check=False, **kwargs)
    return {
        "argv": argv,
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
        "elapsed_seconds": round(time.monotonic() - started, 6),
    }


def post_json(base_url: str, payload: dict[str, object], timeout: float) -> dict[str, object]:
    url = base_url.rstrip("/") + "/v1/chat/completions"
    encoded = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode()
    request = urllib.request.Request(
        url,
        data=encoded,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    started = time.monotonic()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            raw = response.read()
            status = response.status
            response_headers = dict(response.headers.items())
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        raise RuntimeError(f"HTTP {exc.code}: {raw.decode(errors='replace')}") from exc
    parsed = json.loads(raw)
    return {
        "url": url,
        "request": payload,
        "request_body_sha256": hashlib.sha256(encoded).hexdigest(),
        "http_status": status,
        "response_headers": response_headers,
        "raw_response_utf8": raw.decode("utf-8"),
        "response": parsed,
        "elapsed_seconds": round(time.monotonic() - started, 6),
    }


def make_text_payload() -> dict[str, object]:
    return {
        "model": "deepseek-chat",
        "messages": [{"role": "user", "content": TEXT_PROMPT}],
        "temperature": 0,
        "seed": 4242,
        "max_tokens": 32,
    }


def make_image_url_payload(image: Path, question: str) -> dict[str, object]:
    return {
        "model": "deepseek-chat",
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "image_url", "image_url": {"url": str(image)}},
                    {"type": "text", "text": question},
                ],
            }
        ],
        "temperature": 0,
        "seed": 4242,
        "max_tokens": 128,
    }


def make_span_payload(rows: int, span: Path, question: str) -> dict[str, object]:
    return {
        "model": "deepseek-chat",
        "messages": [
            {"role": "user", "content": ROUTE_TOKEN * rows + "\n" + question}
        ],
        "ds4_embedding_span_path": str(span),
        "temperature": 0,
        "seed": 4242,
        "max_tokens": 96,
    }


def make_alias_payload(model: str, thinking: object | None) -> dict[str, object]:
    payload = {
        "model": model,
        "messages": [
            {"role": "user", "content": ROUTE_TOKEN * 769 + TITLE_PROMPT}
        ],
        "ds4_embedding_span_path": str(SPAN_769_REAL),
        "temperature": 0,
        "seed": 4242,
        "max_tokens": 256,
    }
    if thinking is not None:
        payload["thinking"] = thinking
    return payload


def ensure_control_payloads() -> list[dict[str, object]]:
    commands = [
        [
            str(PYTHON), str(MUTATOR), "--input", str(SPAN_257_REAL),
            "--output", str(SPAN_257_ZERO), "--mode", "zero",
        ],
        [
            str(PYTHON), str(MUTATOR), "--input", str(SPAN_257_REAL),
            "--output", str(SPAN_257_SHUFFLE), "--mode", "shuffle", "--seed", "42",
        ],
    ]
    records = [command_record(command) for command in commands]
    failures = [item for item in records if item["returncode"] != 0]
    if failures:
        raise RuntimeError(json.dumps(failures, indent=2))
    return records


def artifact_record(path: Path, *, expected_sha: str | None = None) -> dict[str, object]:
    actual = sha256(path)
    if expected_sha and actual != expected_sha:
        raise RuntimeError(f"SHA256 mismatch for {path}: {actual} != {expected_sha}")
    return {"path": str(path), "size": path.stat().st_size, "sha256": actual}


def receiver_record(profile: str) -> dict[str, object]:
    expected = RECEIVERS[profile]
    path = expected["path"]
    assert isinstance(path, Path)
    actual_size = path.stat().st_size
    if actual_size != expected["size"]:
        raise RuntimeError(f"receiver size mismatch: {actual_size} != {expected['size']}")
    return {
        "path": str(path),
        "size": actual_size,
        "manifest_sha256": expected["sha256"],
        "hash_source": "catalog manifest; full GGUF hash was verified before live replay",
        "quality_flag": expected["quality_flag"],
    }


def server_command(profile: str) -> list[str]:
    receiver = RECEIVERS[profile]
    argv = [
        str(SERVER), "--model", str(receiver["path"]), "--metal",
    ]
    if receiver["quality_flag"]:
        argv.append("--quality")
    argv.extend([
        "--ctx", "2048", "--tokens", "128",
        "--vision-python", str(PYTHON),
        "--vision-encoder", str(ENCODER),
        "--vision-tower", str(TOWER),
        "--vision-adapter", str(PROJECTOR),
        "--host", "127.0.0.1", "--port", "18082",
    ])
    return argv


def pi_record(base_url: str, model: str, thinking: str, timeout: float) -> dict[str, object]:
    argv = [
        str(PI), "--provider", "ds4-local", "--model", model,
        "--thinking", thinking, "--no-tools", "--no-session",
        "--no-context-files", "--no-extensions", "--no-skills",
        "--system-prompt", "Answer image questions precisely.",
        "-p", f"@{IMAGE_769}", TITLE_PROMPT,
    ]
    env = os.environ.copy()
    env.update({
        "PI_CODING_AGENT_DIR": str(CATALOG / "runtime" / "pi"),
        "PI_OFFLINE": "1",
    })
    record = command_record(argv, env=env, timeout=timeout)
    record["environment"] = {
        "PI_CODING_AGENT_DIR": env["PI_CODING_AGENT_DIR"],
        "PI_OFFLINE": env["PI_OFFLINE"],
    }
    if record["returncode"] != 0:
        raise RuntimeError(json.dumps(record, indent=2))
    return record


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=tuple(RECEIVERS), required=True)
    parser.add_argument("--base-url", default="http://127.0.0.1:18082")
    parser.add_argument("--preflight", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=1800.0)
    args = parser.parse_args()

    preflight = json.loads(args.preflight.read_text())
    if (
        preflight.get("profile") != args.profile
        or preflight.get("phase") != "preflight"
        or not preflight.get("launch_allowed")
    ):
        raise RuntimeError("the profile-matched preflight does not authorize launch")
    captured_at = dt.datetime.fromisoformat(preflight["captured_at"])
    preflight_age = dt.datetime.now(dt.timezone.utc) - captured_at
    if preflight_age.total_seconds() > 600:
        raise RuntimeError(
            f"preflight is {preflight_age.total_seconds():.1f}s old; maximum is 600s"
        )

    control_commands = ensure_control_payloads()
    artifacts = {}
    for path_text, expected in EXPECTED_HASHES.items():
        path = Path(path_text)
        artifacts[path_text] = artifact_record(path, expected_sha=expected)

    repo = Path(__file__).resolve().parents[1]
    os.chdir(repo)
    branch = subprocess.check_output(["git", "branch", "--show-current"], text=True).strip()
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    if branch != BRANCH:
        raise RuntimeError(f"branch mismatch: {branch} != {BRANCH}")
    if subprocess.run(["git", "merge-base", "--is-ancestor", BASE, "HEAD"]).returncode:
        raise RuntimeError("expected base is not an ancestor of HEAD")
    if preflight["git"]["head"] != head or preflight["git"]["branch"] != branch:
        raise RuntimeError("preflight git identity does not match the live replay")

    pi_catalog = json.loads(PI_CATALOG.read_text())
    provider = pi_catalog["providers"]["ds4-local"]
    catalog_base_url = provider["baseUrl"].removesuffix("/v1")
    if catalog_base_url != args.base_url.rstrip("/"):
        raise RuntimeError(
            f"Pi catalog base URL mismatch: {catalog_base_url} != {args.base_url}"
        )
    aliases = {item["id"]: item for item in provider["models"]}
    if aliases["deepseek-chat"]["reasoning"] is not False:
        raise RuntimeError("Pi catalog deepseek-chat must disable reasoning")
    if aliases["deepseek-reasoner"]["reasoning"] is not True:
        raise RuntimeError("Pi catalog deepseek-reasoner must enable reasoning")

    requests: dict[str, object] = {}
    for index in range(1, 3):
        requests[f"text_repeat_{index}"] = post_json(
            args.base_url, make_text_payload(), args.timeout
        )
    requests["image_url_257_real"] = post_json(
        args.base_url, make_image_url_payload(IMAGE_257, QUESTION_257), args.timeout
    )
    requests["image_url_769_real"] = post_json(
        args.base_url, make_image_url_payload(IMAGE_769, QUESTION_769), args.timeout
    )

    span_layouts = (
        (257, QUESTION_257, {
            "real": SPAN_257_REAL,
            "zero": SPAN_257_ZERO,
            "shuffle_seed_42": SPAN_257_SHUFFLE,
        }),
        (769, QUESTION_769, {
            "real": SPAN_769_REAL,
            "zero": SPAN_769_ZERO,
            "shuffle_seed_42": SPAN_769_SHUFFLE,
        }),
    )
    for rows, question, variants in span_layouts:
        for name, span in variants.items():
            requests[f"span_{rows}_{name}"] = post_json(
                args.base_url, make_span_payload(rows, span, question), args.timeout
            )

    alias_cases = (
        ("chat_default", "deepseek-chat", None),
        ("reasoner_disabled", "deepseek-reasoner", {"type": "disabled"}),
        ("reasoner_default", "deepseek-reasoner", None),
        ("chat_enabled", "deepseek-chat", {"type": "enabled"}),
    )
    for name, model, thinking in alias_cases:
        requests[f"alias_{name}"] = post_json(
            args.base_url, make_alias_payload(model, thinking), args.timeout
        )

    pi_results = {
        "intended_chat_off": pi_record(
            args.base_url, "deepseek-chat", "off", args.timeout
        ),
        "defect_reasoner_high": pi_record(
            args.base_url, "deepseek-reasoner", "high", args.timeout
        ),
    }

    evidence = {
        "schema": "ds4.vision-revalidation.v1",
        "captured_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "profile": args.profile,
        "workspace": str(repo),
        "branch": branch,
        "head": head,
        "expected_base": BASE,
        "base_url": args.base_url,
        "server_command": server_command(args.profile),
        "receiver": receiver_record(args.profile),
        "artifacts": artifacts,
        "pi_catalog": {
            "path": str(PI_CATALOG),
            "provider": "ds4-local",
            "base_url": provider["baseUrl"],
            "aliases": aliases,
        },
        "control_generation": control_commands,
        "preflight_path": str(args.preflight.resolve()),
        "preflight_sha256": sha256(args.preflight),
        "preflight": preflight,
        "request_contract": {
            "route_token": ROUTE_TOKEN,
            "separator": "one LF byte for causal trios; no separator for alias cases",
            "questions": {"257": QUESTION_257, "769": QUESTION_769},
            "text_prompt": TEXT_PROMPT,
            "title_prompt": TITLE_PROMPT,
        },
        "requests": requests,
        "vanilla_pi": pi_results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(evidence, indent=2, ensure_ascii=False) + "\n")
    summary = {
        "output": str(args.output.resolve()),
        "profile": args.profile,
        "request_count": len(requests),
        "pi_count": len(pi_results),
        "output_sha256": sha256(args.output),
    }
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
