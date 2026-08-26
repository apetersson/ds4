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
Preserved-MXFP4 DSpark quantization and Metal execution already exist in d147270d with unit, Metal, catalog-pair, and A/B evidence. This epic tracks clean extraction and focused reruns.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Existing preserved-MXFP4 implementation, artifact, and A/B evidence are recorded as completed work.
- [ ] #2 The implementation is transplanted cleanly to feature/dspark-preserved-mxfp4 without unrelated features.
- [ ] #3 Focused quantizer/Metal tests and exact receiver/support-pair checks pass on the clean branch.
- [ ] #4 DSpark acceptance and break-even results remain evidence-based, including neutral or negative speedups.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
Preserve baseline evidence, transplant d147270d, rerun focused tests and exact-pair A/B, then merge the clean branch into integration/dev.
<!-- SECTION:PLAN:END -->
