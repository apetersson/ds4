#!/bin/sh
set -eu

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT INT TERM
log="$tmp_dir/output.log"
failures=0

ok() { printf 'ok - %s\n' "$1"; }
fail() { printf 'not ok - %s\n' "$1" >&2; failures=$((failures + 1)); }

run_fail() {
    run_case_name=$1
    run_pattern=$2
    shift 2
    if "$@" >"$log" 2>&1; then
        fail "$run_case_name unexpectedly succeeded"
    elif grep -q -- "$run_pattern" "$log"; then
        ok "$run_case_name"
    else
        fail "$run_case_name returned the wrong diagnostic"
        sed -n '1,12p' "$log" >&2
    fi
}

for bin in ./ds4 ./ds4-server; do
    name=${bin#./}
    "$bin" --help >"$log" 2>&1
    for option in --hf-repo --hf-file --hf-token --hf-revision --hf-cache-dir --hf-offline; do
        if grep -q -- "$option" "$log"; then ok "$name help lists $option"
        else fail "$name help omits $option"; fi
    done
    "$bin" --help runtime >"$log" 2>&1
    if grep -q -- "Explicit local MTP/DSpark" "$log"; then
        ok "$name help documents explicit MTP precedence"
    else
        fail "$name help omits explicit MTP precedence"
    fi

    for alias in -hf -hfr --hf-repo --hf; do
        run_fail "$name accepts $alias llama.cpp-style selector" \
            "HF network failure" \
            env HF_ENDPOINT=http://127.0.0.1:1 \
            "$bin" "$alias" ggml-org/GLM-4.7-Flash-GGUF:Q4_K_M
    done
    run_fail "$name accepts exact HF file override" \
        "HF network failure" \
        env HF_ENDPOINT=http://127.0.0.1:1 \
        "$bin" -hf owner/repo:Q4_K_M -hff nested/model-Q8_0.gguf
    run_fail "$name rejects local/HF receiver conflict before allocation" \
        "mutually exclusive" \
        "$bin" -m /definitely/not/a/model.gguf -hf owner/repo
    run_fail "$name rejects malformed selector" \
        "invalid HF selector" \
        "$bin" -hf owner/repo:bad/selector
    run_fail "$name rejects duplicate HF aliases" \
        "duplicate option" \
        "$bin" -hf owner/repo --hf other/repo
    run_fail "$name rejects missing HF value" \
        "missing value for --hf" \
        "$bin" --hf
    run_fail "$name does not consume the next flag as an HF value" \
        "missing value for --hf" \
        "$bin" --hf --offline
    run_fail "$name accepts offline alias" \
        "offline manifest reuse" \
        "$bin" --hf owner/repo --hf-offline

    if HF_TOKEN=environment-secret HF_ENDPOINT=http://127.0.0.1:1 \
        "$bin" --hf owner/repo --hf-token cli-super-secret >"$log" 2>&1; then
        fail "$name token precedence probe unexpectedly succeeded"
    elif grep -q "cli-super-secret\|environment-secret" "$log"; then
        fail "$name leaked an HF token"
    elif grep -q "HF network failure" "$log"; then
        ok "$name accepts token/endpoint configuration without leaking"
    else
        fail "$name token precedence probe returned the wrong diagnostic"
    fi
done

./ds4-server --help >"$log" 2>&1
for option in --no-vision --vision-python --vision-encoder --vision-tower --vision-adapter; do
    if grep -q -- "$option" "$log"; then ok "server help lists $option"
    else fail "server help omits $option"; fi
done

run_fail "server --no-vision disables catalog planning" \
    "HF network failure" \
    env HF_ENDPOINT=http://127.0.0.1:1 \
    ./ds4-server --hf owner/repo:Headroom128 --no-vision
run_fail "server accepts complete explicit vision override" \
    "HF network failure" \
    env HF_ENDPOINT=http://127.0.0.1:1 ./ds4-server --hf owner/repo \
        --vision-python /usr/bin/python3 \
        --vision-encoder /opt/local/encoder.py \
        --vision-tower /models/tower.safetensors \
        --vision-adapter /models/adapter.safetensors
run_fail "server accepts trusted local programs for automatic catalog vision" \
    "HF network failure" \
    env HF_ENDPOINT=http://127.0.0.1:1 ./ds4-server --hf owner/repo \
        --vision-python /usr/bin/python3 \
        --vision-encoder /opt/local/encoder.py
run_fail "server rejects partial explicit vision override" \
    "explicit vision override requires" \
    ./ds4-server --hf owner/repo --vision-tower /models/tower.safetensors
run_fail "server rejects explicit vision with --no-vision" \
    "cannot be combined" \
    ./ds4-server --hf owner/repo --no-vision \
        --vision-python /usr/bin/python3 \
        --vision-encoder /opt/local/encoder.py \
        --vision-tower /models/tower.safetensors \
        --vision-adapter /models/adapter.safetensors
run_fail "server accepts explicit DSpark catalog opt-in" \
    "catalog DSpark activation is not wired yet" \
    ./ds4-server --hf owner/repo --dspark
run_fail "server rejects DSpark without a support source" \
    "requires either explicit --mtp" \
    ./ds4-server --dspark

if [ "$failures" -ne 0 ]; then
    printf '%d HF CLI integration test(s) failed\n' "$failures" >&2
    exit 1
fi
printf 'all HF CLI integration tests passed\n'
