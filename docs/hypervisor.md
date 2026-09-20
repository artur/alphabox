# Hypervisor.framework as an accelerator: the analysis

Written 2026-09-19, against the code and the numbers in
`docs/performance.md`. The question was "at which stage should we consider
Hypervisor.framework for the macOS build?" The answer is a stage, not a
date, and it is defined by one measurement that has not been made yet.

## What it can and cannot do

Hypervisor.framework virtualises the host ISA. Our guest is Alpha, so a VM
cannot run guest code; it can only run *our emitted ARM64 code*, and the
one thing it offers that user space cannot is the hardware MMU. The design
that makes sense is therefore:

- the CPU core -- the emitted code, the helpers, the interpreter, the block
  cache and the JIT compiler -- runs at EL1 inside a VM, on a vCPU per
  emulated processor;
- the devices, their threads, SDL, pcap and the disk files stay outside, in
  the ordinary process;
- guest DRAM is one shared mapping (`hv_vm_map`) both sides see;
- the VM's stage-1 page tables, which the in-VM runtime owns, *are* the
  Alpha translation buffer: an Alpha 8 KB page becomes two 4 KB stage-1
  entries (4 KB granule; stage 2 is macOS's business and its 16 KB
  granule is irrelevant to us), ASN maps to ASID, the four Alpha
  processor modes become four page-table sets per ASN selected by ASID
  bits (a `TTBR` write, no TLB flush), KSEG becomes block mappings, and
  PAL-mode physical accesses get an identity alias.

A guest load is then one `ldr`. A TB miss is a data abort taken by our own
EL1 vector inside the VM and turned into the Alpha DTB-miss PAL entry;
`tbis`/`tbia`/ASN switch become `TLBI`/ASID changes instead of the
page-cache flushes and epoch bumps we do today; the 47 ns miss helper and
the 128-entry TB scan disappear because the shadow *is* the page table.
Unaligned accesses trap through `SCTLR.A` as Alpha requires.

## What it costs, per access class

The boundary is where this lives or dies. Three kinds of traffic cross it:

| traffic | today | in the VM |
| --- | --- | --- |
| helper calls (page-cache miss, IPRs, locked ops, indirect jumps) | 10-50 ns, in-process | unchanged: the helpers run inside |
| device interrupts, timers | atomic flags polled at block boundaries; wall-clock counter | unchanged: shared-memory flags polled at the same gate; `CNTVCT_EL0` readable at EL1 |
| **MMIO** (device registers, the S3 aperture) | a helper call, ~100 ns | **a VM exit and return, microseconds** |

The MMIO count decides it. The page-cache census over one Windows 2000
boot-plus-benchmark run shows **~140M MMIO accesses from compiled code**
(55% of all page-cache misses). At 1-3 us per exit that is 140-420 s added
to a 143 s boot -- unless the linear framebuffer is mapped as RAM inside
the VM and rendered from outside with dirty tracking, which is what real
hardware does and what the S3 core would have to learn. Register accesses
would still exit. The benchmark itself has no MMIO, so a benchmark number
would flatter the design; the boot and any GUI workload would show the
truth.

## Measured on this Mac: `alphabox hvprobe`

The mechanics above are no longer assumed. `-DALPHABOX_HVF=ON` (macOS on
Apple silicon, with the JIT's asmjit; the build signs the binary ad hoc
with `com.apple.security.hypervisor`) adds `alphabox hvprobe`, which
creates a VM, maps 32 MB, generates its guest code with asmjit at IPA ==
VA and measures, with nothing emulated (M3 Max, 2026-09-19, two runs):

