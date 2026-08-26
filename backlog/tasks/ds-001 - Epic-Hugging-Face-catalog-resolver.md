---
id: DS-001
title: 'Epic: HF catalog resolver with llama.cpp-parity companion UX'
status: To Do
assignee: []
created_date: '2026-08-25 23:46'
updated_date: '2026-08-26 00:19'
labels:
  - epic
  - 'branch:hf-catalog-resolver'
  - hugging-face
dependencies: []
references:
  - 'https://github.com/ggml-org/llama.cpp/blob/master/common/download.cpp'
  - 'https://github.com/ggml-org/llama.cpp/blob/master/docs/multimodal.md'
priority: high
type: feature
ordinal: 1000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Deliver an upstream-friendly HF repository selector for ds4 and ds4-server with the practical UX users expect from current llama.cpp: -hf OWNER/REPO:SELECTOR, immutable/cacheable acquisition, automatic same-variant vision discovery for multimodal servers, explicit disable/override controls, and opt-in DSpark discovery. The DS4 manifest remains a typed integrity contract; the published repository must also be usable by llama.cpp from filenames, directory locality, and embedded GGUF metadata because llama.cpp does not read variants.json.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 ds4 and ds4-server support llama.cpp-compatible -hf/-hfr/--hf-repo syntax plus exact-file, token, cache, and offline controls without breaking local paths.
- [ ] #2 One ds4-server -hf command automatically acquires and wires the selected receiver plus exact DS4 vision bundle; users can explicitly disable auto-vision or override the complete local vision bundle.
- [ ] #3 The versioned manifest declares exact receiver, DS4 vision, llama.cpp mmproj, and optional DSpark roles with sizes, hashes, capabilities, and runtime constraints.
- [ ] #4 Each selectable variant is independently usable by llama.cpp's existing sibling-discovery rules from ordinary deepseek4 main GGUF plus same-directory lowercase mmproj-/dspark- companions, without variants.json.
- [ ] #5 Downloads are selective, revision-pinned, resumable, integrity-checked, concurrency-safe, offline-capable, and never execute repository code.
- [ ] #6 The HF resolver branch remains separable from DS4 vision implementation and from the external llama.cpp/libmtmd DeepEncoderV2 patch series.
<!-- AC:END -->
