#!/usr/bin/env bash
# Verify DS-005.02 merge ancestry, governed refs, order, and path inventory.

set -euo pipefail

base=52ed7f4123e1267f95b4ce2d2e89a97154f9e523
feature_refs=(
  feature/vision
  feature/native-preserved-gguf
  feature/dspark-preserved-mxfp4
)
feature_heads=(
  9f664c7ea4f6589ec0dc6480acc6ef4b5e2cc636
  2d25fad1bd9b9c86218986daf51514953b56c854
  0f15545eef685a4718d2cd1a322cb7c1e55a9f9c
)
allowed_integration_paths=(
  "backlog/tasks/ds-005.02 - Establish-repeatable-integration-merge-and-conflict-gates.md"
  Makefile
  scripts/check_branch_topology.sh
  scripts/merge_integration.sh
)

die() {
  echo "check_branch_topology: $*" >&2
  exit 1
}

revision=${1:-HEAD}
repo=$(git rev-parse --show-toplevel 2>/dev/null) || die "run inside the target Git worktree"
cd "$repo"
revision=$(git rev-parse --verify "$revision^{commit}") || die "invalid integration revision"

integration_ref=$(git rev-parse --verify refs/heads/integration/dev 2>/dev/null) ||
  die "missing refs/heads/integration/dev"
[[ $integration_ref == "$base" ]] ||
  die "integration/dev moved: expected $base, found $integration_ref"

main=$(git rev-parse --verify refs/heads/main 2>/dev/null) || die "missing local main"
upstream_main=$(git rev-parse --verify refs/remotes/upstream/main 2>/dev/null) ||
  die "missing upstream/main tracking ref"
[[ $main == "$upstream_main" ]] ||
  die "main $main differs from upstream/main $upstream_main"

git merge-base --is-ancestor "$base" "$revision" ||
  die "base $base is not an ancestor of $revision"

for i in "${!feature_heads[@]}"; do
  ref=refs/heads/${feature_refs[$i]}
  actual=$(git rev-parse --verify "$ref^{commit}" 2>/dev/null) || die "missing $ref"
  [[ $actual == "${feature_heads[$i]}" ]] ||
    die "$ref moved: expected ${feature_heads[$i]}, found $actual"
  git merge-base --is-ancestor "${feature_heads[$i]}" "$revision" ||
    die "${feature_refs[$i]} head is not an ancestor of $revision"
done

merges=()
while IFS= read -r merge; do
  merges+=("$merge")
done < <(git rev-list --first-parent --merges --reverse "$base..$revision")
[[ ${#merges[@]} -eq ${#feature_heads[@]} ]] ||
  die "expected ${#feature_heads[@]} first-parent merges, found ${#merges[@]}"

expected_first_parent=$base
for i in "${!merges[@]}"; do
  read -r -a parents <<<"$(git show -s --format=%P "${merges[$i]}")"
  [[ ${#parents[@]} -eq 2 ]] || die "${merges[$i]} is not a true two-parent merge"
  [[ ${parents[0]} == "$expected_first_parent" ]] ||
    die "${merges[$i]} breaks the recorded first-parent chain"
  [[ ${parents[1]} == "${feature_heads[$i]}" ]] ||
    die "merge $((i + 1)) has second parent ${parents[1]}, expected ${feature_heads[$i]}"
  expected_first_parent=${merges[$i]}
done

expected=$(mktemp "${TMPDIR:-/tmp}/ds005-expected.XXXXXX")
actual=$(mktemp "${TMPDIR:-/tmp}/ds005-actual.XXXXXX")
missing=$(mktemp "${TMPDIR:-/tmp}/ds005-missing.XXXXXX")
extra=$(mktemp "${TMPDIR:-/tmp}/ds005-extra.XXXXXX")
allowed=$(mktemp "${TMPDIR:-/tmp}/ds005-allowed.XXXXXX")
trap 'rm -f "$expected" "$actual" "$missing" "$extra" "$allowed"' EXIT

for head in "${feature_heads[@]}"; do
  git diff --name-only "$base...$head"
done | LC_ALL=C sort -u >"$expected"
git diff --name-only "$base..$revision" | LC_ALL=C sort -u >"$actual"
printf '%s\n' "${allowed_integration_paths[@]}" | LC_ALL=C sort -u >"$allowed"

comm -23 "$expected" "$actual" >"$missing"
comm -13 "$expected" "$actual" | comm -23 - "$allowed" >"$extra"
if [[ -s $missing || -s $extra ]]; then
  [[ ! -s $missing ]] || { echo "missing feature paths:" >&2; sed 's/^/  /' "$missing" >&2; }
  [[ ! -s $extra ]] || { echo "unrelated paths:" >&2; sed 's/^/  /' "$extra" >&2; }
  die "integrated path inventory differs from the feature union plus approved gate files"
fi

echo "check_branch_topology: PASS"
echo "  integration revision: $revision"
echo "  base: $base"
for i in "${!merges[@]}"; do
  echo "  merge $((i + 1)): ${merges[$i]} <- ${feature_heads[$i]}"
done
echo "  main == upstream/main: $main"
echo "  feature inventory paths: $(wc -l <"$expected" | tr -d ' ')"
