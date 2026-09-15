---
name: srm-boot-test
description: Run the headless SRM firmware boot test (boot to P00>>> over telnet, diff the console log) on one or more build lanes, and the SRM probes used for SMP, SCSI, memory-layout and exit-path changes. Use after any change to CPU, JIT, chipset, serial, DPR/TIG, memory or firmware code, and once per verified stage of larger work.
---

# SRM boot test

The canonical regression test: boot the SRM console firmware with the
no-VGA config (`test/rom/es40.cfg`), wait for `P00>>>` on the serial console
(telnet), and byte-compare the console log with `test/rom/axp_correct.log`.
`P00>>>` reachability is the real gate; the diff catches behaviour changes.

## Ground rules

- **Never `pkill`/`killall axpbox`.** Other sessions on this host run long
  guest installs. Stop only the PID you started (`kill $PID`), and check
  `sysctl -n kern.memorystatus_vm_pressure_level` before starting guests
  (4 = critical: wait).
- **Linux:** `cd test/rom && bash test.sh` works (it downloads
  `cl67srmrom.exe` itself). It deletes the tracked ROM files at the end
  (`git checkout -- test/rom/` before committing) and leaks the emulator on
  its timeout path.
- **macOS:** `test.sh` can never pass (BSD sed rejects `\x00` in its
  normalize step). Use the per-lane runner below.

## Per-lane runner (macOS and Linux)

Private run directory and telnet port per lane, so lanes can run in
parallel and nothing touches `test/rom`:

```bash
#!/bin/bash
# usage: PORT=<port> run_srm.sh <axpbox-binary> <label> [timeout_s]
export LC_ALL=C
BIN=$1 LABEL=$2 TMO=${3:-300} PORT=${PORT:-21000}
T=/path/to/axpbox/test/rom            # config, reference log
D=/path/to/scratch/srm-$LABEL
rm -rf "$D" && mkdir -p "$D"
sed "s/port = 21000;/port = $PORT;/" "$T/es40.cfg" > "$D/es40.cfg"
cp "$T/cl67srmrom.exe" "$D/"          # or download it once
cd "$D" && : > axp.log
"$BIN" run > axpbox.out 2>&1 & PID=$!
sleep 5; nc -d -t 127.0.0.1 "$PORT" > axp.log & NC=$!
t=0; status=timeout
while [ $t -lt "$TMO" ]; do
  [ "$(tr -d '\000' < axp.log | sed -n '$p')" == "P00>>>" ] && { status=prompt; break; }
  kill -0 $PID 2>/dev/null || { status=died; break; }
  sleep 1; t=$((t + 1))
done
echo "status=$status elapsed=${t}s"
kill $NC $PID 2>/dev/null; wait $PID 2>/dev/null
norm() { tr -d '\000' < "$1" | sed 's/CPU [0-9] speed is.*//'; }
[ "$status" = prompt ] && diff -c <(norm "$T/axp_correct.log") <(norm axp.log) > diff.txt \
  && echo "diff clean" || echo "DIFF or FAIL (see diff.txt)"
echo "mismatch lines: $(grep -ac MISMATCH axpbox.out)"
```

A local copy lives in the git-excluded `lab/run_srm.sh`.

## Which lanes

Run the lanes the change can affect (see the `build-lanes` skill), each on
its own port. A healthy boot takes 15-35 s.

| Lane | Why |
|---|---|
| `build` | interpreter |
| `build-jit` | AArch64 JIT (this host) |
| `build-jit-verify` | every compiled block checked against the interpreter: expect `[JIT][VERIFY] ... 0 mismatches` |
| `build-jit-verify-x64` | x86-64 emitter under JIT_VERIFY (Rosetta) |
| `build-jit-x64` | the only lane running the x86-64 chain gates and links: JIT_VERIFY compiles the gates out |

## Pitfalls (all hit in practice)

- **Output arrives late, not a hang.** A lane's result prints when the lane
  finishes (buffered through pipes). Before calling a hang, check
  `ps -o etime= -p <pid>` and the tail of that lane's `axp.log`.