| what | measured |
| --- | --- |
| `ID_AA64MMFR0_EL1` | **ASID 8 bits**, PA range 40 bits, 4 KB and 16 KB granules yes, 64 KB no |
| exit round trip, HVC (`hv_vcpu_run` returns, resumes) | **0.7-1.2 us** |
| exit round trip, a load from an unmapped IPA emulated on the host (the MMIO case: read the fault, write the register, step the PC, resume) | **0.9-1.0 us** |
| a stage-1 fault taken at EL1 inside the VM, the page mapped by the guest's own vector, `eret`, the load retried (the TB-miss case; no exit) | **170-220 ns** |
| a page touch with the page mapped (the VM's TLB doing its job) | 8 ns |
| the VM's raw speed, a 5-instruction loop | ~17.5 G instructions/s |

Three of the design's numbers change with that:

- **The ASID has 8 bits, exactly one per Alpha ASN.** The four processor
  modes cannot be folded into it. Mode separation would have to come from
  switching table sets (a `TTBR0` write per PAL entry and exit, with the
  TLB entries of the other mode either flushed or left reachable), or from
  checking protection in software on the paths where it differs -- the
  global-page problem below becomes moot and a different one takes its
  place.
- **An in-VM TLB miss costs 170-220 ns, against ~47 ns for today's software
  page-cache miss** (less again with the TB index). The hardware MMU wins on
  the hit path (one load instead of the probe) and loses on every miss, so
  the balance depends on the miss rate of real code, which the profiler can
  give per section.
- **An MMIO access costs 0.9-1.0 us, against 12-20 ns today.** At the 140M
  MMIO accesses a boot-plus-benchmark run makes from compiled code that is
  130-140 s added to a 143 s boot, so the framebuffer would have to be
  shared memory before the design could even be tried on a booting guest.

The probe also stands as the first piece of the prototype's checklist: the
EL1 vector, the stage-1 tables with a 4 KB granule and the fault-and-map
loop all work as designed. It is not an emulator component and does not
run any Alpha code.

## Three gaps a reviewer added

- **Global pages do not mix with modes in the ASID.** An ARM TLB entry
  marked global matches under every ASID, so a kernel page translated with
  the global bit -- the natural rendering of Alpha's ASM bit -- would be
  reachable while the user-mode table set is selected. Either never mark
  entries global and pay more refills per context switch, or use the global
  bit only for pages whose permissions are identical in all four modes.
- **Nothing protects the runtime from the guest.** The emitted code and the
  runtime share one EL1 address space; a stray guest pointer into the
  runtime's memory would succeed where Alpha owes an access violation, and
  a stray store would corrupt the emulator. Hiding the runtime in a range
  that is non-canonical for a 43-bit Alpha VA stops working the moment a
  guest enables the EV6's 48-bit mode. The alternatives -- emitted code at
  EL0 with the runtime privileged, or a canonical-address check per access
  -- both cost, and belong on the prototype's checklist.
- **Two assumptions, now verified** (see the probe above): the ASID width
  is 8 bits, not 16, so the mode cannot live there; an exit round trip is
  0.7-1.2 us. Idle still belongs on the list -- a guest `WFI` must exit so
  the host thread can sleep.

The compiler can stay outside the VM at first: the code cache is shared
memory and compiles are rare, which postpones porting asmjit to the
freestanding runtime.

## What the device traffic actually is

The census above counts page-cache misses on device pages; it does not say
which pages. A per-address histogram of every device access the helpers
served over one Windows 2000 boot-plus-benchmark run (JIT_STATS,
`dump_device_pages`, 2026-09-20; ~158M accesses, 93% captured) says:

| address | what | reads | writes |
| --- | --- | --- | --- |
| port 0x1F0 | **IDE data port: PIO disk transfers** | 81.6M | 10.7M |
| port 0x61 | system control port B: the refresh-toggle bit a HAL stall loop counts (real time) | 22.1M | -- |
| port 0x3BC/0x3BD | parallel-port status polling | 18.0M | -- |
| port 0x42 | PIT channel 2, the other delay timer | 7.4M | 3.7M |
| `0x801a0000280` | Tsunami Pchip 0 TBA2, read in a loop during the loader phase | 8.6M | -- |
| `0xA0000` | the legacy VGA window (text during the boot) | 3.7M | 1.1M |
| ports 0x3D8, 0x3CC, 0x3D4, 0x3C4, 0x3DA | VGA registers | 3.1M | 0.8M |
| BAR0, the S3's linear framebuffer | -- | below 20k | below 20k |

**The framebuffer is not in it.** Windows 2000's S3 driver draws through
the accelerator and its ports; the CPU never writes pixels into the linear
window. So the prerequisite this document named -- the framebuffer as
shared memory -- was the wrong one: it is implemented now (the S3 offers
BAR0's VRAM to the CPUs' page caches, `ALPHABOX_LFB_DIRECT`; a resumed
desktop draws through it, and it changes nothing measurable on this
guest), and it does not touch the cost. The cost is device *registers*,
and 58% of it is one of them: the IDE data port, because the guest moves
its disk data by PIO, one word per port read. Under a hypervisor every one
of those is an exit at ~1 us: ~90 s per boot for the disk alone. What
would remove it is not shared memory but a guest that uses bus-master DMA
for its disks, or an in-VM model of the polled registers (the data port,
the keyboard controller, the PIT, the Pchip CSRs) -- a much larger design
than this document described.

## The MMIO census, priced for today's build

The same census also says something about the emulator as it is. 140M
MMIO accesses from compiled code at a helper call each is real time now,
and the reviewer's estimate was ~14 s at 100 ns a call. Measured instead
(helper timer, whole boot-plus-benchmark run): **helpers take 5.6 s of
149 s (3.8%) in total**, and the read+write helper time in every window
where those two exceed 10% -- the MMIO-heavy ones -- sums to **3.3-3.5 s
per run (2.4%)**, at 12-20 ns a call, not 100. So mapping the S3 linear
framebuffer as ordinary guest memory on the page-cache fast path (packed
linear modes only; the banked window and the planar modes stay MMIO; the
renderer already reads VRAM every frame, so no dirty tracking is needed in
the process build) is worth about 2% of a boot today, plus whatever the
page-cache slots those MMIO pages occupy cost the other misses. A modest
win on its own, and a prerequisite for the VM design.

## What it can gain

Memory ops are **13.7% of hot instructions** on the benchmark (exec-weighted,
`JIT_REGPROF`; four of the nine sections barely touch memory, so a
real-code workload -- the `cab` one -- must confirm it before this figure
is trusted). Today's probe puts about ten host instructions on the address
chain per access; the hardware MMU removes all of them. The reviewer's
bound with that share is ~1.3x on the memory path, not the 2x a
one-in-three memory mix would have given. Everything else that makes us
2.9 host cycles per guest instruction -- block length (a block ends every
~3.8 instructions), the exit sequences, the PC store, the gate -- is
untouched by a hypervisor.

## What it costs to build

- A freestanding EL1 runtime: exception vectors, a page-table manager, an
  allocator, a `printf` over a shared ring. The core as it stands calls
  `printf` in ~180 places, allocates, and uses `std::` containers; asmjit's
  `JitRuntime` wants `mmap`/`mprotect` and would get our page-table hooks
  instead.
- A boundary protocol: synchronous exits for MMIO reads (a value must come
  back), a queue for writes, and a doorbell for interrupts. Seven call
  sites in `AlphaCPU_jit.cpp` reach `cSystem` today (`ReadMem`, `WriteMem`,
  `interrupt`, the lock helpers, `get_tick_seq`); the interpreter has more.
- No debugger inside the VM: our own state dumps, and JIT_VERIFY running
  inside too (it can -- the interpreter is in there).
- A signed binary: `com.apple.security.hypervisor` is an entitlement
  (ad-hoc signing works locally; distribution needs a Developer ID).
- macOS/Apple-silicon only. Intel Macs would need the VMX flavour (a
  second port), Linux would need KVM (a third), and the x86-64 lane keeps
  the software MMU regardless. Two memory paths to keep correct.

The user-space alternative (reserve the Alpha address space, `mmap` guest
pages into it, take `SIGSEGV` on a miss) is not available on Apple
silicon: host pages are 16 KB and cannot mirror 8 KB guest pages. It
would work on Linux and x86, at a system call per fill and a
microsecond-class fault.

## The stage

Consider it when all three hold:

1. the per-block costs that a hypervisor cannot touch have been taken
   down -- the hot-path PC store, the down-counter, exit-record
   addressing, longer blocks -- and the residual is measured;
2. the memory path is measured as a dynamic cost, not counted: add dead
   instructions on the address chain and the same number off it, take the
   two slopes, extrapolate to zero (a wrong-result "cheat build" times a
   different program and is not a measurement); and it is done on a
   real-code workload, not only on the benchmark;
3. that measurement says the memory path is at least a quarter of emitted
   code time, and the S3 framebuffer can be shared memory.

If it comes to that, the first prototype is not the JIT: it is the
*interpreter* alone at EL1 with the hardware TB, because that validates
the boundary, the page-table mapping, the exit cost and the framebuffer
sharing with none of the code-generation risk. The JIT moves in second.

## The build

The stage above was not waited for: the module is being built to measure
rather than to predict, in phases, each with a switch and a test.

### The model: the process's own code at EL1

The doc's original design put a freestanding EL1 runtime inside the VM:
its own vectors, allocator and `printf`, and the CPU core ported to it.
What is built instead is smaller and keeps the whole emulator as it is.
A VM whose stage-1 page tables we own maps *this process's address space
onto itself*, so an ordinary function of the alphabox binary runs at EL1
on a vCPU with the same pointers it uses outside -- the heap it shares
with the device threads, the guest DRAM array, the JIT code cache, the
stack it was given. Only three things differ from running outside:

- **Memory is mapped on first touch, and always as a copy.** Only memory
  the runtime allocated through `hv_vm_allocate` is ever passed to
  `hv_vm_map`: the page tables, the vCPU stacks, the copy arena, and what
  `hv::alloc()` hands out. Every other page the VM touches -- the
  binary's text and data, the dyld shared cache, the commpage, the
  process heap, the process stack -- is copied into the arena and mapped
  at its original virtual address, so the pointers still work but the
  bytes are a private snapshot. Code is immutable, so its copy is exact;
  writable memory diverges, and a global or a `malloc`'d block written
  inside the VM is not seen outside. Anything the inside and the outside
  must share has to come from `hv::alloc()`.

  This is not a preference. Whether the framework will accept a page is a
  property of the *physical frame*, enforced by the kernel's page-table
  monitor, and nothing in user space predicts it -- the refusal is a host
  panic, not an error return. One refused kind is known by name: a page of
  a `MAP_JIT` mapping that has been used as one, which is what asmjit's
  code cache is made of. The framework documents `hv_vm_allocate` as the
  memory "suitable to be mapped as guest memory", and that is the whole of
  what may be offered to it. There is no switch to do otherwise.
- **System calls are proxied.** An `svc` at EL1 lands in our vector,
  which saves the registers in a frame on the vCPU stack and leaves the
  VM with an HVC. The host thread -- the same pthread, so the same
  process, pointers and thread identity for the kernel -- issues the
  same `svc #0x80` with the same registers, BSD and Mach alike, and
  writes the result and the carry flag back into the frame before the
  ERET. Uncontended locks never leave the VM; a contended one costs an
  exit and the wait it would have cost anyway.
- **A few registers are answered by the host.** The framework traps the
  Apple counter register that `mach_absolute_time` reads
  (`S3_4_C15_C10_6`) and the implementation-defined ones the JIT
  write-protect switch flips; the first is answered with the host's
  clock, the others are no-ops (stage 1 is ours and has no W^X). The
  vCPU's generic counter starts at the physical counter rather than at
  what the process reads as `cntvct_el0`, so each vCPU's timer offset is
  calibrated at creation until the two agree within 2 us -- the CPU core
  compares its counter reads with the host's.

This is the model of Dune (Belay et al., 2012) on Hypervisor.framework:
a process with EL1 privileges and its own page tables, everything else
untouched -- with the one difference that Dune's process keeps *one*
copy of its memory, and this one does not. That difference is what
decides the phases below.

### Phase 1: the runtime and its self-test (done)

`src/hv/HvRuntime.cpp`, built with `-DALPHABOX_HVF=ON` (needs the JIT's
asmjit for the vectors; the binary is signed ad hoc with the hypervisor
entitlement). `alphabox hvtest` runs its checks inside the VM:
call/return; `malloc`, libc and `write(2)` inside; `mach_absolute_time`
and `steady_clock`; `printf`; memory from `hv::alloc()` written inside
and read outside; a 64 MB strided sum over that memory with the same
result as outside; a call from a second thread on its own vCPU;
`cntvct_el0` equal to the host's. It also states whether a global
written inside was seen outside, which under the copy rule is "no".

Measured before the rule changed, with the heap mapped directly, the
strided sum ran **1.2-1.3x slower inside** -- two-stage translation on a
workload with poor locality; a stage-1 walk that misses costs a stage-2
walk per level. The same figure over `hv::alloc()` memory has not been
taken yet.

`hvtest --regions` lists every region of the process with how the kernel
describes it, and creates no VM; `ALPHABOX_HV_TRACE=1` prints every
mapping, exit and proxied call; `ALPHABOX_HV_WATCHDOG=<s>` cancels a
vCPU that never exits and prints where it was.

Two things learned building it: asmjit's `align()` stops at 64 bytes,
so the 128-byte exception-vector entries are padded by hand (an aligned
table that was not is a vector landing inside the handler, and a fault
loop with no exit to show for it); and code the host writes is fetched
by the vCPU from the point of unification, so it is cleaned out of the
data cache first.

### Phase 2: the CPU loop inside (wired, not yet exercised)

`ALPHABOX_HV=1` makes every CPU thread run its dispatch loop --
interpreter, JIT compiler, compiled code, and the device models the
guest's MMIO reaches through `cSystem->ReadMem` -- at EL1 on that
thread's vCPU (`CAlphaCPU::run` enters `run_loop` through `hv::call`).
Device threads, SDL, pcap and the disk files stay outside.

The copy rule above is what this phase has to be built around, and it is
not a detail: the CPU loop and the device threads communicate through
ordinary memory -- guest DRAM, the interrupt flags in the CPU object,
the JIT's code cache, the disk buffers -- and none of it is shared while
it comes from `calloc` and `new`. Every such structure must move to
`hv::alloc()` before the loop can run inside for longer than a boot
message: guest DRAM (`CSystem::memory`), the `CAlphaCPU` objects, the
JIT code cache and its block tables, and the device state the CPU thread
reaches. Allocation *inside* the VM is the same problem seen from the
other side: `malloc` called inside works on the private copy of the
heap, so a block allocated inside and freed outside corrupts the
allocator. The loop must not allocate.

One concrete obstacle is already known: asmjit calls
`pthread_jit_write_protect_np` around every code emission, and that
function traps on an implementation-defined register the framework
intercepts, checks the result, and executes `BRK` when it does not take
effect -- so the JIT compiler cannot run inside as it stands. The
interpreter can. That makes the first honest phase-2 experiment the
*interpreter* at EL1 with guest DRAM in `hv::alloc()` memory, compiling
outside or not at all, measured against the same binary with the switch
off.

### Phase 3: the hardware translation buffer

The gain the hypervisor was priced for, and the part the copy rule does
*not* obstruct: guest DRAM is `hv::alloc()` memory, so the stage-1
entries point at pages both sides own. The compiled code has to move
there too -- asmjit's `JitRuntime` allocates `MAP_JIT` memory, which is
the one kind the monitor is known to refuse, so blocks are emitted into
`hv::alloc()` memory and made executable by our own stage-1 entries
(asmjit assembles into a `CodeHolder` and copies out, which the runtime's
vectors already do). Alpha seg0 becomes a window in
the low half of the VM's address space (guest VA plus a constant, above
anything the process maps), kseg and seg1 live in the high half
(`TTBR1`), and the four processor modes times 256 ASNs map onto 256
hardware ASIDs through an allocator that recycles the least recently
used pair with a `TLBI`. The stage-1 entries are filled *lazily from the
software TB* on the in-VM translation fault (measured at ~200 ns): the
software TB stays the truth, the hardware tables are its cache, and
`tbis`/`tbia`/ASN switches become `TLBI` and table drops instead of
page-cache flushes and epoch bumps. A guest load in compiled code is
then one `ldr`.

Device pages are the part that decides whether this pays. An exit costs
about a microsecond and the boot's device traffic is in the hundred
millions of accesses, so **an MMIO access must never fault**: an
emitted load site that faults on a device page once is patched to the
helper call for good (fault-once site patching), and since drivers reach
their registers from a handful of sites while the rest of the code
never does, the direct path keeps the loads and the helpers keep the
devices. The interpreter has one load site for everything, so it keeps
the software check it has today; the hardware TB is the JIT's gain.

### Phase 4: interrupts and the timer

What the framework offers beyond the MMU: a pending interrupt injected
into the vCPU (`hv_vcpu_set_pending_interrupt`) lands in our IRQ vector
without the dispatch loop polling a flag at every batch boundary, and
the virtual timer can fire the interval timer. Whether either is worth
having is measured after phase 3, on the boot and on the guest's idle
behaviour.
