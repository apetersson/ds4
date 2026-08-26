#!/usr/bin/env python3
"""Capture the literal DS-001.11 direct-server causal matrix on localhost."""

import argparse
import base64
import hashlib
import json
import time
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen


def send(url, body=None, headers=None, expected=200):
    encoded = None if body is None else json.dumps(
        body, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8")
    request = Request(url, data=encoded, headers=headers or {},
                      method="GET" if body is None else "POST")
    started = time.monotonic()
    try:
        with urlopen(request, timeout=300) as response:
            status = response.status
            payload = response.read()
    except HTTPError as exc:
        status = exc.code
        payload = exc.read()
    elapsed = time.monotonic() - started
    if status != expected:
        raise RuntimeError(
            f"{url} returned HTTP {status}, expected {expected}: {payload!r}"
        )
    return status, payload, elapsed, encoded


def write_json(path, payload):
    path.write_text(json.dumps(json.loads(payload), indent=2,
                               ensure_ascii=False) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--rubric", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--base-url", default="http://127.0.0.1:18082/v1")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    image_bytes = args.image.read_bytes()
    image_url = "data:image/png;base64," + base64.b64encode(image_bytes).decode()
    rubric = args.rubric.read_text(encoding="utf-8").strip()
    common = {
        "model": "deepseek-chat",
        "messages": [{
            "role": "user",
            "content": [
                {"type": "image_url", "image_url": {"url": image_url}},
                {"type": "text", "text": rubric},
            ],
        }],
        "temperature": 0,
        "max_tokens": 192,
        "stream": False,
    }
    transcript = [
        f"image={args.image}",
        f"image_sha256={hashlib.sha256(image_bytes).hexdigest()}",
        f"image_bytes={len(image_bytes)}",
        "matrix=encoder,real,zero,shuffle",
    ]

    status, payload, elapsed, _ = send(f"{args.base_url}/models")
    write_json(args.output / "models.json", payload)
    transcript.append(f"models http={status} elapsed={elapsed:.3f}s")

    text_body = {
        "model": "deepseek-chat",
        "messages": [{"role": "user", "content":
                      "Reply with exactly: DS4 VISION TEXT OK"}],
        "temperature": 0,
        "max_tokens": 16,
        "stream": False,
    }
    status, payload, elapsed, request_bytes = send(
        f"{args.base_url}/chat/completions", text_body,
        {"Content-Type": "application/json"})
    (args.output / "direct-text-request.json").write_bytes(request_bytes + b"\n")
    write_json(args.output / "direct-text-response.json", payload)
    transcript.append(f"text http={status} elapsed={elapsed:.3f}s")

    for name, control in (("encoder", None), ("real", "real"),
                          ("zero", "zero"), ("shuffle", "shuffle")):
        headers = {"Content-Type": "application/json"}
        if control:
            headers["X-DS4-Vision-Control"] = control
        status, payload, elapsed, request_bytes = send(
            f"{args.base_url}/chat/completions", common, headers)
        (args.output / f"direct-{name}-request.json").write_bytes(
            request_bytes + b"\n")
        write_json(args.output / f"direct-{name}-response.json", payload)
        content = json.loads(payload)["choices"][0]["message"]["content"]
        transcript.extend((
            f"CASE={name} header={control or 'none'} http={status} "
            f"elapsed={elapsed:.3f}s",
            content,
            "---",
        ))

    unsafe = json.loads(json.dumps(common))
    unsafe["messages"][0]["content"][0]["image_url"]["url"] = (
        "file:///private/etc/hosts"
    )
    status, payload, elapsed, request_bytes = send(
        f"{args.base_url}/chat/completions", unsafe,
        {"Content-Type": "application/json"}, expected=400)
    (args.output / "negative-file-url-request.json").write_bytes(
        request_bytes + b"\n")
    write_json(args.output / "negative-file-url-response.json", payload)
    transcript.append(f"negative-file-url http={status} elapsed={elapsed:.3f}s")

    legacy = {
        "model": "deepseek-chat",
        "ds4_embedding_span_path": "/private/etc/hosts",
        "messages": [{"role": "user", "content": "test"}],
    }
    status, payload, elapsed, request_bytes = send(
        f"{args.base_url}/chat/completions", legacy,
        {"Content-Type": "application/json"}, expected=400)
    (args.output / "negative-public-span-request.json").write_bytes(
        request_bytes + b"\n")
    write_json(args.output / "negative-public-span-response.json", payload)
    transcript.append(f"negative-public-span http={status} elapsed={elapsed:.3f}s")
    (args.output / "direct-matrix.txt").write_text(
        "\n".join(transcript) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
