---
id: DS-004
title: 'Epic: DSpark preserved-MXFP4 compatibility'
status: To Do
assignee: []
created_date: '2026-08-25 23:46'
labels:
  - epic
  - 'branch:dspark-preserved-mxfp4'
  - dspark
  - mxfp4
dependencies: []
priority: high
type: feature
ordinal: 4000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Branch: feature/dspark-preserved-mxfp4. Isolate the quantizer planning, runtime compatibility, and regression coverage required for DSpark support models that preserve native MXFP4 tensors.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The epic is decomposed into independently testable child tasks on feature/dspark-preserved-mxfp4.
- [ ] #2 Preserved-MXFP4 DSpark plans are accepted only when tensor layout and runtime support are valid.
- [ ] #3 Existing DSpark and non-DSpark model paths retain regression coverage.
<!-- AC:END -->
