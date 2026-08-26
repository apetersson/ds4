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
merge_trees=(
  7b4e233e8f4a9c159558d013ec03f21f3da48ca0
  da38af0df1f50653664cf356e84dbf94b5d76154
  87b8a9180ab075b13445305d8278fd8c5c17dfe3
)
approved_makefile_blob=eba268897e08dab725e63e439573ac0b6f3b7f6f
approved_merge_helper_blob=413663d91e5cde12d4cd6b32216191ad7b0a8a00
allowed_integration_paths=(
  "backlog/tasks/ds-005.02 - Establish-repeatable-integration-merge-and-conflict-gates.md"
  "backlog/tasks/ds-005.02.01 - Pin-semantic-merge-identity-in-topology-gates.md"
  "backlog/tasks/ds-005.02.02 - Accept-semantically-identical-merge-replays.md"
  "backlog/tasks/ds-005.02.03 - Pin-approved-post-merge-gate-blobs.md"
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
  tree=$(git show -s --format=%T "${merges[$i]}")
  [[ $tree == "${merge_trees[$i]}" ]] ||
    die "merge $((i + 1)) has tree $tree, expected validated tree ${merge_trees[$i]}"
  expected_first_parent=${merges[$i]}
done

final_feature_merge=${merges[2]}
if [[ $revision != "$final_feature_merge" ]]; then
  makefile_blob=$(git rev-parse --verify "$revision:Makefile" 2>/dev/null) ||
    die "revision has no Makefile"
  [[ $makefile_blob == "$approved_makefile_blob" ]] ||
    die "post-merge Makefile blob is $makefile_blob, expected $approved_makefile_blob"
  merge_helper_blob=$(git rev-parse --verify "$revision:scripts/merge_integration.sh" 2>/dev/null) ||
    die "revision has no merge replay helper"
  [[ $merge_helper_blob == "$approved_merge_helper_blob" ]] ||
    die "post-merge replay helper blob is $merge_helper_blob, expected $approved_merge_helper_blob"
fi

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

git diff --name-only "$final_feature_merge..$revision" | LC_ALL=C sort -u >"$actual"
comm -23 "$actual" "$allowed" >"$extra"
if [[ -s $extra ]]; then
  echo "paths changed after the validated feature merge but outside DS-005.02:" >&2
  sed 's/^/  /' "$extra" >&2
  die "post-merge delta contains non-gate changes"
fi

echo "check_branch_topology: PASS"
echo "  integration revision: $revision"
echo "  base: $base"
for i in "${!merges[@]}"; do
  echo "  merge $((i + 1)): ${merges[$i]} <- ${feature_heads[$i]}"
  echo "    tree: ${merge_trees[$i]}"
done
echo "  main == upstream/main: $main"
echo "  feature inventory paths: $(wc -l <"$expected" | tr -d ' ')"