- **SIGTERM is graceful, but only once the main loop runs.** A signal
  during startup (before `P00>>>`) kills the process with stdout still
  buffered, so its log is empty. Wait for the prompt before stopping a run
  whose output you need.
- **Stale ROM cache**: a `decompressed.rom` left by a crashed run is garbage.
  Delete `decompressed.rom flash.rom dpr.rom` after a crash.
- **Port conflicts**: a leftover instance holds the port and the new one's
  serial silently gets no connection.
- **The "CPU n speed is" line is filtered**: SRM measures it from the
  wall-clock RPCC, so it is host-dependent.
- **`axp_correct.log` has NUL bytes** (`grep -a`, `LC_ALL=C` for tr/sed).
  Refresh it only after reading the diff, for an intentional output change.
- **Shell**: the Bash tool runs zsh, where unquoted `$vars` don't split.
  Write lane loops as bash scripts with arrays, and copy a script before a
  background job runs it (editing a running script corrupts that run).
- The emulator blocks in serial init until a telnet client connects.

## SRM probes

Same boot, different config, driven over telnet (a small Python client that
waits for `P00>>>`, sends a command, waits again). Local scripts in `lab/`:
`srm_device_probe.sh`, `srm_cmds.sh`, `srm_exit_probe.sh`,
`srm_floppy_probe.sh`.

- **SMP / SCSI** (4-CPU config trimmed to N CPUs, Sym53C810 + disk):
  `show device`, `init`, `show device` with 1, 2 and 4 CPUs. After `init`
  every secondary must print `starting console on CPU n` and the second
  `show device` must list the disks. SRM writes each secondary's DPR start
  register three times; only the first may start it (the others log
  `CPU n is already running, not redirected`).
- **Memory layout** (`memory.bits` 26..35): `show memory`, `show fru`,
  `show config`. Above 8 GB memory shows as arrays of 8192Mb (16 GB: 2,
  32 GB: 4); `show fru` lists DIMMs matching the size; no DIMM error lines.
  34/36-style out-of-range values end with `Emulator Failure: ...
  memory.bits must be between 26 (64 MB) and 35 (32 GB)`.
- **Exit paths**:
  - A guest HALT can't be started from the SRM prompt (`start`/`continue`
    give "invalid processor ID" / "Slot context is not valid"). Boot a
    floppy instead: block 0 with count=1 at 0x1E0, start LBA=1 at 0x1E8,
    flags=0 at 0x1F0 and the sum of the first 63 quadwords at 0x1F8; block
    1 all zeros (longword 0 = CALL_PAL HALT); `boot dva0`. With
    `exit_on_pal_halt = true` in sys0 the emulator exits gracefully ~3 s
    later; without it SRM prints "HALT instruction executed".
  - SIGTERM at the prompt, and telnet disconnect followed by SIGTERM: both
    exit gracefully in well under a second (the latter aborted before
    a6142d0).

## Manual boot

```bash
cd <run dir with es40.cfg + cl67srmrom.exe>
/path/to/build/axpbox run > run.log 2>&1 & PID=$!
sleep 6; nc -t 127.0.0.1 <port> > axp.log &
# poll: LC_ALL=C sed -n '$p' axp.log | tr -d '\000'  == "P00>>>"
kill $PID   # graceful: flash.rom and dpr.rom are saved
```

## Debugging a boot hang

- `AXPBOX_PC_SAMPLE=1` prints every CPU's PC each check_state pass;
  `AXPBOX_IRQSTATS=1` prints interrupt rates every 5 s.
- Debugger: attach as the parent (`lldb -- build/axpbox run` on macOS,
  `gdb -batch -ex run -ex 'thread apply all bt 14' --args ...` on Linux).
- A guest spinning on garbage usually means a runaway PC: anything that
  writes `state.pc` directly must reset the fetch cursor via
  `set_pc()`/`break_seq_icache()`.
- Playbook that found earlier upstream bugs: sample the guest PC over time;
  ring-buffer (pc, ins, pc_phys) dumped at the first bad PC; diff a
  "frompc>topc" control-flow trace between a good and a bad binary.
