#!/bin/bash
# SRM probe: boot a variant of the test machine and run console commands.
#
# usage: [VAR=value ...] srm_probe.sh <alphabox-binary> <label> [timeout_s]
#   PORT=<n>             telnet port (default 21100; one per parallel probe)
#   CPUS=1..4            number of CPUs (default 1)
#   MEMBITS=<n>          memory.bits (default: the test machine's 26)
#   SCSI=sym53c810|825|875|895 add the controller with a 1 GB disk + RAM disk
#   PLATFORM=<name>      machine to emulate (default es40)
#   ROM=<file>           console firmware image, copied into the run directory
#                        (default: the ES40's cl67srmrom.exe)
#   NIC=dec21143|de600|i82557|i82558|i82559  add the NIC (null backend)
#   NET_PEER=N:P         with NIC: UDP backend on port N, and net_peer.py on
#                        port P answering ARP/BOOTP/TFTP (log: peer.log);
#                        NET_PEER_OPT="..." adds net_peer.py options
#   EXTRA_CFG=<file>     configuration text for the machine block
#   IDE_CFG=<file>       drives for the ali_ide block
#   FLOPPY=<image>|halt  fdc0 with this image; "halt" generates the CALL_PAL
#                        HALT boot-block floppy (make_halt_floppy.py)
#   EXIT_ON_HALT=1       sys0 exit_on_pal_halt = true
#   CPU_OPT="k=v ..."    settings for every CPU; CPU1_OPT="k=v ..." for cpu1
#   CMDS="a|b|c"         console commands, '|'-separated (default: none)
#   CMD_TIMEOUT=<s>      per command (default 60)
#   AFTER=sigterm|disconnect-sigterm|wait-exit|none   (default sigterm)
#
# Output in $ALPHABOX_WORK/runs/probe-<label> (ALPHABOX_WORK defaults to
# <repo>/lab): es40.cfg, console.log (whole session), cmds.txt (the commands'
# output), alphabox.out. Prints the status lines, the command output and the
# emulator messages that matter for SMP, memory, PAL and exit-path checks.
#
# Examples:
#   CPUS=4 SCSI=sym53c810 CMDS="show device|init|show device" srm_probe.sh build-jit/alphabox smp4
#   MEMBITS=35 CMDS="show memory|show fru" srm_probe.sh build-jit/alphabox mem32g
#   FLOPPY=halt EXIT_ON_HALT=1 CMDS="boot dva0" AFTER=wait-exit srm_probe.sh build/alphabox halt
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 2 ] || { sed -n '2,40p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2
TMO=${3:-300}
PORT=${PORT:-21100}
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/runs/probe-$LABEL

[ -x "$BIN" ] || { echo "srm_probe: $BIN is not executable"; exit 2; }
if [ "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)" = 4 ]; then
  echo "srm_probe: memory pressure critical, not starting"
  exit 2
fi
rm -rf "$D" && mkdir -p "$D" || exit 2
cp "$R/test/rom/cl67srmrom.exe" "$D/"

cfg=(--out "$D/es40.cfg" --port "$PORT" --cpus "${CPUS:-1}")
[ -n "${MEMBITS:-}" ] && cfg+=(--membits "$MEMBITS")
[ -n "${SCSI:-}" ] && cfg+=(--scsi "$SCSI")
[ -n "${PLATFORM:-}" ] && cfg+=(--platform "$PLATFORM")
if [ -n "${ROM:-}" ]; then
  cp "$ROM" "$D/" || exit 2
  chmod u+w "$D/$(basename "$ROM")"
  cfg+=(--rom "$(basename "$ROM")")
fi
[ -n "${NIC:-}" ] && cfg+=(--nic "$NIC")
[ -n "${NET_PEER:-}" ] && cfg+=(--nic-udp "$NET_PEER")
[ -n "${EXTRA_CFG:-}" ] && cfg+=(--extra-cfg "$(cd "$(dirname "$EXTRA_CFG")" && pwd)/$(basename "$EXTRA_CFG")")
[ -n "${IDE_CFG:-}" ] && cfg+=(--ide-cfg "$IDE_CFG")
[ "${EXIT_ON_HALT:-0}" = 1 ] && cfg+=(--exit-on-halt)
for o in ${CPU_OPT:-}; do cfg+=(--cpu-opt "$o"); done
for o in ${CPU1_OPT:-}; do cfg+=(--cpu1-opt "$o"); done
if [ "${FLOPPY:-}" = halt ]; then
  python3 "$T/make_halt_floppy.py" "$D/floppy.img" || exit 2
  cfg+=(--floppy floppy.img)
elif [ -n "${FLOPPY:-}" ]; then
  cp "$FLOPPY" "$D/floppy.img" || exit 2
  cfg+=(--floppy floppy.img)
fi
python3 "$T/srm_cfg.py" "${cfg[@]}" || exit 2

con=(--port "$PORT" --log console.log --cmd-log cmds.txt --timeout "$TMO"
     --cmd-timeout "${CMD_TIMEOUT:-60}" --after "${AFTER:-sigterm}")
cmds=()
[ -n "${CMDS:-}" ] && IFS='|' read -r -a cmds <<< "$CMDS"
# ${a[@]+"${a[@]}"}: bash 3.2 (macOS) calls an empty array unbound under set -u
for c in ${cmds[@]+"${cmds[@]}"}; do [ -n "$c" ] && con+=(--cmd "$c"); done

cd "$D" || exit 2
PEER=
if [ -n "${NET_PEER:-}" ]; then
  # shellcheck disable=SC2086
  python3 "$T/net_peer.py" --nic "${NET_PEER%%:*}" --listen "${NET_PEER##*:}" \
    --timeout "$TMO" ${NET_PEER_OPT:-} > peer.log 2>&1 &
  PEER=$!
fi
"$BIN" run > alphabox.out 2>&1 &
PID=$!
con+=(--pid "$PID")
python3 "$T/srm_console.py" "${con[@]}" | sed 's/^/  /'
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null # our own emulator only
  for i in $(seq 1 40); do kill -0 $PID 2>/dev/null || break; sleep 0.5; done
  kill -0 $PID 2>/dev/null && { echo "  SIGTERM ignored, SIGKILL"; kill -9 $PID; }
fi
wait $PID 2>/dev/null
echo "  emulator exit code: $?"
[ -n "$PEER" ] && { kill $PEER 2>/dev/null; wait $PEER 2>/dev/null; }

echo "== $LABEL: command output"
[ -s cmds.txt ] && tr -d '\r' < cmds.txt | grep -av '^[[:space:]]*$' | head -80 | sed 's/^/  /'
if [ -n "$PEER" ]; then
  echo "== $LABEL: network peer"
  grep -E 'RRQ|complete|error|summary' peer.log | head -10 | sed 's/^/  /'
fi
echo "== $LABEL: CPUs seen by SRM"
tr -d '\000\r' < console.log | grep -aE 'CPU [0-9] speed|starting console on CPU' | sort | uniq -c | sed 's/^/  /'
echo "== $LABEL: emulator messages"
grep -aE 'Exiting gracefully|Emulator Failure|Exception in|STARTING \*\*\*|DPR \*\*\*|MEMTEST|NATIVEPAL|VMSPAL|UNKNOWNCFG|BADREST|Unknown TIG|MISMATCH|terminating' alphabox.out |
  sort | uniq -c | head -20 | sed 's/^/  /'
exit 0
