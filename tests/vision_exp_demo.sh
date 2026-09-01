#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf 'usage: %s reference|resident MODEL VISION_DIR OUTPUT_DIR\n' "$0" >&2
    printf 'env: DS4_SERVER, VISION_PYTHON, VISION_ENCODER, PORT, EXPERT_CACHE, TEXT_MAX_TOKENS, IMAGE_MAX_TOKENS\n' >&2
    exit 2
}

[[ $# -eq 4 ]] || usage
mode=$1
model=$2
vision_dir=$3
output_dir=$4

case "$mode" in
    reference|resident) ;;
    *) usage ;;
esac

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
server=${DS4_SERVER:-$repo_dir/ds4-server}
vision_python=${VISION_PYTHON:-python3}
vision_encoder=${VISION_ENCODER:-$repo_dir/misc/encode-deepseek4-vision.py}
vision_tower=$vision_dir/DeepSeek-V4-Flash-Vision-Exp-Abliterated-Native.safetensors
vision_config=$vision_dir/config.json
port=${PORT:-18080}
expert_cache=${EXPERT_CACHE:-48GB}
text_max_tokens=${TEXT_MAX_TOKENS:-4}
image_max_tokens=${IMAGE_MAX_TOKENS:-64}

[[ $text_max_tokens =~ ^[1-9][0-9]*$ ]] || { printf 'invalid TEXT_MAX_TOKENS: %s\n' "$text_max_tokens" >&2; exit 2; }
[[ $image_max_tokens =~ ^[1-9][0-9]*$ ]] || { printf 'invalid IMAGE_MAX_TOKENS: %s\n' "$image_max_tokens" >&2; exit 2; }

for path in "$server" "$model" "$vision_encoder" "$vision_tower" "$vision_config"; do
    [[ -e "$path" ]] || { printf 'missing required path: %s\n' "$path" >&2; exit 2; }
