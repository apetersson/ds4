#!/usr/bin/env python3
"""Atomically own the global lane while supervising the DS-001.11 server."""

import fcntl
import os
import signal
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
GLOBAL_LOCK = "/private/tmp/ds4.lock"
SERVER_LOCK = "/private/tmp/ds00111-server.lock"
OUTPUT = Path("/private/tmp/ds00111-review-fix-live")
MODEL_ROOT = Path(
    "/Volumes/Samsung_4TB/models/DeepSeek-V4-Flash-0731-Abliterated-Vision"
)
args = [
    str(ROOT / "ds4-server"),
    "-hf", "apetersson/DeepSeek-V4-Flash-0731-Abliterated-Vision:Headroom128-IQ2_XXS",
    "--hf-cache-dir", "/Volumes/DS00111HF",
    "--metal", "--ctx", "2048", "--tokens", "192",
    "--vision-python", "/private/tmp/dsv4-vision-venv/bin/python",
    "--vision-encoder", str(MODEL_ROOT / "tools/encode_flycockpit.py"),
    "--host", "127.0.0.1", "--port", "18082",
]

OUTPUT.mkdir(parents=True, exist_ok=True)
fd = os.open(GLOBAL_LOCK, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
identity = os.fstat(fd)
fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
os.write(fd, f"DS-001.11 pid={os.getpid()} HF vision matrix\n".encode("ascii"))
os.fsync(fd)
print(f"lane_acquired pid={os.getpid()} inode={identity.st_ino}", flush=True)

env = os.environ.copy()
env.update({
    "HF_ENDPOINT": "http://127.0.0.1:18081",
    "DS4_LOCK_FILE": SERVER_LOCK,
    "DS4_VISION_TEST_REAL_SPAN": str(
        MODEL_ROOT / "eval/orion_dashboard.fly.ds4v"
    ),
    "DS4_VISION_TEST_ZERO_SPAN": str(
        MODEL_ROOT / "eval/orion_dashboard.zero.ds4v"
    ),
    "DS4_VISION_TEST_SHUFFLE_SPAN": str(
        MODEL_ROOT / "eval/orion_dashboard.shuffle.ds4v"
    ),
})
child = None
return_code = 1


def forward(signum, _frame):
    if child is not None and child.poll() is None:
        child.send_signal(signum)


signal.signal(signal.SIGINT, forward)
signal.signal(signal.SIGTERM, forward)

try:
    with (OUTPUT / "server.log").open("wb", buffering=0) as log:
        child = subprocess.Popen(
            args, env=env, cwd=ROOT, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        assert child.stdout is not None
        for chunk in iter(lambda: child.stdout.read(65536), b""):
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            log.write(chunk)
        return_code = child.wait()
finally:
    try:
        os.unlink(SERVER_LOCK)
    except FileNotFoundError:
        pass
    current = os.stat(GLOBAL_LOCK, follow_symlinks=False)
    if current.st_ino != identity.st_ino:
        raise RuntimeError("global lane lock inode changed while held")
    os.unlink(GLOBAL_LOCK)
    os.close(fd)
    print(f"lane_released pid={os.getpid()} inode={identity.st_ino}", flush=True)

raise SystemExit(return_code)
