---
id: DS-001
title: 'Epic: HF catalog resolver with llama.cpp-parity companion UX'
status: To Do
assignee: []
created_date: '2026-08-25 23:46'
updated_date: '2026-08-26 00:24'
labels:
  - epic
  - 'branch:hf-catalog-resolver'
  - hugging-face
dependencies: []
references:
  - feature/hf-catalog-resolver
  - 'https://github.com/ggml-org/llama.cpp/blob/master/common/download.cpp'
priority: high
type: feature
ordinal: 1000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Deliver a secure DS4 HF repository selector with the practical companion UX of current llama.cpp: -hf OWNER/REPO:SELECTOR, automatic same-variant vision for ds4-server, explicit disable/override, opt-in DSpark, and a dual-runtime catalog layout. DS4 uses variants.json for typed roles/integrity; llama.cpp ignores it and discovers ordinary receiver/mmproj/dspark GGUFs from filenames, directory locality, and metadata.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 DS4 supports llama.cpp-familiar HF selector, exact-file, credential, cache, and offline controls without changing local-path behavior.
- [ ] #2 One ds4-server -hf command resolves the receiver and exact DS4 vision bundle; auto-vision can be disabled or fully overridden locally.
- [ ] #3 Manifest v2 describes receiver, DS4 raw vision, llama.cpp BF16 mmproj, and optional DSpark roles with exact integrity and capability data.
- [ ] #4 Published variant directories satisfy current llama.cpp main/mmproj/dspark sibling discovery independently of variants.json.
- [ ] #5 Resolution is selective, immutable, resumable, integrity-checked, concurrency-safe, offline-capable, and data-only.
- [ ] #6 DS4 resolver and external llama.cpp/libmtmd changes remain separate reviewable patch series.
<!-- AC:END -->
