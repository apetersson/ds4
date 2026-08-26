#!/usr/bin/env bash
# Deterministic positive and negative fixtures for DS-005.02 topology modes.

set -euo pipefail

base=52ed7f4123e1267f95b4ce2d2e89a97154f9e523
promoted_head=9402d363300ecfb430a5feb1706bbfcd6f378765
task_tracking_head=bf4b93f2d896ebc944e4e2bcf05117c3e6e66d2b
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

die() {
  echo "test_branch_topology: $*" >&2
  exit 1
}

source_repo=$(git rev-parse --show-toplevel 2>/dev/null) || die "run inside the target Git worktree"
fixture_root=$(mktemp -d "${TMPDIR:-/tmp}/ds005-topology.XXXXXX")
fixture_repo=$fixture_root/repo
trap 'rm -rf "$fixture_root"' EXIT

git clone --quiet --no-local "$source_repo" "$fixture_repo"
cp "$source_repo/scripts/check_branch_topology.sh" "$fixture_repo/scripts/check_branch_topology.sh"
chmod +x "$fixture_repo/scripts/check_branch_topology.sh"

source_main=$(git -C "$source_repo" rev-parse refs/heads/main)
git -C "$fixture_repo" update-ref refs/heads/main "$source_main"
git -C "$fixture_repo" update-ref refs/remotes/upstream/main "$source_main"
for i in "${!feature_heads[@]}"; do
  case $i in
    0) feature_ref=feature/vision ;;
    1) feature_ref=feature/native-preserved-gguf ;;
    2) feature_ref=feature/dspark-preserved-mxfp4 ;;
  esac
  git -C "$fixture_repo" update-ref "refs/heads/$feature_ref" "${feature_heads[$i]}"
done

invoke_checker() {
  (cd "$fixture_repo" && ./scripts/check_branch_topology.sh "$@")
}

expect_pass() {
  label=$1
  shift
  if ! "$@" >"$fixture_root/pass.out" 2>&1; then
    sed 's/^/  /' "$fixture_root/pass.out" >&2
    die "expected PASS: $label"
  fi
}

expect_reject() {
  label=$1
  shift
  if "$@" >"$fixture_root/reject.out" 2>&1; then
    die "expected rejection: $label"
  fi
}

set_integration_ref() {
  git -C "$fixture_repo" update-ref refs/heads/integration/dev "$1"
}

reject_both_modes() {
  label=$1
  revision=$2
  set_integration_ref "$base"
  expect_reject "$label (pre-promotion)" invoke_checker --pre-promotion "$revision"
  set_integration_ref "$promoted_head"
  expect_reject "$label (post-promotion)" invoke_checker --post-promotion "$revision"
}

set_integration_ref "$base"
expect_pass "reviewed worker before promotion" invoke_checker --pre-promotion "$promoted_head"
set_integration_ref "$promoted_head"
expect_pass "historical promoted head" invoke_checker --post-promotion "$promoted_head"
set_integration_ref "$task_tracking_head"
expect_pass "current post-promotion task lineage" invoke_checker --post-promotion HEAD

git -C "$fixture_repo" update-ref refs/heads/feature/vision "$base"
reject_both_modes "moved feature ref" "$promoted_head"
git -C "$fixture_repo" update-ref refs/heads/feature/vision "${feature_heads[0]}"

make_commit() {
  tree=$1
  shift
  args=()
  for parent in "$@"; do
    args+=(-p "$parent")
  done
  printf 'fixture\n' | git -C "$fixture_repo" commit-tree "$tree" "${args[@]}"
}

wrong_order_1=$(make_commit "${merge_trees[0]}" "$base" "${feature_heads[1]}")
wrong_order_2=$(make_commit "${merge_trees[1]}" "$wrong_order_1" "${feature_heads[0]}")
wrong_order_3=$(make_commit "${merge_trees[2]}" "$wrong_order_2" "${feature_heads[2]}")
reject_both_modes "wrong merge parent/order" "$wrong_order_3"

base_tree=$(git -C "$fixture_repo" show -s --format=%T "$base")
wrong_tree_1=$(make_commit "$base_tree" "$base" "${feature_heads[0]}")
wrong_tree_2=$(make_commit "${merge_trees[1]}" "$wrong_tree_1" "${feature_heads[1]}")
wrong_tree_3=$(make_commit "${merge_trees[2]}" "$wrong_tree_2" "${feature_heads[2]}")
reject_both_modes "wrong merge tree" "$wrong_tree_3"

commit_with_blob() {
  parent=$1
  path=$2
  content=$3
  mode=$4
  git -C "$fixture_repo" read-tree "$parent^{tree}"
  blob=$(printf '%s\n' "$content" | git -C "$fixture_repo" hash-object -w --stdin)
  git -C "$fixture_repo" update-index --add --cacheinfo "$mode,$blob,$path"
  tree=$(git -C "$fixture_repo" write-tree)
  make_commit "$tree" "$parent"
}

unrelated=$(commit_with_blob "$promoted_head" unrelated-inventory.txt unrelated 100644)
reject_both_modes "unrelated inventory path" "$unrelated"

for gate_path in Makefile scripts/merge_integration.sh; do
  gate_mode=100644
  [[ $gate_path != scripts/merge_integration.sh ]] || gate_mode=100755
  unapproved=$(commit_with_blob "$promoted_head" "$gate_path" unapproved "$gate_mode")
  reject_both_modes "unapproved post-merge gate blob: $gate_path" "$unapproved"
done

echo "test_branch_topology: PASS"
echo "  positive modes: pre-promotion worker, historical/current post-promotion lineage"
echo "  negative modes: moved refs, merge parents/order/tree, inventory, gate blobs"
