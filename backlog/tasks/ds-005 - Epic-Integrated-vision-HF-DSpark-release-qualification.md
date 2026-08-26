---
id: DS-005
title: 'Epic: Integrate DS4 and llama.cpp vision paths and qualify HF release'
status: In Progress
assignee: []
created_date: '2026-08-25 23:46'
updated_date: '2026-08-26 00:23'
labels:
  - epic
  - 'branch:integration-dev'
  - integration
  - release
dependencies: []
references:
  - integration/dev
  - 'https://github.com/ggml-org/llama.cpp/blob/master/common/download.cpp'
priority: high
type: feature
ordinal: 5000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The local DS4/MLX catalog baseline exists. Remaining integration now includes the DS4 HF resolver, clean DS4 feature branches, and the separate llama.cpp/libmtmd DeepEncoderV2 patch required for a conventional two-GGUF VLM experience from the same HF selector directories.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Existing catalog, GGUF, MLX, direct-Pi, and branch-governance work remains recorded as completed baseline.
- [ ] #2 Clean DS4 vision/native/DSpark/HF branches merge reproducibly while main remains upstream-clean.
- [ ] #3 The external llama.cpp/libmtmd work passes stagewise and end-to-end DeepEncoderV2 parity before the catalog claims llama.cpp vision support.
- [ ] #4 Private HF staging proves both selectors through DS4 receiver+raw-vision discovery and llama.cpp receiver+mmproj sibling discovery, with matching optional DSpark.
- [ ] #5 Final 100k/128 GB, causality, semantic, parity, integrity, and local-vs-HF gates pass on exact release artifacts.
- [ ] #6 Release docs distinguish patched versus upstream runtime compatibility and measured versus inferred claims.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Preserve the existing release baseline. 2. Merge the clean feature branches. 3. Integrate and privately stage the new HF resolver/catalog flow. 4. Run final high-risk qualification. 5. Finalize docs and a validated release tag.
<!-- SECTION:PLAN:END -->
