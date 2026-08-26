#!/usr/bin/env bash
# Verify DS-005.02 merge ancestry, governed refs, order, and path inventory.

set -euo pipefail

base=52ed7f4123e1267f95b4ce2d2e89a97154f9e523
promoted_head=9402d363300ecfb430a5feb1706bbfcd6f378765
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
approved_gate_blob_pairs=(
  "eba268897e08dab725e63e439573ac0b6f3b7f6f 413663d91e5cde12d4cd6b32216191ad7b0a8a00"
  "29212d83bd37492e125d9b036fb785c0d135be1a 0a42cc7ab92a9b47fa1ac89e1a5911040d57fb3d"
)
allowed_integration_paths=(
  "backlog/tasks/ds-005.02 - Establish-repeatable-integration-merge-and-conflict-gates.md"
  "backlog/tasks/ds-005.02.01 - Pin-semantic-merge-identity-in-topology-gates.md"
  "backlog/tasks/ds-005.02.02 - Accept-semantically-identical-merge-replays.md"
  "backlog/tasks/ds-005.02.03 - Pin-approved-post-merge-gate-blobs.md"
  "backlog/tasks/ds-005.02.04 - Make-integration-topology-gates-valid-after-promotion.md"
  Makefile
  scripts/check_branch_topology.sh
  scripts/merge_integration.sh
  tests/test_branch_topology.sh
)

usage() {
  cat <<'EOF'
usage: scripts/check_branch_topology.sh MODE [REVISION]

  --pre-promotion   Validate a worker revision while integration/dev remains
                    pinned to the immutable pre-merge base.
  --post-promotion  Validate integration/dev and REVISION on the lineage rooted
                    at the exact reviewed promoted head.
EOF
}

die() {
  echo "check_branch_topology: $*" >&2
  exit 1
}

mode=${1:-}
revision=${2:-HEAD}
if [[ $# -gt 2 ]] || [[ $mode != --pre-promotion && $mode != --post-promotion ]]; then
  usage >&2
  exit 64
fi

repo=$(git rev-parse --show-toplevel 2>/dev/null) || die "run inside the target Git worktree"
cd "$repo"
revision=$(git rev-parse --verify "$revision^{commit}") || die "invalid integration revision"

integration_ref=$(git rev-parse --verify refs/heads/integration/dev 2>/dev/null) ||
  die "missing refs/heads/integration/dev"
case "$mode" in
  --pre-promotion)
    [[ $integration_ref == "$base" ]] ||
      die "pre-promotion integration/dev moved: expected $base, found $integration_ref"
    ;;
  --post-promotion)
    git merge-base --is-ancestor "$promoted_head" "$integration_ref" ||
      die "post-promotion integration/dev $integration_ref is not on promoted lineage $promoted_head"
    git merge-base --is-ancestor "$integration_ref" "$revision" ||
      die "post-promotion integration/dev $integration_ref is not an ancestor of revision $revision"
    git merge-base --is-ancestor "$promoted_head" "$revision" ||
      die "revision $revision is not on promoted lineage $promoted_head"
    ;;
esac

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
  merge_helper_blob=$(git rev-parse --verify "$revision:scripts/merge_integration.sh" 2>/dev/null) ||
    die "revision has no merge replay helper"
  gate_blob_pair="$makefile_blob $merge_helper_blob"
  approved=false
  for approved_pair in "${approved_gate_blob_pairs[@]}"; do
    if [[ $gate_blob_pair == "$approved_pair" ]]; then
      approved=true
      break
    fi
  done
  [[ $approved == true ]] ||
    die "post-merge gate blobs are not an approved pair: Makefile $makefile_blob, merge helper $merge_helper_blob"
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
echo "  mode: ${mode#--}"
echo "  integration revision: $revision"
echo "  integration/dev: $integration_ref"
echo "  base: $base"
[[ $mode != --post-promotion ]] || echo "  promoted lineage root: $promoted_head"
for i in "${!merges[@]}"; do
  echo "  merge $((i + 1)): ${merges[$i]} <- ${feature_heads[$i]}"
  echo "    tree: ${merge_trees[$i]}"
done
echo "  main == upstream/main: $main"
echo "  feature inventory paths: $(wc -l <"$expected" | tr -d ' ')"
