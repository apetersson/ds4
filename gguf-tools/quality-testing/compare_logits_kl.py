#!/usr/bin/env python3
"""Compare two full-vocabulary logit vectors with distribution metrics."""

from __future__ import annotations

import argparse
import array
import json
import math
import sys
from pathlib import Path
from typing import Any


def load_logits(path: Path, file_format: str) -> tuple[list[float], dict[str, Any]]:
    if file_format == "auto":
        file_format = "json" if path.suffix.lower() == ".json" else "f32"

    if file_format == "json":
        payload = json.loads(path.read_text())
        raw_logits = payload.get("logits")
        if not isinstance(raw_logits, list):
            raise ValueError(f"{path}: JSON object has no logits array")
        logits = [float("-inf") if value is None else float(value) for value in raw_logits]
        metadata = {key: value for key, value in payload.items() if key != "logits"}
        return logits, metadata

    raw = path.read_bytes()
    if len(raw) % 4:
        raise ValueError(f"{path}: raw float32 byte count is not divisible by four")
    values = array.array("f")
    values.frombytes(raw)
    if sys.byteorder != "little":
        values.byteswap()
    return [float(value) for value in values], {}


def log_softmax(logits: list[float], temperature: float) -> list[float]:
    scaled = [value / temperature for value in logits]
    if any(math.isnan(value) or value == math.inf for value in scaled):
        raise ValueError("logits contain NaN or positive infinity")
    maximum = max(scaled)
    if not math.isfinite(maximum):
        raise ValueError("logits contain no finite values")
    normalizer = maximum + math.log(sum(math.exp(value - maximum) for value in scaled))
    return [value - normalizer for value in scaled]


def probability(log_probability: float) -> float:
    return math.exp(log_probability) if math.isfinite(log_probability) else 0.0


def analyze(
    reference: list[float],
    candidate: list[float],
    temperature: float,
) -> dict[str, Any]:
    if len(reference) != len(candidate):
        raise ValueError(
            f"vocabulary mismatch: reference={len(reference)}, candidate={len(candidate)}"
        )
    if not reference:
        raise ValueError("empty logit vectors")

    log_p = log_softmax(reference, temperature)
    log_q = log_softmax(candidate, temperature)
    p = [probability(value) for value in log_p]
    q = [probability(value) for value in log_q]

    kl_pq = 0.0
    kl_qp = 0.0
    js = 0.0
    total_variation = 0.0
    entropy_p = 0.0
    entropy_q = 0.0
    for pi, qi, lpi, lqi in zip(p, q, log_p, log_q):
        mixture = 0.5 * (pi + qi)
        log_mixture = math.log(mixture) if mixture else float("-inf")
        if pi:
            kl_pq += pi * (lpi - lqi)
            js += 0.5 * pi * (lpi - log_mixture)
            entropy_p -= pi * lpi
        if qi:
            kl_qp += qi * (lqi - lpi)
            js += 0.5 * qi * (lqi - log_mixture)
            entropy_q -= qi * lqi
        total_variation += abs(pi - qi)

    finite_deltas = [
        candidate_value - reference_value
        for reference_value, candidate_value in zip(reference, candidate)
        if math.isfinite(reference_value) and math.isfinite(candidate_value)
    ]
    delta_mean = sum(finite_deltas) / len(finite_deltas)
    centered_logit_rmse = math.sqrt(
        sum((value - delta_mean) ** 2 for value in finite_deltas) / len(finite_deltas)
    )

    reference_top = sorted(range(len(reference)), key=reference.__getitem__, reverse=True)[:50]
    candidate_top = sorted(range(len(candidate)), key=candidate.__getitem__, reverse=True)[:50]
    reference_argmax = reference_top[0]
    candidate_argmax = candidate_top[0]

    return {
        "vocab": len(reference),
        "temperature": temperature,
        "kl_reference_to_candidate_nats": kl_pq,
        "kl_reference_to_candidate_bits": kl_pq / math.log(2.0),
        "kl_candidate_to_reference_nats": kl_qp,
        "kl_candidate_to_reference_bits": kl_qp / math.log(2.0),
        "jensen_shannon_nats": js,
        "jensen_shannon_bits": js / math.log(2.0),
        "total_variation": 0.5 * total_variation,
        "reference_entropy_nats": entropy_p,
        "candidate_entropy_nats": entropy_q,
        "reference_argmax_id": reference_argmax,
        "reference_argmax_probability": p[reference_argmax],
        "candidate_argmax_id": candidate_argmax,
        "candidate_argmax_probability": q[candidate_argmax],
        "top10_overlap": len(set(reference_top[:10]) & set(candidate_top[:10])),
        "top50_overlap": len(set(reference_top) & set(candidate_top)),
        "centered_logit_rmse": centered_logit_rmse,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, help="native/reference logits")
    parser.add_argument("candidate", type=Path, help="quantized/candidate logits")
    parser.add_argument("--format", choices=("auto", "json", "f32"), default="auto")
    parser.add_argument("--temperature", type=float, default=1.0)
    parser.add_argument("--label", default="")
    parser.add_argument("--out", type=Path, help="optional JSON output path")
    args = parser.parse_args()

    if not math.isfinite(args.temperature) or args.temperature <= 0.0:
        parser.error("--temperature must be finite and greater than zero")

    reference, reference_metadata = load_logits(args.reference, args.format)
    candidate, candidate_metadata = load_logits(args.candidate, args.format)
    result = {
        "label": args.label,
        "reference": str(args.reference),
        "candidate": str(args.candidate),
        "reference_metadata": reference_metadata,
        "candidate_metadata": candidate_metadata,
        "metrics": analyze(reference, candidate, args.temperature),
    }
    rendered = json.dumps(result, indent=2) + "\n"
    if args.out:
        args.out.write_text(rendered)
        print(f"wrote {args.out}")
    else:
        print(rendered, end="")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
