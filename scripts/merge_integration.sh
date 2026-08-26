#!/usr/bin/env bash
# Replay the validated DS-005.02 feature merges in their deliberate order.

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
merge_subjects=(
  "merge(vision): integrate validated embedding span head"
  "merge(gguf): integrate validated native-preserving head"
  "merge(dspark): integrate validated preserved MXFP4 head"
)

usage() {
  cat <<'EOF'
usage: scripts/merge_integration.sh [--check|--apply]

  --check  Verify the immutable base, feature refs, and recorded order (default).
  --apply  Merge the exact heads with --no-ff on a clean non-governed branch.

Before promotion, --apply finishes by running the topology verifier in
--pre-promotion mode. After the reviewed worker is promoted, use
`make integration-gates`; its topology target uses --post-promotion mode.

If a semantic conflict occurs, resolve only the reported files, stage them, and
rerun --apply. The script verifies MERGE_HEAD, commits the pending merge, and
continues in the same recorded order.
EOF
}

die() {
  echo "merge_integration: $*" >&2
  exit 1
}

mode=${1:---check}
if [[ $# -gt 1 ]] || [[ $mode != --check && $mode != --apply ]]; then
  usage >&2
  exit 64
fi

repo=$(git rev-parse --show-toplevel 2>/dev/null) || die "run inside the target Git worktree"
cd "$repo"

git cat-file -e "$base^{commit}" 2>/dev/null || die "missing base commit $base"
for i in "${!feature_heads[@]}"; do
  ref=refs/heads/${feature_refs[$i]}
  actual=$(git rev-parse --verify "$ref^{commit}" 2>/dev/null) || die "missing local ref $ref"
  [[ $actual == "${feature_heads[$i]}" ]] ||
    die "$ref moved: expected ${feature_heads[$i]}, found $actual"
  common=$(git merge-base "$base" "${feature_heads[$i]}")
  [[ $common == c9e133bd1e989adc357daa8bd675f0b536c32076 ]] ||
    die "unexpected merge base for ${feature_refs[$i]}: $common"
done

echo "base $base"
for i in "${!feature_heads[@]}"; do
  printf '%d %s %s\n' "$((i + 1))" "${feature_refs[$i]}" "${feature_heads[$i]}"
done

[[ $mode == --apply ]] || exit 0

branch=$(git symbolic-ref --quiet --short HEAD 2>/dev/null) ||
  die "--apply requires a named disposable/integration worker branch"
case "$branch" in
  main|integration/dev|feature/vision|feature/native-preserved-gguf|feature/dspark-preserved-mxfp4)
    die "refusing to mutate governed branch $branch"
    ;;
esac

pending=$(git rev-parse --quiet --verify MERGE_HEAD 2>/dev/null || true)
if [[ -z $pending ]]; then
  [[ -z $(git status --porcelain) ]] || die "--apply requires a clean worktree"
  git merge-base --is-ancestor "$base" HEAD || die "base is not an ancestor of HEAD"
fi

for i in "${!feature_heads[@]}"; do
  head=${feature_heads[$i]}
  subject=${merge_subjects[$i]}

  if git merge-base --is-ancestor "$head" HEAD; then
    continue
  fi

  pending=$(git rev-parse --quiet --verify MERGE_HEAD 2>/dev/null || true)
  if [[ -n $pending ]]; then
    [[ $pending == "$head" ]] ||
      die "pending MERGE_HEAD $pending does not match next recorded head $head"
    [[ -z $(git diff --name-only --diff-filter=U) ]] || {
      echo "merge_integration: resolve and stage the semantic conflicts, then rerun --apply" >&2
      git diff --name-only --diff-filter=U >&2
      exit 1
    }
    git commit -m "$subject"
    continue
  fi

  if ! git merge --no-ff --no-commit "$head"; then
    echo "merge_integration: semantic resolution required for ${feature_refs[$i]}" >&2
    git diff --name-only --diff-filter=U >&2
    echo "merge_integration: stage the resolved union and rerun --apply" >&2
    exit 1
  fi
  git commit -m "$subject"
done

echo "merge_integration: all validated heads are present"
echo "merge_integration: verifying worker topology before promotion"
"$repo/scripts/check_branch_topology.sh" --pre-promotion HEAD
