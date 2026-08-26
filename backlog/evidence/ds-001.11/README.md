# DS-001.11 serialized HF vision evidence

Captured on 2026-08-26 from clean branch `worker/DS-001.11-hf-vision` at
`d70d1c8f344cf292709c88b7336a6df0bf5c9d17`, whose merge base is the exact
completed DS-001.06 head `1f6f19a1a9d8861246f4096c322908eb7341fb48`.

## Launch identity

- Repository: `apetersson/DeepSeek-V4-Flash-0731-Abliterated-Vision`
- Immutable revision: `0123456789abcdef0123456789abcdef01234567`
- Selector: `Headroom128-IQ2_XXS`
- Cache: APFS `/Volumes/DS00111HF`
- Receiver: 86,720,111,776 bytes, SHA-256 `162e2b5e245ca0927282111064c8dfbd58894cabd51958322161814eb9addbb6`
- Tower: 906,533,408 bytes, SHA-256 `9dcf6803d4c6b63acc4008bc2409e599a2ab6e3886e241f1727f61550c300df5`
- Projector: 40,919,752 bytes, SHA-256 `77f8be7a44a93aeec05f7294d51d72bed2dc4328770ba214186bcc671480db77`
- Config: 658 bytes, SHA-256 `2c1295c110b1b7ac2b238c451f34a1112aa4296052d8119e703fd58a4c193fbb`
- Trusted Python: `/private/tmp/dsv4-vision-venv/bin/python`
- Trusted encoder: `/Volumes/Samsung_4TB/models/DeepSeek-V4-Flash-0731-Abliterated-Vision/tools/encode_flycockpit.py`

The command specified only `-hf`, cache, trusted local Python/encoder, Metal,
context/token limits, and bind address. It did not specify a local receiver,
tower, projector, config, mmproj, or DSpark artifact. The deterministic local
Hub served only the repository identity and `variants.json`; every artifact URL
returned 404. Its request audit contains only two API/manifest pairs (one
pre-allocation interrupted attempt and the final supervised attempt), proving
the final run used the already verified cache.

The final run used an atomic global lane supervisor. PID 34567 created and
exclusively held global lock inode 173367886 before spawning server PID 34600.
The child used a task-scoped singleton lock because an independent second open
cannot reacquire the supervisor's macOS flock. The supervisor removed only its
exact inode after graceful server drain.

## Results

- `models.json`: verified receiver plus tower/projector/config roles;
  `vision_bundle_verified`, `vision_active`, and `multimodal` are true;
  `dspark_active` is false.
- `text.json`: exact text-only baseline `DS4 VISION TEXT OK`.
- `image-url-real.json`: real OpenAI `image_url` returned Orion System Status,
  42.7 toks, 38 ms, 114.9 GiB, Sensor -> Encoder -> Router -> Decoder, and
  NOMINAL.
- `server.log`: production encoder reports the exact image SHA-256, BF16
  weights, adapter step 4800, 3 views, 769 tokens, and inherited `/dev/fd/5`
  tower plus `/dev/fd/6` projector. This is the suffix-preserving verified
  descriptor path introduced by DS-001.11.02.
- `span-real.json`: the committed real 769-row span reproduces every target
  fact.
- `span-zero.json`: the identical route prompt with zeroed embeddings invents
  2,847 requests/s, 412 ms, 61%, and API/Auth/RateLimiter/LoadBalancer.
- `span-shuffle.json`: shuffled rows instead produce Memory Status, 4.2 GB/s,
  Origin/Snapshot/Encoder/Decoder, and Requested. These materially different
  controls demonstrate causal use of the real visual rows.
- Vanilla Pi ran offline with `deepseek-chat`, thinking off, and no tools,
  extensions, skills, context files, prompt templates, or session. The first
  title sample inserted a stray space and is retained in `pi-title-first.txt`.
  A full factual sample recovered the exact title, three values, and four flow
  nodes but hallucinated status `Active`; it is retained in `pi-full.txt`.
  The focused title retry returned the visible title exactly as
  `Orión System Status` in `pi-title-pass.txt`.

The matrix therefore passes the intended vanilla-Pi real-image title gate and
the direct API's complete real/zero/shuffle causal gate without hiding the two
non-gating quantized samples.

## Safety evidence

`preflight.txt` records clean identity, absent global locks, free port 18082,
read-only exact cache paths/sizes, no exact artifact/support handles, and the
unchanged unrelated oMLX/Pi processes immediately before atomic acquisition.
`postflight.txt` records all global/task lock paths absent, ports 18081/18082
free, zero lane processes, no exact handles, 89% memory free, and unchanged
unrelated oMLX/Pi immediately after release. The temporary task-only flock
semantics file was verified unheld and removed; its audit is preserved.

