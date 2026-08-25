---
id: DS-002
title: 'Epic: Vision embedding-span serving'
status: To Do
assignee: []
created_date: '2026-08-25 23:46'
labels:
  - epic
  - 'branch:vision'
  - vision
  - multimodal
dependencies: []
priority: high
type: feature
ordinal: 2000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Branch: feature/vision. Isolate and harden the external embedding-span and DeepEncoderV2 image-serving capability as a clean feature line based on integration/dev.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The epic is decomposed into independently verifiable child tasks on feature/vision.
- [ ] #2 OpenAI-compatible image requests reach the receiver through validated external embedding spans.
- [ ] #3 Text-only serving remains behaviorally compatible with the base runtime.
<!-- AC:END -->
