# HF resolver tests

Run the complete deterministic suite from the repository root:

```sh
make -j4 test-hf
```

The suite starts loopback-only fake Hugging Face endpoints and uses tiny
metadata, GGUF, safetensors, and JSON fixtures. It removes Hugging Face token
environment variables from child processes and needs no credentials or
production model downloads.

Coverage is split by failure boundary:

- `test_hf_args.c` and `test_hf_cli.sh`: aliases, precedence, local/HF
  conflicts, exact-file selection, offline mode, text-only vision behavior,
  complete local vision overrides, and DSpark opt-in.
- `test_hf_manifest.c`: bounded schema-v2 parsing, receiver/vision/mmproj/
  DSpark roles, selector matching, llama.cpp primary and lowercase sibling
  rules, nested directories, and cross-directory decoys.
- `test_hf_transport.py`: immutable branch/tag resolution, endpoint routing,
  authentication failures, token precedence, timeouts, and safe diagnostics.
- `test_hf_cache.py` and `test_hf_integrity.py`: selective downloads,
  redirect/range resume, concurrent processes, interrupted publication,
  offline reuse, SHA-256/size verification, cache identity, symlink and
  special-file rejection, and verified-file-descriptor handoff.
- `test_hf_runtime.py`: byte-identical local/HF receiver execution, server
  auto-vision, override/disable behavior, exact missing roles, semantic vision
  compatibility, and verified startup handoff.
- `test_hf_dspark.py`: exact same-profile opt-in, decoys, cross-profile
  rejection, explicit local precedence, and server/CLI wiring.
- `test_hf_diagnostics.py`: stable human and JSON listing/dry-run output,
  cache/offline state, endpoint isolation, and non-overlapping DS4 versus
  llama.cpp runtime totals.
- `ds4_test --server`: the server's internal request and vision contracts.

Real private-Hub qualification is a release gate, not part of ordinary CI. It
must use a pinned repository revision and record separate Headroom and Quality
results for this DS4 build and the exact patched llama.cpp/libmtmd build. Never
turn a missing private credential or production model into an ordinary CI
failure or weaken the deterministic suite to accommodate staging.
