---
id: DS-003
title: 'Epic: Native-preserving mixed FP8/packed-FP4 GGUF'
status: To Do
assignee: []
created_date: '2026-08-25 23:46'
labels:
  - epic
  - 'branch:native-preserved-gguf'
  - gguf
  - mxfp4
dependencies: []
priority: high
type: feature
ordinal: 3000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Branch: feature/native-preserved-gguf. Isolate conversion, metadata, loader, Metal execution, and verification needed for the exact-value native-preserving F16/F32/MXFP4 reference GGUF.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The epic is decomposed into conversion, runtime, verification, and documentation child tasks on feature/native-preserved-gguf.
- [ ] #2 The native-preserving GGUF loads and executes with the intended F16, F32, I32, and MXFP4 tensor types.
- [ ] #3 The branch can be reviewed without depending on Hugging Face catalog resolution.
<!-- AC:END -->
