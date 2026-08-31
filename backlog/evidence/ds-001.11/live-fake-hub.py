#!/usr/bin/env python3
"""Pinned metadata-only Hub for the already verified DS-001.11 cache."""

import json
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit


REVISION = "0123456789abcdef0123456789abcdef01234567"
REPOSITORY = "apetersson/DeepSeek-V4-Flash-0731-Abliterated-Vision"
ROOT = Path(__file__).resolve().parents[3]
FIXTURE = ROOT / "tests/fixtures/hf/variants-v2.json"
LOG = Path("/private/tmp/ds00111-review-fix-live/hub-requests.txt")

manifest = json.loads(FIXTURE.read_text(encoding="utf-8"))
manifest["repository"] = REPOSITORY
manifest["variants"] = manifest["variants"][:1]
variant = manifest["variants"][0]
identities = {
    "receiver": (
        86720111776,
        "162e2b5e245ca0927282111064c8dfbd58894cabd51958322161814eb9addbb6",
    ),
    "tower": (
        906533408,
        "9dcf6803d4c6b63acc4008bc2409e599a2ab6e3886e241f1727f61550c300df5",
    ),
    "projector": (
        40919752,
        "77f8be7a44a93aeec05f7294d51d72bed2dc4328770ba214186bcc671480db77",
    ),
    "config": (
        658,
        "2c1295c110b1b7ac2b238c451f34a1112aa4296052d8119e703fd58a4c193fbb",
    ),
}
artifacts = {
    "receiver": variant["receiver"],
    "tower": variant["ds4_vision"]["tower"],
    "projector": variant["ds4_vision"]["projector"],
    "config": variant["ds4_vision"]["config"],
}
for role, artifact in artifacts.items():
    artifact["bytes"], artifact["sha256"] = identities[role]
manifest_body = json.dumps(
    manifest, sort_keys=True, separators=(",", ":")
).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        message = fmt % args
        line = f"{datetime.now(timezone.utc).isoformat()} {message}\n"
        print(message, flush=True)
        with LOG.open("a", encoding="utf-8") as stream:
            stream.write(line)

    def send_bytes(self, status, payload, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        path = unquote(urlsplit(self.path).path)
        if path == f"/api/models/{REPOSITORY}":
            payload = json.dumps({"sha": REVISION}).encode("utf-8")
            self.send_bytes(200, payload, "application/json")
        elif path == f"/{REPOSITORY}/resolve/{REVISION}/variants.json":
            self.send_bytes(200, manifest_body, "application/json")
        else:
            self.send_bytes(404, b"cached-artifacts-only", "text/plain")

LOG.parent.mkdir(parents=True, exist_ok=True)
LOG.write_text("", encoding="utf-8")
ThreadingHTTPServer(("127.0.0.1", 18081), Handler).serve_forever()
