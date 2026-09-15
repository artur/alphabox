#!/bin/bash
# Build each given commit in a separate worktree, in every lane configured
# there, so every commit of a series is known to build.
# usage: WT=<worktree dir> build_revs.sh <rev> [<rev> ...]
#   Create the worktree once (git worktree add <dir> <base>) and configure
#   its lanes (see the build-lanes skill). The worktree must have no local
#   changes: each rev is checked out detached.
# Exit status: 0 when every rev built in every lane, 1 otherwise.
: "${WT:?set WT to the worktree directory}"
JOBS=${JOBS:-$( (sysctl -n hw.ncpu || nproc) 2>/dev/null || echo 4)}
[ $# -gt 0 ] || { echo "usage: WT=<dir> $0 <rev>..."; exit 2; }
failed=0
for rev in "$@"; do
  if ! git -C "$WT" checkout -q --detach "$rev"; then
    echo "== $rev: checkout failed (local changes in $WT?)"
    failed=1
    continue
  fi
  echo "== $(git -C "$WT" log --oneline -1)"
  for c in "$WT"/build*/CMakeCache.txt; do
    [ -f "$c" ] || continue
    d=$(dirname "$c")
    if cmake --build "$d" -j"$JOBS" > "$d/build_revs.log" 2>&1; then
      printf '  %-26s rc=0\n' "$(basename "$d")"
    else
      printf '  %-26s FAILED\n' "$(basename "$d")"
      grep -a 'error:' "$d/build_revs.log" | head -5 | sed 's/^/    /'
      failed=1
    fi
  done
done
exit $failed
