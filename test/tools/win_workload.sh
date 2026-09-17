#!/bin/bash
# Real-workload benchmark: boot an installed Windows 2000 guest headless on a
# throwaway clone, run a CPU-bound command in it through the Run dialog, and
# time the command from its console window opening to closing.
#
# usage: win_workload.sh <label> <alphabox-binary> <install-dir> <cfg> "<command>" [VAR=value ...]
#   install-dir  relative paths are under $ALPHABOX_WORK (default <repo>/lab);
#                the install is cloned to $ALPHABOX_WORK/work-<label>
#   command      typed into Start > Run, e.g.
#                "cmd /c for /l %i in (1,1,300000) do @rem"
#   VAR=value    extra environment for the emulator
# Tunables (environment): SETTLE=<s> after the desktop appears (default 60),
#   BOOT_TIMEOUT (400), RUN_TIMEOUT (900), REPEATS=<n> runs of the command in
#   one boot (default 1; compare medians -- this removes boot-to-boot
#   variance, the dominant noise), BETWEEN=<s> between repeats (default 10).
#
# Idle pacing stays on, so the guest's idle CPU sleeps and the timing reflects
# the command's code. The guest keyboard layout must be US (keys_for.py).
# Compare the printed workload_seconds between binaries on a quiet host; the
# resolution is about 2 s (frame dumps), so pick a command that runs 100 s+.
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
[ $# -ge 5 ] || { sed -n '2,20p' "$0"; exit 2; }
LABEL=$1 BIN=$2 INST=$3 CFG=$4 CMD=$5
shift 5
case "$INST" in /*) SRC=$INST ;; *) SRC=$WORK/$INST ;; esac
[ -x "$BIN" ] || { echo "$LABEL: binary $BIN missing"; exit 2; }
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
[ -f "$SRC/$CFG" ] || { echo "$LABEL: $SRC/$CFG missing"; exit 2; }
if [ "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)" = 4 ]; then
  echo "$LABEL: memory pressure critical, not starting"
  exit 2
fi
D=$WORK/work-$LABEL
rm -rf "$D"
cp -c -R "$SRC" "$D" 2>/dev/null || cp -R "$SRC" "$D" || exit 2
rm -rf "$D/fb" && mkdir -p "$D/fb" && : > "$D/keys.txt"
cd "$D" || exit 2
env "$@" SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb ALPHABOX_KEYPIPE=keys.txt \
  "$BIN" run "$CFG" > run.log 2>&1 &
P=$!
echo "== $LABEL: pid $P, $(basename "$BIN") on $INST/$CFG: $CMD"
python3 "$T/win_workload.py" --fb fb --keypipe keys.txt --command "$CMD" --pid $P \
  --settle "${SETTLE:-60}" --boot-timeout "${BOOT_TIMEOUT:-400}" \
  --run-timeout "${RUN_TIMEOUT:-900}" --repeats "${REPEATS:-1}" \
  --between "${BETWEEN:-10}" | sed 's/^/  /'
rc=${PIPESTATUS[0]}
last=$(ls -t fb 2>/dev/null | grep ppm | head -1)
[ -n "$last" ] && python3 "$T/ppm2png.py" "fb/$last" last.png && echo "  last screen: $D/last.png"
kill $P 2>/dev/null # our own emulator only
for i in $(seq 1 20); do kill -0 $P 2>/dev/null || break; sleep 0.5; done
kill -0 $P 2>/dev/null && echo "  still running after SIGTERM!"
wait $P 2>/dev/null
grep -aE 'KEYPIPE|Emulator Failure|OPCDEC' run.log | sort | uniq -c | sort -rn | head -5 | sed 's/^/  /'
exit $rc
