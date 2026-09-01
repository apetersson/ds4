# Vision-Exp Headroom128 KL Divergence

This report compares the stable no-iMatrix Vision-Exp quant against the
native-preserved GGUF using full-vocabulary next-token distributions. KL is a
property of a distribution at a specific prompt position, not a universal
scalar for an entire model, so both measured checkpoints are stated exactly.

## Models and runtime

- Candidate: `DeepSeek-V4-Flash-Vision-Exp-Abliterated-Headroom128-IQ2_XXS-NoImatrix-AttentionF16.gguf`
  - SHA-256: `b6a6fbf2bacd0187a9ef613b40050c0139f5821059d2bb59874c11d0acddab7f`
  - 101.43 GiB, 43 layers, 1,371 tensors
  - No iMatrix; routed tensors retain IQ2_XXS/Q2_K with ten native MXFP4 layers
  - All 215 core attention projections use native F16
- Reference: `DeepSeek-V4-Flash-Vision-Exp-Abliterated-NativePreserved-F16-F32-MXFP4.gguf`
  - SHA-256: `4b46c3b767c09ed707f2e89122e8a0ea9a92e0f714ffd5a5d3656c91f1cc69b4`
- Runtime: Metal SSD streaming, 48 GiB expert-cache target, patched local DS4
  commit `92f68ba2`, vocabulary size 129,280.

## Definition

For native logits `z_p`, quantized logits `z_q`, and temperature `T = 1`:

```text
P = softmax(z_p / T)
Q = softmax(z_q / T)
KL(P || Q) = sum_i P_i log(P_i / Q_i)
```

The reverse KL is also reported because KL is asymmetric. Jensen-Shannon (JS)
divergence uses `M = (P + Q) / 2`; total variation (TV) is
`0.5 * sum_i |P_i - Q_i|`. Natural-log results are in nats; bits are nats
divided by `ln(2)`.

## Results

| Checkpoint | KL native→quant | KL quant→native | JS | TV | Argmax |
|---|---:|---:|---:|---:|---|
| Text, after 12-token CLI-rendered `just answer OK` prompt | 0.0000290243 nats / 0.0000418732 bits | 0.000248932 nats | 0.0000100512 nats | 0.0000290891 | Same: token 11932, `OK` |
| Vision, after 125-token pure-red prompt plus the common first newline token | 0.289686 nats / 0.417928 bits | 0.302504 nats | 0.0687368 nats | 0.303796 | Same: token 9854 |

Additional diagnostics:

| Checkpoint | Native entropy | Quant entropy | Native argmax probability | Quant argmax probability | Top-10 overlap | Top-50 overlap |
|---|---:|---:|---:|---:|---:|---:|
| Text | 0.000000245 nats | 0.000425880 nats | 0.999999988 | 0.999970899 | 7/10 | 31/50 |
| Vision | 4.648883 nats | 4.965574 nats | 0.314304 | 0.213940 | 4/10 | 35/50 |

## Interpretation

The controlled text distribution is effectively unchanged at the decision that
produces `OK`. The image-conditioned distribution shows a measurable shift:
the quant is more diffuse and assigns less probability to the shared argmax.
It nevertheless preserves the greedy token at this checkpoint and passes the
full semantic test twice, describing the controlled image as bright, solid red
in agreement with both native DS4 and the Python reference.

The vision KL therefore documents residual distribution drift rather than a
semantic failure. It also explains why a generated-text-only check is not
sufficient: the text checkpoint is nearly deterministic, while the vision
checkpoint exposes substantially more uncertainty.

## Reproduction

The raw evidence is under:

```text
/Users/andreas/fast_models/vision-exp-demo/kl-divergence-20260901
```

Recompute each row with:

```sh
python3 gguf-tools/quality-testing/compare_logits_kl.py \
  /Users/andreas/fast_models/vision-exp-demo/kl-divergence-20260901/native/text-logits.json \
  /Users/andreas/fast_models/vision-exp-demo/kl-divergence-20260901/quant/text-logits.json \
  --label text

python3 gguf-tools/quality-testing/compare_logits_kl.py \
  /Users/andreas/fast_models/vision-exp-demo/kl-divergence-20260901/native/vision_result_output-43_pos0.bin \
  /Users/andreas/fast_models/vision-exp-demo/kl-divergence-20260901/quant/vision_result_output-43_pos0.bin \
  --format f32 --label vision
```

The vision files are graph-session logits after both models generated the same
first newline token from the same 125-token image prompt. This common prefix is
required for a valid next-token comparison.

## Limitations

- These are two controlled checkpoints, not a corpus-wide KL average.
- The text prompt is deliberately easy and nearly deterministic.
- The vision checkpoint occurs before the answer reaches the color word.
- KL changes with temperature and prompt position; compare runs only when the
  tokenizer, rendered prompt, vocabulary, runtime flags, and prefix are equal.
- A release-grade extension should average teacher-forced KL over a curated
  text-and-image prompt suite and report bootstrap confidence intervals.
