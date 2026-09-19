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
- **Two assumptions to verify before believing any number.** 256 ASNs times
  four modes needs ten ASID bits: read the width from `ID_AA64MMFR0_EL1`
  rather than assume 16. And the exit cost above is assumed, not measured:
  one measured exit round trip replaces the 1-3 us range with a number.
  Idle belongs with it -- a guest `WFI` must exit so the host thread can
  sleep.

The compiler can stay outside the VM at first: the code cache is shared
memory and compiles are rare, which postpones porting asmjit to the
freestanding runtime.

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
