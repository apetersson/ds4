#!/bin/bash
# Serialize one exact DS-002.03 live replay from safety preflight through shutdown.

set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ] || \
    { [ "$1" != "headroom" ] && [ "$1" != "quality" ]; } || \
    { [ "$#" -eq 2 ] && [ "$2" != "--print-server-command" ]; }; then
  echo "usage: $0 headroom|quality [--print-server-command]" >&2
  exit 64
fi

profile=$1
repo=/Users/andreas/.codex/worktrees/a023/ds4-apetersson
catalog=/Volumes/Samsung_4TB/models/DeepSeek-V4-Flash-0731-Abliterated-Vision
evidence_dir="$repo/tests/evidence"
preflight_tmp="/private/tmp/ds-002.03-${profile}-preflight.json"
server_log_tmp="/private/tmp/ds-002.03-${profile}-server.log"
preflight="$evidence_dir/ds-002.03-${profile}-preflight.json"
results="$evidence_dir/ds-002.03-${profile}-results.json"
postflight="$evidence_dir/ds-002.03-${profile}-postflight.json"
server_log="$evidence_dir/ds-002.03-${profile}-server.log"

case "$profile" in
  headroom)
    receiver="$catalog/Headroom128-IQ2_XXS/DeepSeek-V4-Flash-0731-Abliterated-Vision-Headroom128-IQ2_XXS.gguf"
    ;;
  quality)
    receiver="$catalog/Quality128-IQ2_XXS_XL/DeepSeek-V4-Flash-0731-Abliterated-Vision-Quality128-IQ2_XXS_XL.gguf"
    ;;
esac

server_argv=(
  "$repo/ds4-server"
  --model "$receiver"
  --metal
)
if [ "$profile" = "quality" ]; then
  server_argv+=(--quality)
fi
server_argv+=(
  --ctx 2048
  --tokens 128
  --vision-python /private/tmp/dsv4-vision-venv/bin/python
  --vision-encoder "$catalog/tools/encode_flycockpit.py"
  --vision-tower "$catalog/Vision-BF16/DeepEncoderV2-BF16.safetensors"
  --vision-adapter "$catalog/Vision-BF16/DeepSeek-V4-0731-Projector-BF16.safetensors"
  --host 127.0.0.1
  --port 18082
)

if [ "$#" -eq 2 ]; then
  printf '%q ' "${server_argv[@]}"
  printf '\n'
  exit 0
fi

cd "$repo"
python3 tests/vision_revalidation_preflight.py \
  --profile "$profile" --output "$preflight_tmp"

server_pid=
cleanup() {
  if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
    kill -INT "$server_pid"
    wait "$server_pid" || true
  fi
  server_pid=
}
trap cleanup EXIT INT TERM

"${server_argv[@]}" >"$server_log_tmp" 2>&1 &
server_pid=$!

ready=0
for _ in $(seq 1 600); do
  if ! kill -0 "$server_pid" 2>/dev/null; then
    echo "ds4-server exited before readiness" >&2
    wait "$server_pid"
  fi
  if curl --silent --show-error --fail \
      http://127.0.0.1:18082/v1/models >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 1
done
if [ "$ready" -ne 1 ]; then
  echo "ds4-server did not become ready within 600 seconds" >&2
  exit 1
fi

mkdir -p "$evidence_dir"
cp "$preflight_tmp" "$preflight"
python3 tests/vision_revalidation.py \
  --profile "$profile" \
  --base-url http://127.0.0.1:18082 \
  --preflight "$preflight_tmp" \
  --output "$results"

cleanup
cp "$server_log_tmp" "$server_log"
python3 tests/vision_revalidation_preflight.py \
  --profile "$profile" --phase postflight --output "$postflight"

trap - EXIT INT TERM
echo "ALL LIVE REQUESTS COMPLETE: $profile"
