---
id: DS-001
title: 'Epic: Hugging Face catalog resolver'
status: To Do
assignee: []
created_date: '2026-08-25 23:46'
labels:
  - epic
  - 'branch:hf-catalog-resolver'
  - hugging-face
dependencies: []
priority: high
type: feature
ordinal: 1000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Branch: feature/hf-catalog-resolver. Deliver upstream-friendly runtime resolution of a selected Hugging Face catalog variant into verified local DS4 model paths, without introducing vision-specific dependencies into the generic resolver.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The epic is decomposed into independently reviewable child tasks on feature/hf-catalog-resolver.
- [ ] #2 The completed branch supports the documented --hf repository-and-selector command for a target GGUF.
- [ ] #3 The branch remains reviewable independently from custom vision and native-preserving GGUF work.
<!-- AC:END -->
