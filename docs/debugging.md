# Debugging a guest with gdb

Alphabox has a gdb remote stub, like QEMU's `-gdb tcp::1234`: gdb attaches
over TCP and sees the machine's processors as threads. It stops them,
sets breakpoints, single-steps, and reads and writes registers and memory,
in the interpreter and in JIT builds alike. `monitor` commands show what
gdb has no register for: PALmode, the internal processor registers, the
TBs, interrupts and the JIT's blocks.

The code is in `src/debug/`: `CGdbStub` speaks the protocol,
`CDebugTarget` stops and starts the processors and reads their state.

| Section | What is in it |
| --- | --- |
| [Attaching](#attaching) | the configuration key, the environment hooks, a first session |
| [What gdb sees](#what-gdb-sees) | registers, PALmode, memory, threads |
| [Breakpoints and stepping](#breakpoints-and-stepping) | what stops where, and how gdb steps an Alpha |
| [Monitor commands](#monitor-commands) | the Alpha state gdb cannot express |
| [The JIT](#the-jit) | how a breakpoint stops compiled code |
| [Time while stopped](#time-while-stopped) | the guest's clock stands still |
| [What is left out](#what-is-left-out) | watchpoints, symbols, the limits |
| [Testing it](#testing-it) | `test/tools/gdb_check.sh` |

## Attaching

In the `sys0` section of `es40.cfg`:

```
sys0 = tsunami
{
  gdb = 1234;        // listen on 127.0.0.1:1234 and [::1]:1234
  gdb.wait = true;   // hold every processor before its first instruction
  ...
```

or, without touching the configuration, `ALPHABOX_GDB=1234` and
`ALPHABOX_GDB_WAIT=1` in the environment (they override the keys). The
stub listens on the loopback interface only, in both families:
`127.0.0.1` and `::1`, so `target remote localhost:1234` works whichever
address the host's resolver gives gdb first, and `target remote
[::1]:1234` and `target remote 127.0.0.1:1234` name one. A host with only
one of them has that one (`%GDB-I-SKIP` names the other). Use an SSH
tunnel to debug from another host. It prints `%GDB-I-LISTEN` when it is
ready (`gdb stub on 127.0.0.1:1234 and [::1]:1234`), and each attach
says where gdb came from (`%GDB-I-ATTACH: gdb attached from
[::1]:54459`).

The stub starts just before the processors' threads. By then the
configuration has been read, a telnet serial port has had its client
(a `serial` section with a `port` waits for one first), and the console
image has been decompressed: that decompressor runs on the main thread at
startup, outside the debugger's reach. With `gdb.wait` the first
instruction a processor stops at is the console's own entry, `0x8000` in
PALmode on the ES40.

Then, with gdb 17 (`brew install gdb` on macOS has the Alpha target):

```
$ gdb
(gdb) target remote localhost:1234
0x0000000000008000 in ?? ()
(gdb) info threads
  Id   Target Id                        Frame
* 1    Thread 1 (cpu0, PALmode, kernel) 0x0000000000008000 in ?? ()
  2    Thread 2 (cpu1, not started)     0x0000000000008000 in ?? ()
(gdb) break *0x13880
(gdb) continue
(gdb) x/8i $pc
(gdb) stepi
(gdb) monitor ipr
(gdb) detach
```

The stub sends gdb a target description naming the Alpha, so `set
architecture alpha` is not needed. Attaching stops the machine; `detach`
removes every breakpoint and lets it run; `kill` ends the emulator the
orderly way (flash and DPR saved), as Ctrl-C at its own terminal would.
Ctrl-C in gdb stops a running machine. One gdb at a time; after a detach
another can attach.

Without an executable gdb's Alpha unwinder has no function bounds, and
prints "Hit beginning of text section" on most stops. It is harmless.

## What gdb sees

**Registers** are gdb's Alpha numbering (gdb's `alpha-tdep`): `r0`-`r31`,
`f0`-`f30`, `fpcr` in `f31`'s place, `pc`, an unused slot and `unique`,
eight bytes each.

- A GPR is the one the instruction at the PC would name: in PALmode with
  `I_CTL[SDE]` set, `r4`-`r7` and `r20`-`r23` are the PALshadow bank.
  `monitor ipr` shows the shadow bank whatever the mode.
- `pc` is the address without the PALmode bit, which is what gdb needs to
  disassemble and to match breakpoints. Whether the processor is in
  PALmode is in the thread list, `monitor cpu` and `monitor ipr`. Writing
  `pc` keeps the processor's mode.
- `fpcr` is the architectural FPCR (ARM 4.7.8).
- `unique` is the PALcode's per-process value, kept wherever that PALcode
  keeps it; gdb shows it as unavailable.

**Threads** are processors: thread N is cpu(N-1). All-stop: when one stops
(a breakpoint, a step, Ctrl-C), all of them stop, and `continue` starts all
of them. A secondary the console has not started yet is listed as "not
started" and has no instruction to stop at; it stops at its first one if
the console starts it while the machine is stopped.

**Memory** is read through the current thread's processor, without side
effects: no TB fill, no fault delivered, no device register read.

- In PALmode the processor fetches by physical address, and so does the
  stub: `x/8i $pc` works wherever the PC is.
- Otherwise an address is virtual and goes through the processor's
  current mapping: the superpages, the instruction and data TBs (under the
  current ASN) and the data TB's shadow. An address none of them maps is
  an error, as in any gdb session.
- `monitor mem virt` and `monitor mem phys` override the choice;
  `monitor mem auto` restores it.
- Only DRAM is reached. A device's registers are never read on a
  debugger's behalf, since reading one is an access the device answers.
- A write to guest memory is reported to the code-page map, and every
  processor drops its instruction cache when it resumes, as after an IMB:
  patched code runs patched, compiled or not.

## Breakpoints and stepping

Breakpoints are kept by the stub (`Z0` packets; `Z1`, gdb's `hbreak`, is
the same thing here). Guest memory is never written, so a breakpoint works
in ROM, in code another processor is running, and in compiled code. A
breakpoint is an address with the low two bits ignored, and it matches in
any mode: a breakpoint at a PAL entry (`break *0x8680`, the interrupt
entry at PAL_BASE 0x8000) stops in PALmode.

A processor stops before it executes the instruction at the breakpoint.
Resuming from it runs that instruction once without stopping again.

gdb steps an Alpha the way its `alpha-tdep` does on any target: it decodes
the instruction at the PC, plants breakpoints on the instruction's
possible successors and continues. So a `stepi` is a short continue, with
interrupts enabled: a step can come back from an interrupt handler. The
stub's own single step (`vCont;s`, which `maint packet vCont;s:1` sends)
executes one instruction with interrupts held off, and leaves a pending
interrupt pending.

## Monitor commands

`monitor <command>`, for the current thread's processor where one is
meant. Numbers are C-style: `0x` for hex.

| Command | Shows |
| --- | --- |
| `help` | the list |
| `cpu` | every processor: state (stopped, running, not started), PC with the PALmode bit, mode, ASN, PAL_BASE, instructions executed, which one stopped the machine |
| `ipr` | the internal processor registers: PAL_BASE, EXC_ADDR, EXC_SUM, VA, MM_STAT, I_CTL, VA_CTL, M_CTL, CM, ASNs, IER, SIRR, PCTX, the cycle counter, FPCR and the PALshadow bank |
| `tlb [i\|d]` | the valid ITB and DTB entries: virtual, physical, page size, ASN or ASM, read and write enables per mode, fault bits |
| `xlate <va>` | how the processor maps a virtual address, and which entry does it |
| `pr <pa> [n]` | `n` quadwords of physical memory |
| `irq` | every processor's external, software and AST requests and enables, whether one is deliverable, and the delayed IRQ_H timers |
| `jit [<va>]` | the JIT's blocks that cover an address (default: the PC): compiled or not, current or awaiting re-validation, whether chains into them are valid, and whether a breakpoint keeps one interpreted |
| `mem [auto\|virt\|phys]` | how gdb's memory accesses are addressed |

`ipr`'s cycle counter is what `RPCC` would read at that moment of guest
time, which stops when the processor parks.

## The JIT

While no debugger is attached the dispatcher (`CAlphaCPU::jit_run`) tests
one flag per block it dispatches, the computed-jump helper one per jump it
chains, and nothing else changes; no instruction pays anything. While one is
attached it asks the stub, before every block, whether to park, and how
far the processor may run before the next breakpoint in straight-line
code:

- an interpreted span stops before the breakpoint, so the dispatcher sees
  it as the next block's start, and stops there;
- a compiled block runs only if no breakpoint is inside it; otherwise its
  instructions are interpreted up to the breakpoint;
- a jump, an exception or an interrupt always comes back to the dispatcher
  first, where the same question is asked;
- a compiled block can also be entered without the dispatcher, by a chain
  from another block or by a computed jump. When the breakpoints change,
  every processor moves its JIT's epoch on, which makes every cached link
  and computed-jump entry miss; the dispatcher only links blocks it runs
  compiled, and the computed-jump helper refuses a block that holds a
  breakpoint.

Nothing is recompiled or dropped: a block keeps its code and runs compiled
again when its breakpoints go. `monitor jit` shows which blocks a
breakpoint is holding. The interpreter-only build checks between batches;
while a breakpoint is set its batches shrink to one instruction.

## Time while stopped

Guest time stands still while the machine is stopped. When a processor
resumes, its cycle counter's last sync, its interval-timer schedule and
its speed calibration are moved on by the time it was parked, so `RPCC`
and the interval timer carry on from where they stopped: no backlog of
timer ticks arrives, and a guest that measures elapsed time sees none
pass. The devices are not stopped: their threads go on, and an interrupt
one raises is pending when the processors resume. The TOY clock keeps
real time, as on a machine that was halted at the console.

## What is left out

- Watchpoints (`Z2`-`Z4`): no access is watched.
- Non-stop mode, reverse execution, the `x` and `X` binary memory packets
  (gdb falls back to `m` and `M`).
- `unique` (above), and any register beyond gdb's Alpha set.
- Symbols: gdb loads none from the stub; load a guest image with
  `symbol-file` if one with symbols exists.
- The ROM decompression at startup (above).
- Hosts without POSIX sockets (Windows) and `ALPHABOX_HV=1`.

## Testing it

`test/tools/gdb_check.sh` runs a scripted gdb against the SRM console and
prints one PASS or FAIL line per check:

```
PORT=21300 GDB_PORT=21301 test/tools/gdb_check.sh build-jit/alphabox jit
PORT=21302 GDB_PORT=21303 test/tools/gdb_check.sh build-jit/alphabox jit2 2
```

The third argument is the number of processors. It attaches at reset,
stops at a console routine, reads registers and memory, steps, writes and
restores a register and a memory word, runs the monitor commands, lets the
console reach `P00>>>`, then attaches again and:

- stops at a breakpoint inside the console's scheduler loop, which the
  JIT has compiled by then;
- checks that the cycle counter did not move across a two-second stop;
- steps from the head of that compiled block, one instruction per step;
- stops the running machine with Ctrl-C.

It finally diffs the console log against `test/rom/axp_correct.log` (one
processor) and counts JIT_VERIFY mismatches.

gdb connects to `localhost`; `GDB_HOST='[::1]'` (or `127.0.0.1`) makes
it one family. Without gdb, `test/tools/ipv6_check.sh`'s `gdb6` section
speaks the protocol itself over `::1` (the stop reply, `g`, a detach).
