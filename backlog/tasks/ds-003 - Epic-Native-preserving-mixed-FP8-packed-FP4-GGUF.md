---
id: DS-003
title: 'Epic: Extract and revalidate the existing native-preserving GGUF path'
status: Done
assignee: []
created_date: '2026-08-25 23:46'
updated_date: '2026-08-26 01:17'
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
- [x] #1 The existing native converter/runtime/artifact evidence is recorded as completed work.
- [x] #2 Only native-preserving GGUF changes are transplanted onto the clean feature branch.
- [x] #3 The exact packaged artifact passes structural, byte-provenance, coherent-generation, and 128 GB SSD-streaming checks on the clean branch.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Freeze artifact and commit evidence. 2. Replay the native-preserving commit series cleanly. 3. Revalidate the existing packaged artifact. 4. Hand the clean branch to integration/dev.
<!-- SECTION:PLAN:END -->

## Comments

<!-- COMMENTS:BEGIN -->
author: Codex
created: 2026-08-26 01:17
---
Parent-scope closure: fresh read-only review at child evidence commit e6834f7f covered DS-003.01, DS-003.02, DS-003.02.01, and DS-003.03 and returned exactly ALL GOOD. All three parent ACs are now objectively supported and checked.
---
<!-- COMMENTS:END -->

## Final Summary

<!-- SECTION:FINAL_SUMMARY:BEGIN -->
Completed the native-preserving extraction epic. DS-003.01 recorded the existing converter/runtime/artifact baseline, DS-003.02 transplanted the proven native-preserving commit series onto the clean feature branch with focused build/exactness validation, and DS-003.03 revalidated the exact packaged 153.386 GiB artifact on that clean branch. The actual size/SHA, manifests, 1328-tensor inventory, frozen-source byte contract, structural inspection, and 48 GB-cache SSD-streaming smoke on the 128 GB M1 Ultra all passed. Resident execution remains correctly assigned to hardware where the full artifact plus runtime memory fits. Fresh read-only review of the full parent DS-003 scope returned exactly ALL GOOD with no findings.
<!-- SECTION:FINAL_SUMMARY:END -->
