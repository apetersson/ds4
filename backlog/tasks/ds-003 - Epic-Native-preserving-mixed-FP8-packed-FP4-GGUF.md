---
id: DS-003
title: 'Epic: Extract and revalidate the existing native-preserving GGUF path'
status: To Do
assignee: []
created_date: '2026-08-25 23:46'
updated_date: '2026-08-26 00:09'
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
The native-preserving converter/runtime and the final 153.39 GiB F16/F32/MXFP4 GGUF already exist and have byte-level plus SSD-streaming validation. This epic tracks recording that baseline, transplanting its commits onto a clean feature/native-preserved-gguf branch, and rerunning focused qualification there.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The existing native converter/runtime/artifact evidence is recorded as completed work.
- [ ] #2 Only native-preserving GGUF changes are transplanted onto the clean feature branch.
- [ ] #3 The exact packaged artifact passes structural, byte-provenance, coherent-generation, and 128 GB SSD-streaming checks on the clean branch.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Freeze artifact and commit evidence. 2. Replay the native-preserving commit series cleanly. 3. Revalidate the existing packaged artifact. 4. Hand the clean branch to integration/dev.
<!-- SECTION:PLAN:END -->