done
if [[ $mode == reference && $model != /Users/andreas/fast_models/* ]]; then
    printf 'reference mode requires the receiver under /Users/andreas/fast_models\n' >&2
    exit 2
fi
command -v magick >/dev/null || { printf 'ImageMagick magick is required\n' >&2; exit 2; }
command -v jq >/dev/null || { printf 'jq is required\n' >&2; exit 2; }

now() { perl -MTime::HiRes=time -e 'printf "%.6f\n", time'; }
process_alive() {
    local state
    kill -0 "$1" 2>/dev/null || return 1
    state=$(ps -o stat= -p "$1" 2>/dev/null | tr -d '[:space:]')
    [[ -n $state && $state != Z* ]]
}

if curl -fsS --max-time 1 "http://127.0.0.1:$port/v1/models" >/dev/null 2>&1; then
    printf 'port %s already has an OpenAI-compatible server; refusing to capture another process\n' "$port" >&2
    exit 2
fi

mkdir -p "$output_dir"
red_png=$output_dir/pure-red-100x100.png
text_request=$output_dir/text-request.json
image_request=$output_dir/image-request.json
server_log=$output_dir/server.log
trace_log=$output_dir/server.trace
peak_file=$output_dir/peak-process-tree-rss-kib.txt

magick -size 100x100 xc:'#ff0000' -alpha off PNG24:"$red_png"
pixel_stats=$(magick identify -format '%w %h %[fx:minima.r] %[fx:maxima.r] %[fx:minima.g] %[fx:maxima.g] %[fx:minima.b] %[fx:maxima.b]' "$red_png")
[[ $pixel_stats == '100 100 1 1 0 0 0 0' ]] || {
    printf 'red fixture verification failed: %s\n' "$pixel_stats" >&2
    exit 1
}
printf '%s\n' "$pixel_stats" > "$output_dir/pure-red-100x100.verify.txt"
shasum -a 256 "$red_png" > "$output_dir/pure-red-100x100.sha256"

jq -n --argjson max_tokens "$text_max_tokens" '{
  model: "deepseek-chat",
  messages: [{role: "user", content: "just answer OK"}],
  max_tokens: $max_tokens,
  temperature: 0,
  stream: false
}' > "$text_request"

image_b64=$(base64 < "$red_png" | tr -d '\n')
jq -n --arg url "data:image/png;base64,$image_b64" --argjson max_tokens "$image_max_tokens" '{
  model: "deepseek-chat",
  messages: [{
    role: "user",
    content: [
      {type: "image_url", image_url: {url: $url}},
      {type: "text", text: "what color is that image"}
    ]
  }],
  max_tokens: $max_tokens,
  temperature: 0,
  stream: false
}' > "$image_request"

server_args=(
    "$server"
    --model "$model"
    --metal
    --ctx 2048
    --tokens 32
    --host 127.0.0.1
    --port "$port"
    --trace "$trace_log"
    --vision-python "$vision_python"
    --vision-encoder "$vision_encoder"
    --vision-tower "$vision_tower"
    --vision-adapter "$vision_config"
)
if [[ $mode == reference ]]; then
    server_args+=(--ssd-streaming --ssd-streaming-cache-experts "$expert_cache")
fi

{
    for arg in "${server_args[@]}"; do printf '%q ' "$arg"; done
    printf '\n'
} > "$output_dir/launch-command.txt"

started=$(now)
"${server_args[@]}" > "$server_log" 2>&1 &
server_pid=$!
monitor_pid=

cleanup() {
    if process_alive "$server_pid"; then
        kill -TERM "$server_pid" 2>/dev/null || true
    fi
    wait "$server_pid" 2>/dev/null || true
    if [[ -n ${monitor_pid:-} ]]; then wait "$monitor_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM

(
    peak=0
    while process_alive "$server_pid"; do
        rss=$(ps -axo pid=,ppid=,rss= | awk -v root="$server_pid" '$1 == root || $2 == root { total += $3 } END { print total + 0 }')
        if (( rss > peak )); then peak=$rss; fi
        printf '%s\n' "$peak" > "$peak_file"
        sleep 0.25
    done
) &
monitor_pid=$!

deadline=$((SECONDS + 3600))
until grep -Fq "ds4-server: listening on http://127.0.0.1:$port" "$server_log" &&
      curl -fsS --max-time 2 "http://127.0.0.1:$port/v1/models" > "$output_dir/models.json" 2>/dev/null; do
    if ! process_alive "$server_pid"; then
        wait "$server_pid" 2>/dev/null || true
        printf 'server exited before becoming ready; see %s\n' "$server_log" >&2
        exit 1
    fi
    (( SECONDS < deadline )) || { printf 'server startup timed out\n' >&2; exit 1; }
    sleep 1
done
ready=$(now)
awk -v start="$started" -v end="$ready" 'BEGIN { printf "%.6f\n", end-start }' > "$output_dir/load-seconds.txt"

curl -sS --fail-with-body --max-time 1800 \
    -H 'Content-Type: application/json' \
    --data-binary "@$text_request" \
    -o "$output_dir/text-response.json" \
    -w '%{http_code} %{time_total}\n' \
    "http://127.0.0.1:$port/v1/chat/completions" > "$output_dir/text-http-metrics.txt"

curl -sS --fail-with-body --max-time 1800 \
    -H 'Content-Type: application/json' \
    --data-binary "@$image_request" \
    -o "$output_dir/image-response.json" \
    -w '%{http_code} %{time_total}\n' \
    "http://127.0.0.1:$port/v1/chat/completions" > "$output_dir/image-http-metrics.txt"

jq -e . "$output_dir/text-response.json" >/dev/null
jq -e . "$output_dir/image-response.json" >/dev/null
text_answer=$(jq -r '.choices[0].message.content // ""' "$output_dir/text-response.json")
image_answer=$(jq -r '.choices[0].message.content // ""' "$output_dir/image-response.json")
printf '%s\n' "$text_answer" > "$output_dir/text-answer.txt"
printf '%s\n' "$image_answer" > "$output_dir/image-answer.txt"

if process_alive "$server_pid"; then kill -TERM "$server_pid"; fi
wait "$server_pid" || true
wait "$monitor_pid" || true
monitor_pid=
trap - EXIT INT TERM

peak_kib=$(tr -d '[:space:]' < "$peak_file")
load_seconds=$(tr -d '[:space:]' < "$output_dir/load-seconds.txt")
text_http=$(tr -d '\n' < "$output_dir/text-http-metrics.txt")
image_http=$(tr -d '\n' < "$output_dir/image-http-metrics.txt")
jq -n \
    --arg mode "$mode" \
    --arg model "$model" \
    --arg load_seconds "$load_seconds" \
    --arg peak_rss_kib "$peak_kib" \
    --arg text_http "$text_http" \
    --arg image_http "$image_http" \
    --arg text_answer "$text_answer" \
    --arg image_answer "$image_answer" \
    '{mode:$mode, model:$model, load_seconds:($load_seconds|tonumber),
      peak_process_tree_rss_kib:($peak_rss_kib|tonumber),
      text_http_metrics:$text_http, image_http_metrics:$image_http,
      text_answer:$text_answer, image_answer:$image_answer,
      text_semantic_pass:($text_answer == "OK"),
      image_semantic_pass:($image_answer | test("(^|[^A-Za-z])red([^A-Za-z]|$)"; "i"))}' \
    > "$output_dir/summary.json"

printf 'captured demo in %s\n' "$output_dir"
