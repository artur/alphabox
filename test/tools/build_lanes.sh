#!/bin/bash
# Build AXPbox build lanes and print one rc line per lane.
# usage: build_lanes.sh [lane ...]
#   With no arguments, every configured build*/ directory in the repo root.
#   Lanes are configured beforehand (see the build-lanes skill).
# Exit status: 0 when every lane built, 1 otherwise.
R=$(cd "$(dirname "$0")/../.." && pwd)
JOBS=${JOBS:-$( (sysctl -n hw.ncpu || nproc) 2>/dev/null || echo 4)}
cd "$R" || exit 1
if [ $# -gt 0 ]; then
  lanes=("$@")
else
  lanes=()
  for c in build*/CMakeCache.txt; do [ -f "$c" ] && lanes+=("$(dirname "$c")"); done
fi
failed=0
for l in "${lanes[@]}"; do
  if [ ! -f "$l/CMakeCache.txt" ]; then
    printf '  %-26s not configured\n' "$l"
    failed=1
    continue
  fi
  if cmake --build "$l" -j"$JOBS" > "$l/build_lanes.log" 2>&1; then
    printf '  %-26s rc=0\n' "$l"
  else
    printf '  %-26s FAILED (see %s/build_lanes.log)\n' "$l" "$l"
    grep -a 'error:' "$l/build_lanes.log" | head -5 | sed 's/^/    /'
    failed=1
  fi
done
exit $failed
