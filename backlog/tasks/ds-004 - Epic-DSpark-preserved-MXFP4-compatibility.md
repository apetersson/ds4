---
id: DS-004
title: 'Epic: Extract and revalidate existing DSpark preserved-MXFP4 support'
status: In Progress
assignee: []
created_date: '2026-08-25 23:46'
updated_date: '2026-08-26 00:13'
labels:
  - epic
  - 'branch:dspark-preserved-mxfp4'
  - dspark
  - mxfp4
dependencies: []
references:
  - feature/dspark-preserved-mxfp4
  - d147270d
priority: high
type: feature
ordinal: 4000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Preserved-MXFP4 DSpark quantization and Metal execution support already exists in commit d147270d and has unit, Metal, catalog-pair, and A/B evidence. This epic now tracks clean extraction, focused reruns, and documentation of measured DSpark value rather than reimplementation.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The existing preserved-MXFP4 implementation, artifact, and A/B evidence are recorded as completed work.
- [ ] #2 The implementation is transplanted cleanly to feature/dspark-preserved-mxfp4 without vision, native-reference, or HF-resolver changes.
- [ ] #3 Focused quantizer and Metal tests plus exact catalog receiver/support-model checks pass on the clean branch.
- [ ] #4 DSpark acceptance and break-even results are reported honestly, including neutral or negative speedups.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Freeze the proven baseline. 2. Transplant d147270d cleanly. 3. Rerun focused unit/Metal and exact-artifact A/B checks. 4. Hand the clean branch to integration/dev.
<!-- SECTION:PLAN:END -->
