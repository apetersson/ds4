---
id: DS-002
title: 'Epic: Extract and revalidate existing vision embedding-span serving'
status: In Progress
assignee: []
created_date: '2026-08-25 23:46'
updated_date: '2026-08-26 00:13'
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
The vision implementation already exists on feature/vision-embedding-span and has been exercised against the packaged catalog. This epic tracks preserving that baseline, transplanting only vision-specific commits onto feature/vision, and resolving the remaining alias/semantic-drift risk with focused revalidation.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Existing vision implementation and validation evidence are recorded as a completed baseline.
- [ ] #2 Only vision-specific changes are transplanted to feature/vision; native-GGUF, DSpark-quantizer, and HF-resolver work remain isolated.
- [ ] #3 The clean branch passes focused text, image-span, 257/769-row, causal-control, and direct DS4-server/Pi checks.
- [ ] #4 Observed alias-dependent OCR/semantic drift is reproduced or resolved and documented honestly.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Preserve baseline evidence, transplant existing vision commits, revalidate exact catalog artifacts, then merge the clean branch into integration/dev.
<!-- SECTION:PLAN:END -->
