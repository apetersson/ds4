---
id: DS-005
title: 'Epic: Integrate clean feature branches and qualify HF release'
status: In Progress
assignee: []
created_date: '2026-08-25 23:46'
updated_date: '2026-08-26 00:13'
labels:
  - epic
  - 'branch:integration-dev'
  - integration
  - release
dependencies: []
references:
  - integration/dev
priority: high
type: feature
ordinal: 5000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The local multimodal catalog, GGUF A/B evaluation, direct DS4-server/Pi path, native artifact, DSpark variants, documentation, and both MLX variants are already built and validated. Integration now focuses on merging clean feature branches, adding the genuinely new HF resolver, staging the catalog, rerunning the high-risk 100k/128 GB gates, and publishing evidence-backed documentation.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Existing catalog, GGUF, MLX, direct-Pi, and branch-governance work is recorded as a completed baseline.
- [ ] #2 Clean vision, native-GGUF, DSpark, and HF resolver branches merge reproducibly into integration/dev while main remains upstream-clean.
- [ ] #3 A private HF staging repo proves selector/companion discovery and the direct ds4-server plus vanilla Pi workflow.
- [ ] #4 Final merged builds pass focused regression, image causality, DSpark parity, and 100k-context/128 GB memory gates.
- [ ] #5 Release docs preserve exact identity, artifact sizes/hashes, compatibility limits, and measured—not inferred—claims.
<!-- AC:END -->

## Implementation Plan

<!-- SECTION:PLAN:BEGIN -->
1. Preserve the existing release baseline. 2. Merge the clean feature branches. 3. Integrate and privately stage the new HF resolver/catalog flow. 4. Run final high-risk qualification. 5. Finalize docs and a validated release tag.
<!-- SECTION:PLAN:END -->
