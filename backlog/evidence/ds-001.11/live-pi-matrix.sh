#!/bin/zsh
set -eu
set -o pipefail

evidence_dir=${0:A:h}
image=/Volumes/Samsung_4TB/models/DeepSeek-V4-Flash-0731-Abliterated-Vision/eval/fixtures/orion_dashboard.png
rubric=$evidence_dir/live-rubric.txt
output=/private/tmp/ds00111-review-fix-live
agent_dir=/private/tmp/ds00111-review-fix-pi-agent

test -f "$image"
test -f "$rubric"
test -f "$evidence_dir/pi-models.json"
mkdir -p "$output" "$agent_dir"
cp "$evidence_dir/pi-models.json" "$agent_dir/models.json"

prompt=$(<"$rubric")
pi_version=$(pi --version)
print -r -- "pi_version=$pi_version" > "$output/pi-matrix.txt"
print -r -- "pi_binary=$(command -v pi)" >> "$output/pi-matrix.txt"
print -r -- "image=$image" >> "$output/pi-matrix.txt"
shasum -a 256 "$image" >> "$output/pi-matrix.txt"

for provider in ds4-encoder ds4-real ds4-zero ds4-shuffle; do
  print -r -- "CASE=$provider" | tee -a "$output/pi-matrix.txt"
  PI_CODING_AGENT_DIR="$agent_dir" PI_OFFLINE=1 PI_TELEMETRY=0 \
    pi --provider "$provider" --model deepseek-chat --api-key local-evidence-only \
      --thinking off --no-tools --no-extensions --no-skills \
      --no-prompt-templates --no-themes --no-context-files --no-session \
      --offline --print "@$image" "$prompt" \
      2>&1 | tee "$output/pi-$provider.txt" | tee -a "$output/pi-matrix.txt"
  print -r -- '---' >> "$output/pi-matrix.txt"
done
