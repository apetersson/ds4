---
id: DS-003
title: 'Epic: Extract and revalidate existing native-preserving GGUF support'
status: To Do
assignee: []
created_date: '2026-08-25 23:46'
updated_date: '2026-08-26 00:13'
labels:
  - epic
  - 'branch:native-preserved-gguf'
  - gguf
  - mxfp4
dependencies: []
references:
  - feature/native-preserved-gguf
  - feature/vision-embedding-span
priority: high
type: feature
ordinal: 3000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The native-preserving converter/runtime and final 153.39 GiB F16/F32/MXFP4 GGUF already exist with byte-level and SSD-streaming validation. This epic tracks clean extraction and focused revalidation.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Existing native converter/runtime/artifact evidence is recorded as completed work.
- [ ] #2 Only native-preserving GGUF changes are transplanted onto feature/native-preserved-gguf.
- [ ] #3 The exact packaged artifact passes provenance, structural, coherent-generation, and 128 GB SSD-streaming checks on the clean branch.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Preserve artifact evidence, transplant the existing native commit series, revalidate the packaged artifact, then merge the clean branch into integration/dev.
<!-- SECTION:PLAN:END -->
