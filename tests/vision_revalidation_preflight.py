#!/usr/bin/env python3
"""Record and enforce the DS-002.03 single-instance launch preflight."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import subprocess
import sys
from pathlib import Path


BASE = "27eb58a0aec6829f80b5eaaba710fc344ed4384f"
BRANCH = "worker/DS-002.03-vision-revalidate"
CATALOG = Path(
    "/Volumes/Samsung_4TB/models/DeepSeek-V4-Flash-0731-Abliterated-Vision"
)
LOCK = Path("/private/tmp/ds4.lock")
PORT = 18082
MODELS = (
    CATALOG
    / "Headroom128-IQ2_XXS"
    / "DeepSeek-V4-Flash-0731-Abliterated-Vision-Headroom128-IQ2_XXS.gguf",
    CATALOG
    / "Quality128-IQ2_XXS_XL"
    / "DeepSeek-V4-Flash-0731-Abliterated-Vision-Quality128-IQ2_XXS_XL.gguf",
    CATALOG / "Vision-BF16" / "DeepEncoderV2-BF16.safetensors",
    CATALOG
    / "Vision-BF16"
    / "DeepSeek-V4-0731-Projector-BF16.safetensors",
)


def run(command: list[str], *, check: bool = False) -> dict[str, object]:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    record: dict[str, object] = {
        "argv": command,
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }
    if check and result.returncode != 0:
        raise RuntimeError(json.dumps(record, indent=2))
    return record


def output(record: dict[str, object]) -> str:
    return str(record["stdout"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=("headroom", "quality"), required=True)
    parser.add_argument(
        "--phase", choices=("preflight", "postflight"), default="preflight"
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    os.chdir(repo)
    git = {
        "pwd": str(repo),
        "root": output(run(["git", "rev-parse", "--show-toplevel"], check=True)).strip(),
        "branch": output(run(["git", "branch", "--show-current"], check=True)).strip(),
        "head": output(run(["git", "rev-parse", "HEAD"], check=True)).strip(),
        "status_porcelain": output(run(["git", "status", "--porcelain"])),
        "base_is_ancestor": run(
            ["git", "merge-base", "--is-ancestor", BASE, "HEAD"]
        )["returncode"]
        == 0,
    }

    ps = run(["ps", "-ww", "-axo", "pid=,ppid=,comm=,args="], check=True)
    ps_lines = output(ps).splitlines()
    ds4_processes = []
    omlx_processes = []
    pi_processes = []
    for line in ps_lines:
        columns = line.strip().split(None, 3)
        if len(columns) < 3:
            continue
        comm = Path(columns[2]).name
        args_text = columns[3] if len(columns) == 4 else ""
        if comm in {"ds4", "ds4-server"}:
            ds4_processes.append(line)
        if re.search(r"(?:^|[/\s])(?:omlx|omlx\.server)(?:[/\s]|$)", args_text, re.I):
            omlx_processes.append(line)
        if comm == "pi" or re.search(r"pi-coding-agent/.*/cli\.js", args_text):
            pi_processes.append(line)

    lock_lsof = run(["lsof", "-nP", str(LOCK)])
    port_lsof = run(["lsof", "-nP", f"-iTCP:{PORT}"])
    model_lsof = [run(["lsof", "-nP", str(path)]) for path in MODELS]
    memory_pressure = run(["memory_pressure"])
    vm_stat = run(["vm_stat"])
    memory_match = re.search(
        r"System-wide memory free percentage:\s*(\d+)%",
        output(memory_pressure),
    )
    memory_free_percent = int(memory_match.group(1)) if memory_match else None
    memory_floor = 75 if args.profile == "headroom" else 85

    conflicts: list[str] = []
    if git["root"] != str(repo):
        conflicts.append("git root mismatch")
    if git["branch"] != BRANCH:
        conflicts.append("worker branch mismatch")
    if not git["base_is_ancestor"]:
        conflicts.append("expected base is not an ancestor")
    if args.phase == "preflight" and str(git["status_porcelain"]).strip():
        conflicts.append("worktree is not clean before model launch")
    if ds4_processes:
        conflicts.append("a DS4 process is already live")
    if output(lock_lsof).strip():
        conflicts.append("a process holds /private/tmp/ds4.lock")
    if output(port_lsof).strip():
        conflicts.append(f"a process is using TCP port {PORT}")
    if any(output(item).strip() for item in model_lsof):
        conflicts.append("a required model artifact has an open handle")
    if memory_free_percent is None:
        conflicts.append("memory_pressure free percentage was not parseable")
    elif memory_free_percent < memory_floor:
        conflicts.append(
            f"memory free percentage {memory_free_percent}% is below "
            f"the {memory_floor}% {args.profile} floor"
        )

    record = {
        "schema": "ds4.vision-revalidation-preflight.v1",
        "captured_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "profile": args.profile,
        "phase": args.phase,
        "expected_base": BASE,
        "expected_branch": BRANCH,
        "catalog_root": str(CATALOG),
        "planned_resident_gib": 80.95 if args.profile == "headroom" else 95.95,
        "memory_free_floor_percent": memory_floor,
        "git": git,
        "processes": {
            "ds4": ds4_processes,
            "omlx": omlx_processes,
            "pi": pi_processes,
            "pi_count": len(pi_processes),
            "full_ps_command": ps["argv"],
        },
        "connections_and_handles": {
            "lock": lock_lsof,
            "port_18082": port_lsof,
            "models": dict(zip((str(path) for path in MODELS), model_lsof)),
        },
        "memory": {
            "free_percent": memory_free_percent,
            "memory_pressure": memory_pressure,
            "vm_stat": vm_stat,
        },
        "conflicts": conflicts,
        "launch_allowed": not conflicts,
        "safety_statement": (
            "No process was killed, signalled, or modified by this read-only audit. "
            "Unrelated oMLX/Pi processes are recorded and remain untouched."
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(record, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({
        "output": str(args.output.resolve()),
        "profile": args.profile,
        "phase": args.phase,
        "pi_count": len(pi_processes),
        "memory_free_percent": memory_free_percent,
        "conflicts": conflicts,
        "launch_allowed": not conflicts,
    }, indent=2))
    return 0 if not conflicts else 2


if __name__ == "__main__":
    sys.exit(main())
