---
id: DS-002
title: 'Epic: Extract and revalidate existing vision embedding-span serving'
status: To Do
assignee: []
created_date: '2026-08-25 23:46'
updated_date: '2026-08-26 00:08'
labels:
  - epic
  - 'branch:vision'
  - vision
  - multimodal
dependencies: []
references:
  - feature/vision
  - feature/vision-embedding-span
priority: high
type: feature
ordinal: 2000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The vision implementation already exists on feature/vision-embedding-span and has been exercised against the packaged catalog. This epic now tracks preserving that completed baseline, transplanting only the vision-specific commits onto the clean feature/vision branch, and resolving the remaining model-alias/semantic-drift risk with focused revalidation. It does not re-plan the implementation from scratch.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Existing vision implementation and validation evidence are recorded as a completed baseline.
- [ ] #2 Only vision-specific changes are transplanted to feature/vision; native-GGUF, DSpark-quantizer, and HF-resolver work remain isolated.
- [ ] #3 The clean branch builds and passes focused text, image-span, 257-row, 769-row, real/zero/shuffled-image, and direct DS4-server/Pi checks.
- [ ] #4 Observed alias-dependent OCR/semantic drift is reproduced or resolved and documented honestly.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Freeze the proven composite-branch baseline. 2. Transplant the existing vision commits onto the clean branch. 3. Revalidate exact catalog artifacts and investigate semantic drift. 4. Hand the clean branch to integration/dev.
<!-- SECTION:PLAN:END -->
