---
id: DS-006
title: 'Epic: llama.cpp libmtmd DeepEncoderV2 vision support'
status: To Do
assignee: []
created_date: '2026-08-26 00:21'
labels:
  - epic
  - llama.cpp
  - libmtmd
  - vision
  - external-repo
dependencies: []
references:
  - >-
    https://github.com/ggml-org/llama.cpp/blob/master/docs/development/HOWTO-add-model.md
  - 'https://github.com/ggml-org/llama.cpp/blob/master/tools/mtmd/README.md'
  - >-
    /Users/andreas/.codex/attachments/600735e3-7f66-458b-9a99-1ca6f289de6b/pasted-text.txt
priority: high
type: feature
ordinal: 11000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Track the upstream-facing llama.cpp work required for this HF catalog to behave as a conventional two-GGUF VLM: an ordinary deepseek4 receiver plus a same-directory lossless BF16 mmproj containing DeepEncoderV2 and the trained projector. Implementation belongs in an apetersson llama.cpp fork/feature branch, not in the DS4 resolver branch; it is tracked here because public catalog compatibility and release claims depend on it.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A normal upstream-compatible deepseek4 main GGUF and one BF16 mmproj per selectable directory run through llama-mtmd-cli and llama-server without a model-specific endpoint or CLI.
- [ ] #2 The mmproj converter, DeepEncoderV2 preprocessor/graphs, 896-to-4096 projector, view separator, and DeepSeek4 route-token semantics are implemented and reviewable as separate commits.
- [ ] #3 HF selection works through current llama.cpp filename/directory/metadata rules without reading variants.json.
- [ ] #4 Lossless claims are backed by BF16 tensor identity plus stagewise pixel/activation/projected-row and final-logit comparisons.
- [ ] #5 257, 769, 1025, and 1281-row layouts, text-only invariance, OpenAI image_url, and real/zero/shuffled causal controls pass for Headroom and Quality.
- [ ] #6 The work is pinned to an audited upstream revision, kept rebaseable, and not proposed upstream until it satisfies libmtmd contribution and human-review policy.
<!-- AC:END -->
