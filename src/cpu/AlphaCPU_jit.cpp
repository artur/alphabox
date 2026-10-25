/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * Contains code from ES40-Emu:
 * Copyright (C) 2026 gdwnldsKSC and the ES40-Emu contributors
 * Website: https://github.com/ES40-Emu/es40
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

/* AlphaCPU_jit.cpp -- the CPU side of the JIT: the dispatcher that runs
 * compiled blocks, the helpers compiled code calls back into, and the idle
 * pacing that belongs to that dispatcher.
 *
 * Split out of AlphaCPU.cpp, where it was 45% of the file and none of it was
 * about the Alpha. What is left there is the architecture: the interpreter,
 * the translation buffers and address translation, the caches, save and
 * restore. This half is about how the emulator executes, not about what it
 * emulates, and a second processor family inherits it unchanged.
 *
 * The opcode headers stay with execute() in AlphaCPU.cpp on purpose: they are
 * #included into one function so the compiler can keep the interpreter's hot
 * state in registers, and breaking them into translation units would cost
 * real speed. The seams taken here are the ones that are not on that path.
 *
 * $Id$
 */

#include "AlphaCPU.hpp"
#include "StdAfx.hpp"
#ifdef ES40_JIT
#include "jit/jitengine.hpp"
#endif
#include "AliM1543C.hpp"
#include "DebugTarget.hpp"
#include "cpu_debug.hpp"
#include "cpu_memory.hpp"
#include "cpu_misc.hpp"
#include "cpu_pal.hpp"
#include "diag_rpcc.hpp"
#include <algorithm>
#include <cstdlib>
#include <mutex>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h> // _mm_setcsr: pin host MXCSR for the JIT SSE FP path
#else
#include <cfenv> // fesetenv: pin the host FPCR for the JIT FP path
#endif

#ifdef JIT_STATS
// Charge every host cycle between a helper's entry and its return -- callees
// included -- to that helper kind, so the stats can say what share of a
// window the helpers are, not just how often they were called.
struct HelperTimer {
  CJitEngine *e;
  int k;
  uint64_t t0;
  HelperTimer(CJitEngine *e_, int k_) : e(e_), k(k_), t0(jit_rdtsc()) {}
  ~HelperTimer() { e->note_helper_tsc(k, jit_rdtsc() - t0); }
};
#define HELPER_TIMER(cpu, kind) HelperTimer _helper_timer((cpu)->m_jit, kind)
#else
#define HELPER_TIMER(cpu, kind)                                                \
  do {                                                                         \
  } while (0)
#endif

// ASN switch: bump the chain epoch so compiled chain edges revalidate through
// the asn-keyed lookup paths (the chain guard checks tag+epoch only). No-op in
// non-JIT builds.
void CAlphaCPU::jit_note_asn_change() {
#ifdef ES40_JIT
  if (m_jit)
    m_jit->note_itb_invalidate(CJitEngine::EPOCH_ASN);
#endif
}

#ifdef ES40_JIT
void CAlphaCPU::jit_flush_blocks() {
  if (m_jit)
    m_jit->flush();
}

// ASM-bit-clear icache flush (process/ASN switch, ITB_IAP): keep the global PAL
// blocks compiled instead of recompiling them on every such flush (the dominant
// JIT cost in PAL-heavy code).
void CAlphaCPU::jit_flush_blocks_asm() {
  if (m_jit)
    m_jit->flush_non_global();
}

// ALPHABOX_NO_IDLE=1 disables idle pacing; ALPHABOX_IDLESTATS=1 prints its
// counters every 2000 visits to the idle-loop head.
static const bool g_idle_pacing = getenv("ALPHABOX_NO_IDLE") == nullptr;
static const bool g_idle_stats = getenv("ALPHABOX_IDLESTATS") != nullptr;

// The head of Windows NT's idle loop (KiIdleLoop) on Alpha: CALL_PAL enable
// interrupts, CALL_PAL disable interrupts, LDL t0, n(s0) (the PRCB's DPC
// queue), BEQ t0. Matched by instruction words, not address, so any NT kernel
// build is recognized.
// Hand a busy-wait the time it is waiting for instead of spinning through it
// in real time (see the dispatcher below). ALPHABOX_STALL_SKIP=0 turns it off
// and makes the guest wait in real time, as the hardware would.
static const bool g_stall_skip = [] {
  const char *e = getenv("ALPHABOX_STALL_SKIP");
  return !(e && e[0] == '0');
}();

// Windows' KeStallExecutionProcessor, as the Alpha HAL writes it: a driver
// asks to be delayed, and the HAL spins on the cycle counter until the
// cycles have passed.
//
//     rpcc t2              now
//     zap  t2, #240, t2    (the counter is 32 bits)
//     subl t2, t1, t2      elapsed = now - start
//     zap  t2, #240, t2
//     subl t0, t2, t2      remaining = asked - elapsed
//     bgt  t2, -6          ... while there is any left
//
// Matched by its instruction words, which pin the registers too: t0 (r1) is
// what was asked for and t1 (r2) is the counter when the wait began.
static inline bool nt_stall_loop(const char *dram, u64 dram_size, u64 phys) {
  if ((phys & 3) || phys + 24 > dram_size)
    return false;
  u32 w[6];
  memcpy(w, dram + phys, sizeof(w));
  return w[0] == 0x607fc000 && w[1] == 0x487e1603 && w[2] == 0x40620123 &&
         w[3] == 0x487e1603 && w[4] == 0x40230123 && w[5] == 0xfc7ffffa;
}

static inline bool nt_idle_head(const char *dram, u64 dram_size, u64 phys) {
  if ((phys & 3) || phys + 16 > dram_size)
    return false;
  u32 w[4];
  memcpy(w, dram + phys, sizeof(w));
  return w[0] == 0x00000009 && w[1] == 0x00000008 && (w[2] >> 16) == 0xa029 &&
         (w[3] >> 21) == 0x721;
}

// The head of the Windows 2000 Tsunami HAL's loop for a processor that has
// not been started (with Professional's two-processor licence, any CPU past
// the second waits here for good): LDL v0, 32(sp); LDL v0, 28(v0);
// SLL v0, #55, v0; SRL v0, #63, v0 (the start bit); XOR v0, #1, v0; BEQ v0.
// Every pass also calls a PAL routine that does an IC_FLUSH, which makes the
// running CPUs flush their compiled blocks too, so pacing it matters for more
// than the parked CPU's own host core. The BSR displacements that follow vary
// with the HAL build and are not matched.
static inline bool nt_park_head(const char *dram, u64 dram_size, u64 phys) {
  if ((phys & 3) || phys + 24 > dram_size)
    return false;
  u32 w[6];
  memcpy(w, dram + phys, sizeof(w));
  return w[0] == 0xa01e0020 && w[1] == 0xa000001c && w[2] == 0x4806f720 &&
         w[3] == 0x4807f680 && w[4] == 0x44003800 && (w[5] >> 21) == 0x720;
}

// Sleep a CPU that is spinning in the idle loop until an interrupt is raised
// for it (irq_h -> idle_wake: clock ticks, IPIs, devices) or 1 ms passes: the
// loop also polls the DPC queue and NextThread, which another CPU may fill
// without an IPI. CPU0 also wakes by its next interval-timer deadline, which
// its own dispatch batches fire. Guest time is wall-clock based, so sleeping
// only slows the spin.
void CAlphaCPU::jit_idle_pause() {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
  if (state.iProcNum == 0 && next_timer_fire < deadline)
    deadline = next_timer_fire;
  const auto t0 = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lk(m_idle_mx);
  m_idle_sleeping.store(true);
  if (!state.check_int && !state.check_timers && !state.eir)
    m_idle_cv.wait_until(lk, deadline);
  m_idle_sleeping.store(false);
  ++m_idle_sleeps;
  m_idle_slept_ns += (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
}

// Compiling is the one part of the dispatch path that cannot run inside
// the VM: asmjit allocates heavily, and the process allocator's state in
// there is a private copy of the outside's, so a block allocated inside
// and freed outside corrupts it. hv::escape() performs the call on the
// host thread; everything it writes -- the code arena, the block cache,
// the exit records -- is memory the two sides share, so the block is there
// when the VM resumes. Outside the VM this is a direct call.
u64 CAlphaCPU::compile_thunk(void *p) {
  CompileArg *a = (CompileArg *)p;
  CAlphaCPU *c = a->cpu;
  c->m_jit->compile_block(
      (CJitEngine::JitBlock *)a->b, (const uint8_t *)c->dram_ptr, c->dram_size,
      (void *)&CAlphaCPU::jit_read, (void *)&CAlphaCPU::jit_write,
      (void *)&CAlphaCPU::jit_opcdec, (void *)&CAlphaCPU::jit_hw_mfpr,
      (void *)&CAlphaCPU::jit_read_phys, (void *)&CAlphaCPU::jit_hw_mtpr,
      (void *)&CAlphaCPU::jit_write_phys, (void *)&CAlphaCPU::jit_indirect,
      (void *)&CAlphaCPU::jit_read_locked, (void *)&CAlphaCPU::jit_stc,
      (void *)&CAlphaCPU::jit_misc, (void *)&CAlphaCPU::jit_read_vpte,
      (void *)&CAlphaCPU::jit_read_wchk, (void *)&CAlphaCPU::jit_itof,
      (void *)&CAlphaCPU::jit_ftoi, (void *)&CAlphaCPU::jit_fltl,
      (void *)&CAlphaCPU::jit_fp_read, (void *)&CAlphaCPU::jit_fp_write,
      (void *)&CAlphaCPU::jit_fltv);
  return 0;
}

void CAlphaCPU::compile_outside(void *b) {
  CompileArg a{this, b}; // a local: inside the VM that is the vCPU stack,
                         // which the host can read
#ifdef ALPHABOX_HVF
  hv::escape(&compile_thunk, &a);
#else
  compile_thunk(&a);
#endif
  // Compiling may have made a page a code page. Until this processor drops
  // the translations it cached for that page, its compiled code can still
  // store into it inline -- without telling the code-page map, because the
  // whole point of the exclusion is that such a store takes the helper. Do
  // it here, on the spot: we are on this processor's own thread at a
  // dispatch boundary, with no compiled frame live. Leaving it to the
  // deferred request (which the other processors still use) left a window
  // in which an IMB could consume the one write the marking counted and a
  // later store in the same window then went unrecorded -- which is a guest
  // running code that has been overwritten.
  if (m_code_map && m_code_map->code_pages() != m_code_pages_seen)
    honour_new_code_pages();
}

// Post-hang state dump (keypipe token dbg-dump): registers, both banks,
// the code around the PC and the stack, read side-effect free.
void CAlphaCPU::dbg_dump_state() {
  printf("%%DBG-I-DUMP: CPU%d pc=%016llx sde=%d cm=%d asn=%d pal_base=%llx "
         "icount=%llu\n",
         (int)state.iProcNum, (unsigned long long)state.pc, (int)state.sde,
         (int)state.cm, (int)state.asn, (unsigned long long)state.pal_base,
         (unsigned long long)state.instruction_count);
  printf("%%DBG-I-DUMP: exc_addr=%016llx exc_sum=%llx va=%016llx "
         "mm_stat=%llx\n",
         (unsigned long long)state.exc_addr, (unsigned long long)state.exc_sum,
         (unsigned long long)state.fault_va, (unsigned long long)state.mm_stat);
  for (int r = 0; r < 64; r += 4)
    printf("%%DBG-I-DUMP: r%-2d %016llx %016llx %016llx %016llx\n", r,
           (unsigned long long)state.r[r], (unsigned long long)state.r[r + 1],
           (unsigned long long)state.r[r + 2],
           (unsigned long long)state.r[r + 3]);
  // PALcode runs from physical addresses: read the code around a PAL-mode
  // PC without translating it.
  const bool pal_pc = state.pc & 1;
  auto peek_phys = [&](u64 pa, u64 *out) -> bool {
    if (pa + 8 > dram_size)
      return false;
    memcpy(out, (const u8 *)dram_ptr + pa, 8);
    return true;
  };
  auto peek = [&](u64 va, u64 *out) -> bool {
    u64 pa;
    bool asmb;
    if (virt2phys(va, &pa, ACCESS_READ | FAKE | NO_CHECK, &asmb, 0) != 0)
      return false;
    if (pa + 8 > dram_size)
      return false;
    memcpy(out, (const u8 *)dram_ptr + pa, 8);
    return true;
  };
  const u64 pc = state.pc & ~U64(3);
  for (u64 va = pc - 0x100; va < pc + 0x80; va += 8) {
    u64 v;
    if (pal_pc ? peek_phys(va, &v) : peek(va, &v))
      printf("%%DBG-I-CODE: %016llx %08x %08x\n", (unsigned long long)va,
             (unsigned)(v & 0xffffffff), (unsigned)(v >> 32));
  }
  const u64 sp = state.r[30];
  for (u64 va = sp; va < sp + 0x200; va += 8) {
    u64 v;
    if (peek(va, &v))
      printf("%%DBG-I-STACK: %016llx %016llx\n", (unsigned long long)va,
             (unsigned long long)v);
  }
  fflush(stdout);
}

// ALPHABOX_TRACE_ICPROBE=1 (docs/headless.md): three reports, the first 50
// of each. ICPROBE: the dispatcher's icache probe met a line the fetch
// would refuse -- filled before the last flush, or in another mode -- that
// names a physical page other than the live translation. ICPROBE-RECORD: a
// block is recorded at a physical other than the one its instructions were
// just fetched from. ICPROBE-CHAIN: a computed jump chains into code
// compiled from a physical other than the live one. Before the probe checked
// the line's generation, the first led to the second and the third: the
// Windows 2000 warm-restart hang (docs/performance.md, "Not the shadow
// bank"). Now the probe refuses such lines, and the other two reports
// should never appear.
static const bool g_trace_icprobe = getenv("ALPHABOX_TRACE_ICPROBE") != nullptr;
// ALPHABOX_JIT_CHAIN=0: compiled blocks never run on into one another --
// no link is patched and every computed jump returns to the dispatcher -- so
// every block starts from jit_run, the way a JIT_VERIFY build runs them. A
// switch for telling a chaining fault from anything else, not for speed.
static const bool g_jit_chain = [] {
  const char *e = getenv("ALPHABOX_JIT_CHAIN");
  return !(e && strcmp(e, "0") == 0);
}();
void CAlphaCPU::trace_icprobe(int i, u64 va) {
  const auto &l = state.icache[i];
  if (!(l.valid && (l.asn == state.asn || l.asm_bit) &&
        l.address == (va & ICACHE_MATCH_MASK)) ||
      icache_line_hits(i, va))
    return;
  u64 live = 0;
  bool asmb;
  if (virt2phys(va & ~U64(3), &live, ACCESS_EXEC | FAKE, &asmb, 0) != 0)
    return; // not in the ITB: the fetch will tell
  const u64 line_phys = l.p_address + (va & ICACHE_BYTE_MASK);
  static std::atomic<int> n{0};
  if (live != line_phys && n++ < 50)
    printf("[JIT][CPU%d] ICPROBE: pc=%016llx line gen %llu of %llu, line phys "
           "%016llx, live %016llx, icount=%llu\n",
           (int)state.iProcNum, (unsigned long long)va,
           (unsigned long long)l.gen, (unsigned long long)state.icache_gen,
           (unsigned long long)line_phys, (unsigned long long)live,
           (unsigned long long)state.instruction_count);
}

void CAlphaCPU::jit_run(int budget) {
  if (m_jit)
    m_jit->reclaim_if_pending(); // deferred code reclaim, here at a safe point
                                 // (no compiled frame live)
  // A link request from the previous batch's last chain is stale: interrupts
  // or the scheduler may have moved the PC since.
  m_link_from = nullptr;
  if (g_dbg_dump_req.load(std::memory_order_relaxed) != m_dbg_dump_seen) {
    m_dbg_dump_seen = g_dbg_dump_req.load();
    dbg_dump_state();
  }
  // Another CPU flushed its instruction cache (IC_FLUSH / IMB after loading
  // or changing code): drop this CPU's instruction-cache lines, which the
  // interpreter fetches from (g_icache_flush), and its compiled blocks too.
  // Lazy -- unchanged blocks are re-hashed and kept.
  honour_remote_icache_flush();
  if (m_jit) {
    const u64 cf = g_jit_code_flush.load(std::memory_order_relaxed);
    if (cf != m_jit_code_seen) {
      m_jit_code_seen = cf;
      jit_flush_blocks();
    }
  }
  const auto now = now_fast();
  cc_last_sync += ns_to_host_ticks(
      g_diag_excluded_ns); // keep device-diagnostic print stalls out of the
                           // RPCC (diag_rpcc.h)
  g_diag_excluded_ns = 0;
  // Wall-clock RPCC: advance the cycle counter by real elapsed time * cpu_hz
  // so it tracks the configured CPU frequency no matter how fast/bursty the
  // JIT runs (see sync_cc_wallclock; guest RPCC reads sync it too).
  sync_cc_wallclock();

  // Drive the Cchip interval timer once per dispatch batch (CPU0 only), not
  // once per instruction the way the in-execute() poll did.
  interval_timer_poll(now);

  // Instruction-paced interval-tick envelope (timer.max_instr_per_tick; off by
  // default), for every CPU. Not under the firmware/VMS PALcode (base 0x8000,
  // or before PALcode is set up): SRM's speed calibration counts cycles per
  // tick in a tight spin. A CPU that retires the envelope before the
  // wall-clock tick is due is held until the tick lands, so the guest never
  // sees more instructions per tick than configured; RPCC stays wall-clock.
  const u64 pace_period_ns =
      (m_max_instr_per_tick && theAli && state.pal_base &&
       state.pal_base != U64(0x8000))
          ? theAli->get_interval_period_ns()
          : 0; // 0: pacing off, or the guest hasn't programmed the tick yet
  if (pace_period_ns) {
    const u64 ic = state.instruction_count;
    const u32 seq = cSystem->get_tick_seq();
    if (seq != tick_seen_seq) {
      tick_seen_seq = seq;
      tick_last_icount = ic;
    } else if (ic - tick_last_icount >= m_max_instr_per_tick) {
      switch (tick_hold(pace_period_ns)) {
      case TickHold::Ticked: // landed, or CPU0 fires it on re-entry
        return;
      case TickHold::Expired: // secondary with CPU0 late: re-open the window
        tick_last_icount = ic;
        break;
      case TickHold::Doorbell: // interrupt/stop/reset: run this batch
        break;
      }
    }
  }

  // ALPHABOX_INTERP=1, and every run inside the VM: interpret the batch and
  // compile nothing. execute() runs ONE instruction in a build that has the
  // compiler (it is the bail path there; only the interpreter-only build
  // batches internally), so the budget loop belongs here, above the
  // per-batch housekeeping's cost rather than below it.
  if (m_interp_only) {
    if (m_debug_on.load(std::memory_order_relaxed)) {
      m_debug->interp_run(this, budget); // a boundary before every instruction
      return;
    }
    while (budget-- > 0) {
      if (StopThread || state.pc < m_stop_below)
        return;
      execute();
    }
    return;
  }

  while (budget > 0) {
    // The guest debugger (src/debug): while one is attached, park or step
    // here, at a boundary with no compiled frame live, and run no further
    // than the next breakpoint -- interpreted up to it, and compiled only a
    // block that does not hold one. With none attached, this branch is the
    // whole cost.
    int run_max = budget;  // the longest block that may run compiled
    int span_max = budget; // the longest span that may run interpreted
    if (m_debug_on.load(std::memory_order_relaxed)) {
      int span, whole;
      if (!m_debug->boundary(this, &span, &whole))
        return;
      run_max = std::min(budget, whole);
      span_max = std::min(budget, span);
    }
    const u64 start_virt = state.pc;
    const u32 start_asn = (u32)state.asn;
    // The mode the start's fetch was checked in: the block's key (see
    // CJitEngine::key_matches), for the lookup and for the record after the
    // span, which may itself change the mode.
    const u8 start_cm = (u8)state.cm;

    // run_until_below(): an interpreted span ends at the first
    // non-sequential PC and a compiled block leaves at its jump, so the
    // first PC below the floor comes back here before anything there runs.
    // Compiled code could only go on without coming back by a chain into a
    // block below the floor, and the decompressor's jump never forms one:
    // a chain is linked here, by a later turn of this loop, which this
    // return never reaches. (The PALmode bit cannot carry a PC across the
    // 4-aligned floor.)
    if (start_virt < m_stop_below)
      return;

    // PAL reset-vector entry (firmware updater rewrote the image in place, or a
    // restart): drop stale icache lines and compiled blocks first.
    if (start_virt == (state.pal_base | 1)) {
      JIT_FLUSH_CAUSE(EPOCH_PALRST);
      flush_icache();
    }

    // Resolve the block's physical start side-effect-free (FAKE = no fault, no
    // TB fill) so execute() stays the sole I-stream fetcher; covers
    // superpage/KSEG (no TB entry). phys validates a compiled block vs the live
    // translation -- virtual+ASN keying misses remaps.
    u64 start_phys = 0;
    bool start_asm = false;
    bool have_phys = true;
    // The native EV8's PALmode code is interpreted, never recorded or
    // compiled (ev8-native.md, S11): its shadow registers, PALcode forms,
    // CALL_PAL and mode bits are the interpreter's. A block that is not
    // translated is what the cold path below runs that way.
    if ((start_virt & 1) && m_ev8n)
      have_phys = false;
    else if (start_virt & 1) // PALmode: physically addressed, always ASM
    {
      start_phys = start_virt & ~U64(1);
      start_asm = true;
    } else {
      // Fast path: icache hit-probe reads phys straight from the line,
      // read-only (no fill, no fault) -- get_icache's hit geometry minus the
      // side effects. Covers warm code.
      const int ici =
          (int)(((start_virt & ~U64(3)) >> 11) & (ICACHE_ENTRIES - 1));
      if (g_trace_icprobe && icache_enabled)
        trace_icprobe(ici, start_virt);
      if (icache_enabled && icache_line_hits(ici, start_virt)) {
        start_phys =
            state.icache[ici].p_address + (start_virt & ICACHE_BYTE_MASK);
        start_asm = state.icache[ici].asm_bit;
      }
      // Slow path: superpage/KSEG + cold pages (FAKE = no side effect; miss ->
      // interpret).
      else if (virt2phys(start_virt, &start_phys, ACCESS_EXEC | FAKE,
                         &start_asm, 0) != 0)
        have_phys = false;
    }

    // Idle pacing: the CPU keeps coming back to the NT idle-loop head. One pass
    // of Windows 2000's idle loop runs a fixed ~8000-instruction wait between
    // polls (measured with ALPHABOX_IDLESTATS), so visits at most 16000
    // instructions apart, four in a row, with no interrupt pending, mean the
    // CPU is idle ->
    // sleep (jit_idle_pause), then end the batch so the next one re-syncs RPCC
    // and, on CPU0, fires the interval timer. Never sleep twice without running
    // guest code in between (delta 0 right after a pause): the loop must keep
    // polling the DPC queue and NextThread, which another CPU can fill without
    // an IPI.
    if (g_idle_pacing && have_phys && !(start_virt & 1)) {
      if (m_idle_pc == 0 && nt_idle_head(dram_ptr, dram_size, start_phys)) {
        m_idle_pc = start_virt;
        if (m_jit)
          m_jit->note_itb_invalidate(
              CJitEngine::EPOCH_IDLE); // new epoch: drop links into the head
        printf("%%CPU-I-IDLE: CPU%d idle loop recognized at %016llx\n",
               (int)state.iProcNum, (unsigned long long)start_virt);
      }
      if (m_park_pc == 0 && nt_park_head(dram_ptr, dram_size, start_phys)) {
        m_park_pc = start_virt;
        if (m_jit)
          m_jit->note_itb_invalidate(
              CJitEngine::EPOCH_IDLE); // new epoch: drop links into the head
        printf("%%CPU-I-IDLE: CPU%d parked-processor loop recognized at "
               "%016llx\n",
               (int)state.iProcNum, (unsigned long long)start_virt);
      }
      // A CPU is in only one of the two loops at a time; moving from one to the
      // other takes many instructions, which resets the streak.
      if (start_virt == m_idle_pc || start_virt == m_park_pc) {
        const u64 ic = state.instruction_count;
        const u64 delta = ic - m_idle_last_icount;
        if (g_idle_stats && (++m_idle_visits % 2000) == 0)
          printf("%%CPU-I-IDLESTATS: CPU%d visits %llu near %llu zero %llu "
                 "blocked int %llu timers %llu | sleeps %llu slept %.1f ms | "
                 "last delta %llu\n",
                 (int)state.iProcNum, (unsigned long long)m_idle_visits,
                 (unsigned long long)m_idle_near,
                 (unsigned long long)m_idle_zero,
                 (unsigned long long)m_idle_blk_int,
                 (unsigned long long)m_idle_blk_tmr,
                 (unsigned long long)m_idle_sleeps, m_idle_slept_ns / 1e6,
                 (unsigned long long)m_idle_last_delta);
        if (delta) {
          m_idle_last_delta = delta;
          if (delta <= 16000)
            ++m_idle_near;
          m_idle_streak = (delta <= 16000) ? m_idle_streak + 1 : 0;
          m_idle_last_icount = ic;
          if (m_idle_streak >= 4) {
            // An external interrupt already raised counts as pending even
            // while masked: NT's idle loop starts "ei; di" -- interrupts are
            // enabled for one instruction at its head, which is where this
            // runs, with the previous turn's di still in force. A device
            // interrupt that fired then was found masked and its doorbell
            // consumed; sleeping now held it until the next timer tick,
            // ~1.5 ms per interrupt (nada's EHCI driver: every USB transfer).
            if (state.check_int || state.eir)
              ++m_idle_blk_int;
            else if (state.check_timers)
              ++m_idle_blk_tmr;
            else {
              jit_idle_pause();
              return;
            }
          }
        } else {
          ++m_idle_zero;
        }
      }
    }

    // A driver asking to be delayed is asking THIS emulator's cycle counter
    // to advance, and spinning on it costs more than half the wall clock of
    // a Windows 2000 boot. When the processor is in that loop with nothing
    // else pending, give it the cycles it is waiting for and let the loop
    // fall out on its next turn. The guest's counter then runs ahead of real
    // time by what it would have spent waiting, which is the point: it asked
    // for a delay, not for the host to be busy.
    if (g_stall_skip && have_phys && !(start_virt & 1)) {
      if (m_stall_pc == 0 && nt_stall_loop(dram_ptr, dram_size, start_phys)) {
        m_stall_pc = start_virt;
        // Compiled, this loop chains to itself and spins a whole dispatch
        // batch before coming back here -- which is most of the time it was
        // meant to save. Interpreted, every turn of it passes through the
        // dispatcher, and the first one ends the wait.
        if (m_jit)
          m_jit->drop_block(start_virt);
        printf("%%CPU-I-STALL: CPU%d processor-stall loop recognized at "
               "%016llx\n",
               (int)state.iProcNum, (unsigned long long)start_virt);
      }
      if (start_virt == m_stall_pc && !state.check_int && !state.check_timers) {
        const u64 cc_now = rpcc_read(); // syncs the counter to the wall first
        const u32 elapsed = (u32)cc_now - (u32)state.r[2];
        const s32 left = (s32)((u32)state.r[1] - elapsed);
        // A wait longer than the cap is not a delay, it is a guest waiting
        // for something to happen; let real time carry it.
        const u64 cap = cpu_hz / 10;
        if (left > 0 && (u64)left <= cap) {
          state.cc += (u64)left;
          ++m_stall_skips;
          m_stall_cycles += (u64)left;
        } else if (left > 0) {
          ++m_stall_capped;
        }
      }
    }

    // Hot path: virtual+ASN lookup, phys-validated (skipped on a translation
    // miss).
    CJitEngine::JitBlock *b =
        have_phys ? m_jit->lookup(start_virt, start_asn, start_cm) : nullptr;
    if (have_phys && !b) // lazy-flushed survivor? hash-revalidate in place (no
                         // interpreted pass)
      b = m_jit->revalidate_flushed(start_virt, start_asn, start_cm, start_phys,
                                    (const uint8_t *)dram_ptr);

    // A valid block whose phys no longer matches = a page remap the virtual key
    // can't see. The cold path below re-records it; log it -- it's the smoking
    // gun for stale-chain bugs.
    if (b && b->code && b->phys != start_phys) {
      static std::atomic<int> n_stale{0};
      if (n_stale++ < 20)
        printf("[JIT][CPU%d] DISPATCH STALE: pc=%016llx block_phys=%016llx "
               "live_phys=%016llx vgen=%llu epoch=%llu icount=%llu\n",
               (int)state.iProcNum, (unsigned long long)start_virt,
               (unsigned long long)b->phys, (unsigned long long)start_phys,
               (unsigned long long)b->vgen, (unsigned long long)m_jit->vgen(),
               (unsigned long long)state.instruction_count);
    }

#ifdef JIT_STATS
    int cold_reason = CJitEngine::CR_NO_BLOCK; // why we end up interpreting
#endif
    // Run the compiled safe prefix natively when available -- but not while an
    // interrupt or delayed timer is pending. Compiled blocks don't run the
    // per-instruction polls, so run the interpreter.
    if (b && b->code && b->phys == start_phys &&
        (int)b->prefix_len <= run_max && !state.check_int &&
        !state.check_timers &&
        (!(b->tag & 1) ||
         state.sde)) // PALmode block: its shadow-register remap assumes SDE
    {
      b->vgen = m_jit->vgen(); // phys validated + lookup proved flush-fresh:
                               // refresh the chain epoch
#ifdef JIT_VERIFY
      // Interpret the prefix (authoritative), recording each loaded value so
      // the compiled pass can replay it instead of re-reading memory. Skip the
      // compare if an interrupt/trap diverts us mid-prefix.
      u64 snap[64]; // 64: capture the PALshadow bank (r32..r63) for PALmode
                    // blocks
      memcpy(snap, state.r, sizeof(snap));
      u64 f_pre[64]; // FP file before the interp pass -- restored before the
                     // compiled pass below
      memcpy(f_pre, state.f, sizeof(f_pre)); // so the compiled FP ops read the
                                             // same f[] the interp did
      // I_CTL/CM/SIRR + PCTX fields (HW_MFPR 0x40-7f reads
      // astrr/aster/fpen/ppcen) are read-modify-written; a HW_MFPR reads, a
      // later HW_MTPR writes, so the interp pass writes before the compiled
      // pass's HW_MFPR reads. Snapshot the read-back fields here and restore
      // them before b->code() below -- like snap/f_pre do for GPRs/FP.
      const bool ictl_sde_pre = state.sde, ictl_hwe_pre = state.hwe;
      const int ictl_spe_pre = state.i_ctl_spe,
                ictl_vam_pre = state.i_ctl_va_mode;
      const u64 ictl_vptb_pre = state.i_ctl_vptb,
                ictl_other_pre = state.i_ctl_other;
      const int cm_pre = state.cm,
                sir_pre = state.sir; // CM/SIRR read-back (HW_MFPR CM/SIRR)
      const int aster_pre = state.aster,
                astrr_pre = state.astrr; // AST/FPEN/PPCEN read-back via the
      const int fpen_pre = state.fpen,
                ppcen_pre = state.ppcen; // PCTX group (HW_MFPR 0x40-7f)
      // FPCR and EXC_SUM the same way: a block that saves the FPCR (MF_FPCR,
      // STT) and then loads another (LDT, MT_FPCR) -- a context switch --
      // must not have its compiled MF_FPCR read the FPCR the interp pass's
      // MT_FPCR left behind.
      const u64 fpcr_pre = state.fpcr, exc_sum_pre = state.exc_sum;
      const u32 *vw = (const u32 *)((const u8 *)dram_ptr + b->phys);
      u32 vn = 0; // loads recorded for replay
      u32 sn = 0; // stores recorded for the compiled-pass compare
      u64 vpc = start_virt;
      const u32 cur_len = b->prefix_len;
      const u64 cur_tag = b->tag;
      bool clean = true;
      // Loads and stores recorded before each instruction: what a compiled
      // pass that bails there must have consumed and made (the bail compare
      // below).
      u8 vn_at[64], sn_at[64];
      for (u32 k = 0; k < cur_len; ++k) {
        vn_at[k] = (u8)vn;
        sn_at[k] = (u8)sn;
        // Compute the load's effective address from the live registers BEFORE
        // executing it (Rb may be the load's own dest), to compare against
        // the JIT.
        const u32 ins = vw[k];
        const u32 opc = ins >> 26;
        const int lra = (ins >> 21) & 0x1F;
        // HW_LD physical (0x1b, func 0/1) is a load too: the compiled form
        // replays through this same vlog, but its address is physical
        // (untranslated) with a 12-bit disp. Func 5 (quad VPTE) too -- its
        // logged va is virtual, jit_read_vpte's replay key.
        const bool is_hwld = // forms 0,1,4,5,8,9,10,12,13 (compiled HW_LD)
            (opc == 0x1b) && ((U64(0x373b) >> ((ins >> 12) & 0xf)) & 1);
        // RPCC/RC/RS (MISC 0x18) and ISUM (HW_MFPR 0x19 fn 0x0d) read CPU
        // state the verify can't re-derive; the compiled forms pull their
        // value from this same load log (jit_misc / jit_hw_mfpr replay it),
        // so log them like loads.
        const u32 miscfn = (ins & 0xFFFF);
        // Compiled misc reads: RC/RS (0xE000/0xF000) for all Ra incl. 31 (the
        // flag-only side-effect forms compile too); RPCC (0xC000) only when
        // it has a GPR dest (Ra!=31).
        const bool is_miscrd =
            (opc == 0x18) && (miscfn == 0xE000 || miscfn == 0xF000 ||
                              (miscfn == 0xC000 && lra != 31));
        const bool is_isum =
            (opc == 0x19) &&
            (((ins >> 8) & 0xff) == 0x0d); // ISUM: async interrupt-summary
        const bool is_fpld = (opc == 0x22 || opc == 0x23 || opc == 0x20 ||
                              opc == 0x21); // LDS/LDT/LDF/LDG: dest is f[lra]
        // Loads/ISUM/LDS/LDT only log when they have a dest (lra!=31); the
        // misc reads are logged regardless -- a compiled RC/RS Ra==31 still
        // consumes a replay slot, so keep the index in sync.
        const bool isld =
            ((opc == 0x28 || opc == 0x29 || opc == 0x0a || opc == 0x0c ||
              opc == 0x2a || opc == 0x2b || opc == 0x0b || is_hwld || is_isum ||
              is_fpld) &&
             lra != 31) ||
            is_miscrd; // +LDBU/LDWU +LDx_L +LDQ_U +RPCC/RC/RS +ISUM +LDS/LDT
        u64 eva = 0;
        if (isld && !is_miscrd && !is_isum) // misc/ISUM reads have no effective
                                            // address -- only a logged value
        {
          const int lrb = (ins >> 16) & 0x1F;
          const int ldisp =
              is_hwld ? (int)((int32_t)(ins << 20) >> 20) // HW_LD: 12-bit
                      : (int)(int16_t)(ins & 0xFFFF);     // LDx:   16-bit
          eva = (lrb == 31 ? (u64)0 : state.r[RREG(lrb)]) + (u64)ldisp;
          if (opc == 0x0b)
            eva &= ~U64(7); // LDQ_U: address forced to 8-byte alignment
        }
        // Stores touch memory, not GPRs, so record (addr,value) for the
        // compiled-pass compare. Ra (lra) is the value source; Rb is the
        // base. HW_ST physical (0x1f func 0/1) stores too, with a physical
        // (untranslated) address and a 12-bit disp.
        const bool is_sc =
            (opc == 0x2e ||
             opc == 0x2f);   // STL_C/STQ_C: store-conditional (success in Ra)
        const bool is_hwst = // forms 0,1,4,5,12,13 (compiled HW_ST)
            (opc == 0x1f) && ((U64(0x3033) >> ((ins >> 12) & 0xf)) & 1);
        const bool is_fpst =
            (opc == 0x26 || opc == 0x27 || opc == 0x24 ||
             opc == 0x25); // STS/STT/STF/STG: value source is f[lra]
        const bool isst = (opc == 0x2c || opc == 0x2d || opc == 0x0d ||
                           opc == 0x0e || opc == 0x0f || is_sc || is_hwst ||
                           is_fpst); // +STx_C +STQ_U +STS/STT
        u64 sva = 0, sval = 0;
        if (isst) {
          const int srb = (ins >> 16) & 0x1F;
          const int sdisp =
              is_hwst ? (int)((int32_t)(ins << 20) >> 20) // HW_ST: 12-bit
                      : (int)(int16_t)(ins & 0xFFFF);     // STx:   16-bit
          sva = (srb == 31 ? (u64)0 : state.r[RREG(srb)]) + (u64)sdisp;
          if (opc == 0x0f)
            sva &= ~U64(7); // STQ_U: address forced to 8-byte alignment
          if (is_fpst)
            sval = (opc == 0x27)   ? state.f[lra]
                   : (opc == 0x26) ? (u64)ieee_sts(state.f[lra])
                   : (opc == 0x24) ? (u64)vax_stf(state.f[lra])
                                   : vax_stg(state.f[lra]); // STT/STS/STF/STG
          else
            sval = (lra == 31 ? (u64)0 : state.r[RREG(lra)]);
        }
        // Computed jump (JMP/JSR/RET): target = Rb & ~3, taken before
        // execute() (the jump's target uses the old Rb, even if Ra==Rb gets
        // the return address after).
        u64 jtgt = 0;
        if (opc == 0x1a ||
            opc == 0x1e) // JMP/JSR/RET (Rb & ~3) or HW_RET/HWREI (Rb & ~2)
        {
          const int jrb = (ins >> 16) & 0x1F;
          const u64 jmask = (opc == 0x1e) ? ~U64(2) : ~U64(3);
          jtgt = (jrb == 31 ? (u64)0 : state.r[RREG(jrb)]) & jmask;
          if (opc == 0x1a)
            jtgt |= cur_tag & 3; // DO_JMP: mode bits come from the current pc
        }
        execute();
        --budget;
        vpc += 4;
        if (state.pc != vpc) {
          // The terminator (a compiled branch at the last index) diverges to
          // its target -- expected. But execute() services an async
          // interrupt/trap at the TOP, before the instruction runs, so the
          // interpreter can divert to a PAL handler at the terminator WITHOUT
          // executing the branch. Accept the divergence only when state.pc is
          // the branch's actual target; otherwise it's an interrupt/trap and
          // the compiled block (which doesn't model interrupts -- the
          // dispatcher's !check_int guard handles that) legitimately differs,
          // so skip the compare instead of flagging a false mismatch.
          bool ok_branch = false;
          if (k == cur_len - 1 &&
              (opc == 0x30 || opc == 0x34 || (opc >= 0x38 && opc <= 0x3f))) {
            const int64_t bdisp =
                (int64_t)((uint64_t)(ins & 0x1FFFFF) << 43) >> 43;
            const u64 tgt =
                vpc + (u64)(bdisp * 4); // vpc == branch_pc + 4 (fall-through)
            ok_branch = (state.pc == tgt);
          } else if (k == cur_len - 1 && (opc == 0x1a || opc == 0x1e)) {
            ok_branch = (state.pc == jtgt); // computed jump (JMP/HW_RET)
                                            // reached its register target
          } else if (k == cur_len - 1 && opc == 0x00) {
            // CALL_PAL vectored to its PALcode entry (pal_base | offset); the
            // kernel-mode path never traps, but accept the OPCDEC vector too.
            const u32 func = ins & 0x1FFFFFFF;
            const u64 voff = (u64)0x2000 | ((u64)(func & 0x80) << 5) |
                             ((u64)(func & 0x3f) << 6) | U64(1);
            ok_branch = (state.pc == (state.pal_base | voff)) ||
                        (state.pc == (state.pal_base | OPCDEC | U64(1)));
          }
          if (!ok_branch)
            clean = false;
          break;
        }
        if (isld && vn < 64) {
          m_jit_vaddr[vn] = eva;
          m_jit_vlog[vn] =
              is_fpld ? state.f[lra]
                      : state.r[RREG(
                            lra)]; // FP dest -> f[]; shadow-aware GPR otherwise
          vn++;
        }
        if (isst && sn < 64) {
          m_jit_slog_addr[sn] = sva;
          m_jit_slog_val[sn] = sval;
          m_jit_slog_success[sn] =
              is_sc ? state.r[RREG(lra)]
                    : (u64)1; // STx_C result (post-execute); ordinary store = 1
          sn++;
        }
      }
      if (clean) {
        // HW_MTPR verify: a compiled block writes IPR fields directly in LIVE
        // state (its GPR writes go to jr scratch). Snapshot the writable IPR
        // set after the interp pass (authoritative); below we compare the
        // compiled pass's IPR writes and roll the live fields back. Mostly pure
        // stores; I_CTL is read-modify-written, so it's also reset pre-pass
        // (ictl_*_pre, above). Keep this list in sync with jit_hw_mtpr.
        auto cap_iprs = [&](u64 *d) {
          d[0] = state.last_tb_virt;
          d[1] = last_dtb_virt[0];
          d[2] = last_dtb_virt[1];
          d[3] = state.pctr_ctl;
          d[4] = state.dc_ctl;
          d[5] = (u64)state.cc_offset;
          d[6] = (u64)(u32)state.alt_cm;
          // IER enables; check_int not snapshotted (rolling it back could
          // suppress a poll)
          d[7] = (u64)(u32)state.asten;
          d[8] = (u64)(u32)state.sien;
          d[9] = (u64)(u32)state.pcen;
          d[10] = (u64)(u32)state.cren;
          d[11] = (u64)(u32)state.slen;
          d[12] = (u64)(u32)state.eien;
          d[13] =
              state.exc_sum;  // FPSTART clears it (compiled ITOFx/FTOIx/FLTL)
          d[14] = state.fpcr; // MT_FPCR (compiled FLTL) writes it
          d[15] = (u64)state.sde;
          d[16] = (u64)state.hwe; // I_CTL (compiled as terminator)
          d[17] = (u64)(u32)state.i_ctl_spe;
          d[18] = (u64)(u32)state.i_ctl_va_mode;
          d[19] = state.i_ctl_vptb;
          d[20] = state.i_ctl_other;
          d[21] = (u64)(u32)state.cm;
          d[22] = (u64)(u32)state.sir; // CM, SIRR
          d[23] = (u64)(u32)state.aster;
          d[24] = (u64)(u32)state.astrr; // 0x40-group ASTER/ASTRR
          d[25] = (u64)(u32)state.fpen;
          d[26] = (u64)(u32)state.ppcen; // 0x40-group FPEN/PPCEN
        };
        auto put_iprs = [&](const u64 *s) {
          state.last_tb_virt = s[0];
          last_dtb_virt[0] = s[1];
          last_dtb_virt[1] = s[2];
          state.pctr_ctl = s[3];
          state.dc_ctl = s[4];
          state.cc_offset = (u32)s[5];
          state.alt_cm = (int)s[6];
          state.asten = (int)s[7];
          state.sien = (int)s[8];
          state.pcen = (int)s[9];
          state.cren = (int)s[10];
          state.slen = (int)s[11];
          state.eien = (int)s[12];
          state.exc_sum = s[13];
          state.fpcr = s[14];
          state.sde = (bool)s[15];
          state.hwe = (bool)s[16];
          state.i_ctl_spe = (int)s[17];
          state.i_ctl_va_mode = (int)s[18];
          state.i_ctl_vptb = s[19];
          state.i_ctl_other = s[20];
          state.cm = (int)s[21];
          dpc_context_changed();
          state.sir = (int)s[22];
          state.aster = (int)s[23];
          state.astrr = (int)s[24];
          state.fpen = (int)s[25];
          state.ppcen = (int)s[26];
        };
        u64 ipr_interp[27];
        cap_iprs(ipr_interp);
        // FP file: compiled ITOFx writes state.f live (like the IPR writes);
        // snapshot the interp pass's result, compare after the compiled pass,
        // then restore.
        u64 f_interp[64];
        memcpy(f_interp, state.f, sizeof(f_interp));
        // The GPRs the same way: compiled code on AArch64 addresses them from
        // the cpu pointer, so it must run on state.r itself (64 entries: a
        // compiled PALmode block may touch the shadow bank).
        u64 r_interp[64];
        memcpy(r_interp, state.r, sizeof(r_interp));
        memcpy(state.r, snap, sizeof(r_interp));
        memcpy(state.f, f_pre,
               sizeof(f_pre)); // restore the FP file like the GPRs: the
                               // compiled pass reads pre-interp f
        state.sde = ictl_sde_pre;
        state.hwe = ictl_hwe_pre; // restore I_CTL too: a compiled HW_MFPR must
                                  // read the pre-interp value
        state.i_ctl_spe = ictl_spe_pre;
        state.i_ctl_va_mode = ictl_vam_pre;
        state.i_ctl_vptb = ictl_vptb_pre;
        state.i_ctl_other = ictl_other_pre;
        state.cm = cm_pre;
        dpc_context_changed();
        state.sir = sir_pre; // ...and CM/SIRR
        state.aster = aster_pre;
        state.astrr = astrr_pre; // ...and the PCTX read-backs (a
        state.fpen = fpen_pre;
        state.ppcen = ppcen_pre; // HW_MFPR PCTX reads these live)
        state.fpcr = fpcr_pre;
        state.exc_sum = exc_sum_pre;
        const u64 interp_pc =
            state.pc; // interpreter is authoritative for the PC
        const u32 n_stores_interp =
            sn; // interp's RECORDED store count (sn), NOT the replay cursor
                // m_jit_slog_i; a compiled pass must consume exactly this many
                // (catches a missing/extra store)
        // The same for loads: a compiled pass that reads more or fewer
        // values than the interpreter loaded would replay the wrong ones.
        // (Only while the log holds them all: past 64 it stops recording.)
        const u32 n_loads_interp = vn;
#if defined(__aarch64__)
        // Sample as production does, so the pin sets adaptation picks are
        // the ones verified.
        m_jit->pin_sample(b, (const uint8_t *)dram_ptr);
#endif
        m_jit_vreplay = true;
        m_jit_vlog_i = 0;
        m_jit_slog_i = 0;
        m_jit_vbad = false;
        const u32 done =
            b->code(this, state.r); // also writes state.pc (the JIT's next PC)
        m_jit_vreplay = false;
        if (done == b->prefix_len) {
          if (state.pc != interp_pc) {
            m_jit_vbad = true;
            // Dump the JIT's source (DRAM at b->phys, vw[]) vs the icache (what
            // the interpreter actually fetches), word by word. If the middle
            // words differ the icache holds a stale/different version the JIT
            // never compiled.
            const int icl = (int)((start_virt >> 11) & (ICACHE_ENTRIES - 1));
            const u32 wb = (u32)((start_virt >> 2) & ICACHE_INDEX_MASK);
            printf("[JIT][VERIFY] PC MISMATCH at %016llx: interp=%016llx "
                   "jit=%016llx plen=%u\n",
                   (unsigned long long)start_virt,
                   (unsigned long long)interp_pc, (unsigned long long)state.pc,
                   b->prefix_len);
            printf("   dram:");
            for (u32 w = 0; w < b->prefix_len && w < 16; ++w)
              printf(" %08x", vw[w]);
            printf("\n   icch:");
            for (u32 w = 0; w < b->prefix_len && w < 16; ++w)
              printf(" %08x",
                     endian_32(
                         state.icache[icl].data[(wb + w) & ICACHE_INDEX_MASK]));
            printf("\n   r1 i=%016llx j=%016llx  r2 i=%016llx j=%016llx  r5 "
                   "snap=%016llx\n",
                   (unsigned long long)r_interp[1],
                   (unsigned long long)state.r[1],
                   (unsigned long long)r_interp[2],
                   (unsigned long long)state.r[2], (unsigned long long)snap[5]);
          }
          u64 ipr_jit[27];
          cap_iprs(ipr_jit); // compiled pass wrote IPRs into live state; check
                             // vs interp
          for (int ii = 0; ii < 27; ii++)
            if (ipr_jit[ii] != ipr_interp[ii] && (m_jit_vbad = true))
              printf("[JIT][VERIFY] IPR MISMATCH at %016llx slot %d: "
                     "interp=%016llx jit=%016llx\n",
                     (unsigned long long)start_virt, ii,
                     (unsigned long long)ipr_interp[ii],
                     (unsigned long long)ipr_jit[ii]);
          for (int fi = 0; fi < 64; fi++)
            if (state.f[fi] != f_interp[fi] && (m_jit_vbad = true))
              printf("[JIT][VERIFY] FP MISMATCH at %016llx f%d: interp=%016llx "
                     "jit=%016llx\n",
                     (unsigned long long)start_virt, fi,
                     (unsigned long long)f_interp[fi],
                     (unsigned long long)state.f[fi]);
          if (m_jit_slog_i != n_stores_interp && (m_jit_vbad = true))
            printf("[JIT][VERIFY] STORE COUNT MISMATCH at %016llx: interp=%u "
                   "jit=%u\n",
                   (unsigned long long)start_virt, n_stores_interp,
                   m_jit_slog_i);
          if (n_loads_interp < 64 && m_jit_vlog_i != n_loads_interp &&
              (m_jit_vbad = true))
            printf("[JIT][VERIFY] LOAD COUNT MISMATCH at %016llx: interp=%u "
                   "jit=%u\n",
                   (unsigned long long)start_virt, n_loads_interp,
                   m_jit_vlog_i);
          cc_last_sync += ns_to_host_ticks(
              m_jit->verify_compare(start_virt, r_interp, state.r, vw,
                                    b->prefix_len, m_jit_vbad) +
              g_diag_excluded_ns); // don't bill the verify progress-print OR
                                   // the PCI decode-off diag stall to the RPCC
          g_diag_excluded_ns =
              0; // consumed here in the verify path so jit_run's later sync
                 // doesn't double-count it
        } else if (done < b->prefix_len) {
          // The compiled pass bailed at instruction `done` (a device access,
          // or anything else a helper hands back) while the interpreter ran
          // the whole block. The bail protocol: the PC is that instruction's,
          // the loads consumed and the stores made are the interpreter's
          // before it, and nothing of it or after it was done -- so every GPR
          // the rest of the block does not write is the interpreter's. (A
          // CALL_PAL in the rest, whose native routine may write any
          // register, leaves the GPRs out.)
          const u64 bail_pc = b->tag + 4 * (u64)done;
          if (state.pc != bail_pc && (m_jit_vbad = true))
            printf("[JIT][VERIFY] BAIL PC MISMATCH at %016llx: bailed at %u, "
                   "pc=%016llx, expected %016llx\n",
                   (unsigned long long)start_virt, done,
                   (unsigned long long)state.pc, (unsigned long long)bail_pc);
          if (m_jit_slog_i != sn_at[done] && (m_jit_vbad = true))
            printf("[JIT][VERIFY] BAIL STORE COUNT MISMATCH at %016llx: "
                   "bailed at %u, interp=%u jit=%u\n",
                   (unsigned long long)start_virt, done, (unsigned)sn_at[done],
                   m_jit_slog_i);
          if (n_loads_interp < 64 && m_jit_vlog_i != vn_at[done] &&
              (m_jit_vbad = true))
            printf("[JIT][VERIFY] BAIL LOAD COUNT MISMATCH at %016llx: "
                   "bailed at %u, interp=%u jit=%u\n",
                   (unsigned long long)start_virt, done, (unsigned)vn_at[done],
                   m_jit_vlog_i);
          u64 r_cmp[64];
          memcpy(r_cmp, r_interp, sizeof(r_cmp));
          for (u32 k = done; k < b->prefix_len; ++k) {
            const u32 w = vw[k], op = w >> 26;
            int d = -1;
            if (op == 0x00) { // CALL_PAL: no GPR compare
              memcpy(r_cmp, state.r, sizeof(r_cmp));
              break;
            }
            if ((op >= 0x10 && op <= 0x13) || op == 0x1c)
              d = (int)(w & 31); // operates, FTOIx: Rc
            else if (op == 0x08 || op == 0x09 || op == 0x0a || op == 0x0b ||
                     op == 0x0c || (op >= 0x28 && op <= 0x2b) || op == 0x2e ||
                     op == 0x2f || op == 0x30 || op == 0x34 || op == 0x1a ||
                     op == 0x18 || op == 0x19 || op == 0x1b || op == 0x1f)
              d = (int)((w >> 21) & 31); // loads, STx_C, BR/BSR, JMP, MISC,
                                         // HW_MFPR, HW_LD, HW_ST/C: Ra
            if (d < 0 || d == 31)
              continue;
            r_cmp[d] = state.r[d];
            if (d < 24 && (d & 0xc) == 0x4) // and its PALshadow copy
              r_cmp[d + 32] = state.r[d + 32];
          }
          cc_last_sync +=
              ns_to_host_ticks(m_jit->verify_compare(start_virt, r_cmp, state.r,
                                                     vw, done + 1, m_jit_vbad) +
                               g_diag_excluded_ns);
          g_diag_excluded_ns = 0;
        }
        state.pc =
            interp_pc; // restore; the block's PC write was only for the check
        put_iprs(ipr_interp); // roll back the compiled pass's live IPR writes
                              // (verify-only)
        memcpy(state.f, f_interp, sizeof(f_interp)); // ...and its FP writes
        memcpy(state.r, r_interp, sizeof(r_interp)); // ...and its GPR writes
        break_seq_icache(); // compiled pass + raw pc restore bypassed set_pc
      }
      continue;
#else
      // A predecessor block's epilogue cache-missed and asked to be linked
      // here. Now that we know this block is live and compiled, patch its
      // successor pointer so it jumps straight in instead of returning. Never
      // link into a learned idle/parked loop head: idle pacing (above) only
      // sees passes that come back through here. Nor into the PAL reset
      // vector, whose flush (above) runs only for an entry through here;
      // jit_indirect refuses it for computed jumps, and this for the static
      // exits (a PC-relative branch in PALcode to PAL_BASE).
      if (m_link_from &&
          (!g_jit_chain || start_virt == m_idle_pc || start_virt == m_park_pc ||
           start_virt == (state.pal_base | 1))) {
        m_jit->note_link_bail();
        m_link_from = nullptr;
      }
      if (m_link_from) {
        // Low bits (JitBlock is 8-aligned): 0 = scan-style exit, round-robin
        // into the slots; k = a static exit that owns slot k-1.
        const uintptr_t lraw = (uintptr_t)m_link_from;
        const unsigned exact = (unsigned)(lraw & 7);
        m_jit->note_link_bail();
        if (exact && exact <= (unsigned)CJitEngine::kLinkSlots) {
          // A static exit's data link: the tagged pointer is that code's
          // ExitRec. Cache only when b really is the exit's target (the
          // request may predate an interrupt that moved the PC); b was just
          // validated in this epoch (vgen stamped above) and is about to run.
          CJitEngine::ExitRec *xr =
              (CJitEngine::ExitRec *)(lraw & ~(uintptr_t)7);
          const unsigned k = exact - 1;
          // A static exit's slot always holds the SAME compile-time target, so
          // a body already in it means the epoch compare -- not a new target --
          // is what sent us here.
          m_jit->note_dlink_stale(xr->body[k] != nullptr);
          if (b->tag == m_link_target && b->jit_body) {
            xr->body[k] = b->jit_body;
            xr->epoch[k] = m_jit->vgen();
          } else {
            xr->body[k] = nullptr;
            xr->epoch[k] = ~(uint64_t)0;
          }
        } else {
          CJitEngine::JitBlock *lf =
              (CJitEngine::JitBlock *)(lraw & ~(uintptr_t)7);
          m_jit->note_link_edge(lf, b->tag);
          bool in = false; // poly-link: cache b in the source's successor slots
          for (int i = 0; i < CJitEngine::kLinkSlots; ++i)
            if (lf->link[i] == b)
              in = true; // skip if already cached (it just went stale)
          m_jit->note_link_stale(in);
          if (!in) {
            for (int i = CJitEngine::kLinkSlots - 1; i > 0; --i)
              lf->link[i] = lf->link[i - 1];
            lf->link[0] = b;
          } // else round-robin insert
        }
        m_link_from = nullptr;
      }
      m_jit_budget =
          budget; // ceiling for compiled chains (epilogue stops at it)
#if defined(__aarch64__)
      // Where the guest spends its time, for the pin set (one dispatch in
      // kPinEvery; most arrive from a chain's budget or interrupt exit).
      m_jit->pin_sample(b, (const uint8_t *)dram_ptr);
#endif
#ifdef JIT_STATS
      const uint64_t _comp_t0 = jit_rdtsc();
#endif
      const u32 done = b->code(this, &state.r[0]);
#ifdef JIT_STATS
      const uint64_t _comp_tsc =
          jit_rdtsc() - _comp_t0; // host cycles in this compiled chain
#endif
      state.r[31] = 0;
      break_seq_icache(); // compiled block wrote pc natively (no set_pc); drop
                          // the stale cursor
      // state.pc is written by the compiled block itself (next PC, or the bail
      // PC). Account for the compiled ops' instruction count. state.cc (RPCC)
      // is not advanced per instruction but set from wall-clock * cpu_hz at
      // the jit_run boundary above, so a hot compiled loop can't run the
      // cycle counter ahead of real time
      state.instruction_count += done;
      budget -= done;
#ifdef JIT_STATS
      m_jit->note_hot_pc(start_virt, start_phys, done, (const u8 *)dram_ptr);
      cc_last_sync += ns_to_host_ticks(m_jit->note_exec(
          done, 0, _comp_tsc,
          0)); // don't bill the stats-print stall to the wall-clock RPCC
#endif
      if (done > 0)
        continue; // progress made; done==0 (faulting first insn) falls through
#ifdef JIT_STATS
      cold_reason = CJitEngine::CR_DONE0;
#endif
#endif
    }
#ifdef JIT_STATS
    else
      cold_reason = !have_phys ? CJitEngine::CR_NO_PHYS
                    : !b       ? CJitEngine::CR_NO_BLOCK
                    : !b->code ? (b->compiled ? CJitEngine::CR_UNCOMPILABLE
                                              : CJitEngine::CR_NOT_HOT)
                    : b->phys != start_phys ? CJitEngine::CR_STALE
                    : state.check_int ? ((b->tag & 1) ? CJitEngine::CR_INT_PAL
                                                      : CJitEngine::CR_INT)
                    : state.check_timers           ? CJitEngine::CR_TIMER
                    : ((b->tag & 1) && !state.sde) ? CJitEngine::CR_PAL_NOSDE
                                                   : CJitEngine::CR_BUDGET;
    const u32 cold_first_op =
        (have_phys && start_phys + 4 <= dram_size)
            ? (((const u32 *)((const u8 *)dram_ptr + start_phys))[0] >> 26)
            : 0;
#endif

    // Miss path (cold): the up-front translation gave start_phys/start_asm
    // (when have_phys). We're not running a compiled block here, so drop any
    // pending link request, interpret the block, record it, and compile its
    // prefix. Count the link-miss bail before dropping it.
    if (m_link_from)
      m_jit->note_link_bail();
    m_link_from = nullptr;
    u32 n = 0;
    u64 expected = start_virt;
#ifdef JIT_STATS
    const uint64_t _interp_t0 = jit_rdtsc();
#endif
    for (int left = span_max; left > 0; --left) {
      execute();
      ++n;
      expected += 4;
      if (state.pc != expected)
        break;
    }
    budget -= (int)n;
#ifdef JIT_STATS
    cc_last_sync += ns_to_host_ticks(m_jit->note_exec(
        0, n, 0, jit_rdtsc() - _interp_t0)); // don't bill the stats-print stall
                                             // to the wall-clock RPCC
    m_jit->note_cold(cold_reason, n, cold_first_op);
#endif
    // The span just interpreted came from icache lines whose bytes no longer
    // match RAM (an in-place rewrite the icache hasn't seen): don't record a
    // block for it, or it would be hashed against the new bytes.
    bool src_stale = false;
    if (icache_enabled && have_phys) {
      const u64 vs = start_virt & ~U64(3);
      const u64 ve = vs + 4 * (u64)n;
      for (u64 v = vs & ~U64(0x7ff); v < ve && !src_stale; v += 0x800) {
        const int li = (int)((v >> 11) & (ICACHE_ENTRIES - 1));
        const auto &line = state.icache[li];
        if (icache_line_hits(li, v | (start_virt & 1))) {
          const u64 lo = (v > vs) ? v : vs;
          const u64 hi = (v + 0x800 < ve) ? v + 0x800 : ve;
          const u64 poff = line.p_address + (lo - v);
          if (poff + (hi - lo) <= dram_size &&
              memcmp((const uint8_t *)dram_ptr + poff,
                     (const uint8_t *)line.data + (lo - v),
                     (size_t)(hi - lo)) != 0)
            src_stale = true;
        }
      }
    }
    if (g_trace_icprobe && have_phys && !(start_virt & 1)) {
      const int li =
          (int)(((start_virt & ~U64(3)) >> 11) & (ICACHE_ENTRIES - 1));
      if (icache_line_hits(li, start_virt)) {
        const u64 fetched =
            state.icache[li].p_address + (start_virt & ICACHE_BYTE_MASK);
        static std::atomic<int> nr{0};
        if (fetched != start_phys && nr++ < 50)
          printf("[JIT][CPU%d] ICPROBE-RECORD: pc=%016llx recorded phys "
                 "%016llx, fetched from %016llx, icount=%llu\n",
                 (int)state.iProcNum, (unsigned long long)start_virt,
                 (unsigned long long)start_phys, (unsigned long long)fetched,
                 (unsigned long long)state.instruction_count);
      }
    }
    // Record only translatable block starts (a translation miss left have_phys
    // false).
    if (have_phys && !src_stale && state.pc != expected) {
      CJitEngine::JitBlock *nb =
          m_jit->record(start_virt, start_phys, start_asn, start_cm, start_asm,
                        n, (const uint8_t *)dram_ptr);
      // Compile only once the block has proven hot (see compile_after()) --
      // except the stall loop, which is only of use to us interpreted.
      if (!(g_stall_skip && start_virt == m_stall_pc) && !nb->compiled &&
          ++nb->cold_runs >= m_jit->compile_after())
        compile_outside(nb);
    }
  }
}

// Side-effect-free data superpage probe for the JIT memory helpers, mirroring
// virt2phys (VA form check, then SPE[2]/[1]/[0] before the TB). The TB never
// holds superpage translations, so without this every kernel superpage access
// that misses the data page cache bailed to the interpreter. Returns 1 with
// *phys set on a hit, 0 when va is not a superpage address (use the TB), or
// -1 when the interpreter must take it (bad VA form, or a hit outside kernel
// mode -> ACV).
int CAlphaCPU::jit_spe_data(u64 va, int cm, u64 *phys) const {
  // The native EV8: Appendix C's superpages and sign check, the 21464's
  // (ev8_superpage). The TB behind it is the 21464's too: the entries
  // ev8_add_tb makes, valid only in their TPU group, with their own masks;
  // the page cache holds 8 KB pieces of any page, as for a granularity hint.
  if (m_ev8n) {
    const int r = ev8_superpage(va, false, cm, phys);
    return r < 0 ? -1 : r;
  }
  if (!alpha_valid_va_form(va, state.va_ctl_va_mode & 1))
    return -1;
  const int spe = state.m_ctl_spe;
  if (!spe)
    return 0;
  if (((va & SPE_2_MASK) == SPE_2_MATCH) && (spe & 4))
    *phys = va & SPE_2_MAP;
  else if (((va & SPE_1_MASK) == SPE_1_MATCH) && (spe & 2))
    *phys = (va & SPE_1_MAP) | ((va & SPE_1_TEST) ? SPE_1_ADD : 0);
  else if (((va & SPE_0_MASK) == SPE_0_MATCH) && (spe & 1))
    *phys = va & SPE_0_MAP;
  else
    return 0;
  return cm ? -1 : 1;
}

// The copies of virt2phys's fast path the helpers had before JIT-08, as
// each checked it: jit_spe_data, then FindTBEntry and the entry's access and
// fault bits (both for WRCHK); a VPTE fetch went straight to the TB.
int CAlphaCPU::jit_xlate_old(u64 va, int flags, int cm, u64 *phys) {
  if (!(flags & VPTE)) {
    const int spe = jit_spe_data(va, cm, phys);
    if (spe < 0)
      return XL_ACV;
    if (spe)
      return XL_OK;
  }
  const int rw = (flags & ACCESS_WRITE) ? 1 : 0;
  const int i = FindTBEntry(va, rw ? ACCESS_WRITE : ACCESS_READ);
  if (i < 0)
    return XL_MISS;
  const auto &e = state.tb[TB_INDEX_DATA][i];
  if (!e.access[rw][cm] || ((flags & WRCHK) && !e.access[1][cm]))
    return XL_ACV;
  if (e.fault[rw] || ((flags & WRCHK) && e.fault[1]))
    return XL_FAULT;
  *phys = e.phys | (va & e.keep_mask);
  return XL_OK;
}

int CAlphaCPU::jit_xlate(u64 va, int flags, int cm, u64 *phys) {
  if (!g_jit08_xlate)
    return jit_xlate_old(va, flags, cm, phys);
  int entry;
  return virt2phys_fast(va, flags, cm, phys, nullptr, &entry);
}

// JIT load helper (static). Reads size_bits from virtual address va into *out,
// mirroring DATA_PHYS_NT's normal-read fast path. Returns 0 on success, or 1 on
// a translation fault / unaligned access - the caller bails to the interpreter
#ifdef JIT_STATS
// Which device pages the helpers serve, and how often: the framebuffer we
// can offer for direct access, or an engine's port we cannot. Printed when
// the CPU goes away (dump_device_pages).
// A hashed table: the I/O window alone has hundreds of distinct ports.
static u64 g_devpage[65536], g_devpage_n[65536][2];
static u64 g_devpage_dropped = 0;
// Watched addresses, counted per 100M-instruction window as well (the
// timeline says which phase of a boot does what): the keyboard
// controller's data and status ports, the PIT, the IDE data port, the
// Pchip CSR page.
u64 g_watch_n[5];
static const u64 kWatch[5] = {U64(0x00000801fc000060), U64(0x00000801fc000061),
                              U64(0x00000801fc000040), U64(0x00000801fc0001f0),
                              U64(0x00000801a0000000)};
static void note_device_page(u64 phys, bool write) {
  for (int w = 0; w < 5; w++)
    if ((w == 4 ? (phys & ~U64(0x1FFF)) : phys) == kWatch[w])
      g_watch_n[w]++;
  // Keyed by page, except inside the PCI I/O window, where the port itself
  // is the question (one page holds the PIT, the RTC, the keyboard
  // controller, the IDE and the VGA ports).
  const bool io = (phys >> 26) == (U64(0x00000801fc000000) >> 26);
  // ...and the Pchip/Cchip CSR pages by register (64-byte spacing).
  const bool csr = (phys >> 30) == (U64(0x00000801a0000000) >> 30);
  // The I/O window by exact port (0x60 is the keyboard controller, 0x61
  // the system control port beside it).
  // The key keeps every address bit (an exact port has bit 0): shifted up
  // one, with bit 0 as the occupancy mark.
  const u64 key =
      ((io ? phys : csr ? (phys & ~U64(0x3F)) : (phys & ~U64(0x1FFF))) << 1) |
      1;
  u64 h = (key * U64(0x9E3779B97F4A7C15)) >> 48;
  for (int probe = 0; probe < 256; probe++, h = (h + 1) & 65535) {
    if (g_devpage[h] == key || g_devpage[h] == 0) {
      g_devpage[h] = key;
      g_devpage_n[h][write ? 1 : 0]++;
      return;
    }
  }
  g_devpage_dropped++;
}
void dump_device_pages() {
  printf("[JIT][STATS] device addresses served by the helpers (reads/writes), "
         "%llu dropped:\n",
         (unsigned long long)g_devpage_dropped);
  for (int i = 0; i < 65536; i++)
    if (g_devpage[i] && g_devpage_n[i][0] + g_devpage_n[i][1] > 20000)
      printf("[JIT][STATS]   %016llx  %10llu / %10llu\n",
             (unsigned long long)(g_devpage[i] >> 1),
             (unsigned long long)g_devpage_n[i][0],
             (unsigned long long)g_devpage_n[i][1]);
}
#endif

int CAlphaCPU::jit_read(CAlphaCPU *cpu, u64 va, int size_bits, u64 *out) {
  // Bit 8 of size_bits selects DTB_ALTMODE access checks (the HW_LD/HW_ST
  // virtual-alt forms); plain loads/stores pass the bare size. Bit 9 is a
  // prefetch, a load to R31 (DO_LDL and friends, HRM 2.6): translated, with
  // the load's faults, and nothing read or replayed; *out is not written.
  const int cm = (size_bits & 0x100) ? cpu->state.alt_cm : cpu->state.cm;
  const bool prefetch = (size_bits & 0x200) != 0;
  size_bits &= 0xff;
  const u64 amask = (u64)(size_bits / 8) - 1;
  cpu->m_jit->note_helper(CJitEngine::HK_READ);
  HELPER_TIMER(cpu, CJitEngine::HK_READ);
  // Unaligned: the interpreter does the load at the unaligned address when
  // it stays within the page, and traps when it crosses into the next one
  // (docs/cpu-fidelity.md, "Unaligned data accesses do not trap"). Do the
  // first here too, from DRAM; bail for the second and for a device.
  // Bailing on every one ended the chain -- makecab loads ~100k per 100M
  // instructions, and they were most of its interpreter time.
  const bool unaligned = (va & amask) != 0;
  if (unaligned && (va & U64(0x1FFF)) + size_bits / 8 > 0x2000) {
    cpu->m_jit->note_bail(false, CJitEngine::BK_UNALIGNED);
    return 1;
  }

  u64 phys;
  const u64 vp = va & ~U64(0x1FFF);
  SDataPageCache &dpc =
      cpu->data_page_cache[0][cpu->dpc_index(va)]; // direct-mapped by virt page
  // Why the INLINE probe sent us here, which is not the same question as why
  // this helper bails. The inline probe compares one packed tag, so it rejects
  // an empty slot, another page in the slot, a different address space or
  // mode, and an MMIO page alike -- and the fixes for those differ.
  cpu->m_jit->note_dpc_miss(!dpc.valid            ? CJitEngine::DM_EMPTY
                            : dpc.virt_page != vp ? CJitEngine::DM_OTHER_PAGE
                            : dpc.cm != cm        ? CJitEngine::DM_MODE
                            : dpc.asn != cpu->state.asn0 ? CJitEngine::DM_ASN
                            : dpc.host_base == 0         ? CJitEngine::DM_MMIO
                                                         : CJitEngine::DM_HIT);
  if (dpc.valid && dpc.virt_page != vp)
    cpu->m_jit->note_dpc_other(0, unsigned(cpu->dpc_index(va)), vp,
                               dpc.virt_page);
  if (dpc.valid && dpc.virt_page == vp && dpc.cm == cm &&
      dpc.asn == cpu->state.asn0) {
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else if (cpu->dpc_promote(0, cpu->dpc_index(va), vp, cm,
                              cpu->state.asn0)) { // the second level had it
    cpu->m_jit->note_dpc_l2_helper();
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else {
    // virt2phys's translation without its side effects (virt2phys_fast):
    // virt2phys itself walks the page table and vectors faults, which,
    // done again by the interpreter after we bail, corrupts state. Bail on
    // a TB miss or any fault and let the interpreter do it; it fills this
    // cache, so the next compiled run hits.
    const int x = cpu->jit_xlate(va, ACCESS_READ, cm, &phys);
    if (x != XL_OK) {
      cpu->m_jit->note_bail(false, x == XL_MISS    ? CJitEngine::BK_TB_MISS
                                   : x == XL_FAULT ? CJitEngine::BK_FAULT
                                                   : CJitEngine::BK_ACV);
      return 1;
    }
    dpc.fill(vp, phys & ~U64(0x1FFF),
             cpu->dpc_host_base(phys),
             cm, cpu->state.asn0);
    cpu->dpc_l2_put(0, dpc);
  }
  if (prefetch) // translated: a prefetch reads nothing, not even a device
    return 0;

  // DRAM only: bail on MMIO so the interpreter does device reads (side effects
  // + ordering). This MUST precede the verify replay: production bails here,
  // and a device byte/word read is NOT size-truncated (LDBU/LDWU use READ_VIRT,
  // no sext func), so replaying an MMIO value and then re-truncating it with
  // movzx would falsely mismatch the interpreter. Bailing -> the compiled block
  // stops at the load (done < prefix_len) and the verify skips the compare,
  // matching prod.
  if (phys >= cpu->dram_size) {
    if (unaligned) { // a device register read at an odd address: interpreter
      cpu->m_jit->note_bail(false, CJitEngine::BK_UNALIGNED);
      return 1;
    }
#ifdef JIT_STATS
    note_device_page(phys, false);
#endif
    if (dpc.host_base) { // device memory offered for direct access
      *out = dram_read((const char *)dpc.host_base, phys & U64(0x1FFF),
                       size_bits);
      return 0;
    }
#ifdef JIT_VERIFY
    return 1;
#else
    // Production: do the device read here, at the same point in the
    // instruction stream the interpreter would. Bailing instead sent every
    // MMIO load (device registers, S3 aperture) back through the interpreter.
    *out = cpu->sys_read(phys, size_bits);
    return 0;
#endif
  }

  // Verify replay: return the value the interpreter pass loaded here, rather
  // than re-reading (another CPU may have written it)
  if (cpu->m_jit_vreplay) {
    if (va != cpu->m_jit_vaddr[cpu->m_jit_vlog_i]) {
      cpu->m_jit_vbad = true; // a wrong address replays a right value
      static std::atomic<int> n{0};
      if (n++ < 50)
        printf(
            "[JIT] LOAD ADDR MISMATCH: compiled va=%016llx interp va=%016llx\n",
            (unsigned long long)va,
            (unsigned long long)cpu->m_jit_vaddr[cpu->m_jit_vlog_i]);
    }
    *out = cpu->m_jit_vlog[cpu->m_jit_vlog_i++];
    return 0;
  }

  *out = dram_read(cpu->dram_ptr, phys, size_bits);
  return 0;
}

// JIT FP load helper (static). LDS/LDT: f[fa] = convert(MEM[va]) per
// DO_LDS/DO_LDT. descr packs size (low 16) and fmt (1=S/ieee_lds, 0=T/raw, high
// bits). FPSTART (fpen -> FEN bail; exc_sum=0). Production reuses jit_read's
// side-effect-free cache read; verify replays the interp's CONVERTED f-value
// (FP loads join the load log), so it consumes m_jit_vlog like an integer load.
int CAlphaCPU::jit_fp_read(CAlphaCPU *cpu, u64 va, u32 fa, u32 descr) {
  if (cpu->state.fpen == 0)
    return 1; // FEN trap (FPSTART)
  cpu->state.exc_sum = 0;
  if (fa == 31)
    return 0; // f31 dest: interp skips the read
  if (cpu->m_jit_vreplay) {
    const u32 i = cpu->m_jit_vlog_i++;
    if (va != cpu->m_jit_vaddr[i]) {
      cpu->m_jit_vbad = true; // a wrong address replays a right value
      static std::atomic<int> n{0};
      if (n++ < 50)
        printf("[JIT] FP LOAD ADDR MISMATCH: compiled va=%016llx interp "
               "va=%016llx\n",
               (unsigned long long)va, (unsigned long long)cpu->m_jit_vaddr[i]);
    }
    cpu->state.f[fa] =
        cpu->m_jit_vlog[i]; // logged converted value (no re-convert)
    return 0;
  }
  u64 raw;
  if (jit_read(cpu, va, (int)(descr & 0xffff), &raw))
    return 1;          // production cache read
  switch (descr >> 16) // fmt: 0=T raw, 1=S ieee, 2=F vax, 3=G vax
  {
  case 1:
    cpu->state.f[fa] = cpu->ieee_lds((u32)raw);
    break; // LDS
  case 2:
    cpu->state.f[fa] = cpu->vax_ldf((u32)raw);
    break; // LDF
  case 3:
    cpu->state.f[fa] = cpu->vax_ldg(raw);
    break; // LDG
  default:
    cpu->state.f[fa] = raw;
    break; // LDT raw
  }
  return 0;
}

// JIT FP store helper (static). STS/STT: MEM[va] = convert(f[fa]) per
// DO_STS/DO_STT. Routes through jit_write so verify compares vs the store log
// (FP stores join it) and production writes.
int CAlphaCPU::jit_fp_write(CAlphaCPU *cpu, u64 va, u32 fa, u32 descr) {
  if (cpu->state.fpen == 0)
    return 1; // FEN trap (FPSTART)
  cpu->state.exc_sum = 0;
  u64 value;
  switch (descr >> 16) // fmt: 0=T raw, 1=S ieee, 2=F vax, 3=G vax
  {
  case 1:
    value = (u64)cpu->ieee_sts(cpu->state.f[fa]);
    break; // STS
  case 2:
    value = (u64)cpu->vax_stf(cpu->state.f[fa]);
    break; // STF
  case 3:
    value = cpu->vax_stg(cpu->state.f[fa]);
    break; // STG
  default:
    value = cpu->state.f[fa];
    break; // STT raw
  }
  return jit_write(cpu, va, (int)(descr & 0xffff), value);
}

// JIT MISC read helper (static). RPCC (sel 0): the wall-clock-pinned cycle
// counter (DO_RPCC, JIT lane). RC (sel 1) / RS (sel 2): read the interrupt
// flag, then clear / set it. All three read state the differential verify can't
// re-derive (cc advances only at the jit_run boundary; the flag is consumed by
// the read), so in verify we replay the interp pass's value -- like a load.
u64 CAlphaCPU::jit_misc(CAlphaCPU *cpu, u32 sel) {
  // MB with a page-cache flush request pending (memory_barrier): compiled
  // MB fenced and tested the flag inline. It reads nothing the verify pass
  // logged, so it is not replayed.
  if (sel == CJitEngine::kMiscMbFlush) {
    cpu->honour_at_mb();
    return 0;
  }
  cpu->m_misc_calls[sel < 3 ? sel : 2]++;
  if (cpu->m_jit_vreplay)
    return cpu->m_jit_vlog[cpu->m_jit_vlog_i++]; // replay; no re-read, no
                                                 // double side effect

  switch (sel) {
  case 0: // RPCC: Ra = cc_offset : cc[31:0], synced to now at each read
    return cpu->rpcc_read();
  case 1: // RC: Ra = bIntrFlag; bIntrFlag = false
  {
    u64 v = cpu->state.bIntrFlag ? 1 : 0;
    cpu->state.bIntrFlag = false;
    return v;
  }
  default: // RS (sel 2): Ra = bIntrFlag; bIntrFlag = true
  {
    u64 v = cpu->state.bIntrFlag ? 1 : 0;
    cpu->state.bIntrFlag = true;
    return v;
  }
  }
}

// JIT int->FP move helper (static). ITOFx: f[fc] = fmt(value). Mirrors DO_ITOFx
// incl. FPSTART (fpen==0 -> return 1, the FEN-trap bail; exc_sum cleared). fmt:
// 0=T raw, 1=S, 2=F.
int CAlphaCPU::jit_itof(CAlphaCPU *cpu, u32 fc, u64 value, u32 fmt) {
  if (cpu->state.fpen == 0)
    return 1;             // FEN trap: the interpreter vectors it
  cpu->state.exc_sum = 0; // FPSTART
  if (fmt == 1)
    cpu->state.f[fc] = cpu->ieee_lds((u32)value);
  else if (fmt == 2)
    cpu->state.f[fc] = cpu->vax_ldf(SWAP_VAXF((u32)value));
  else
    cpu->state.f[fc] = value;
  return 0;
}

// JIT FP->int move helper (static). FTOIx: *out = fmt(f[fa]). Same FPSTART
// bail. fmt: 0=T, 1=S.
int CAlphaCPU::jit_ftoi(CAlphaCPU *cpu, u32 fa, u32 fmt, u64 *out) {
  if (cpu->state.fpen == 0)
    return 1;             // FEN trap: the interpreter vectors it
  cpu->state.exc_sum = 0; // FPSTART
  *out = (fmt == 1) ? sext_u64_32(cpu->ieee_sts(cpu->state.f[fa]))
                    : cpu->state.f[fa];
  return 0;
}

// JIT FLTL non-arithmetic helper (static): FPCR moves, sign-copies, FP
// conditional moves (FCMOV2 too, on a row with CPU_EXT_EV8) and the
// CVTLQ/CVTQL bit rearrangements, through the
// interpreter's own fltl_operate. classify only routes these funcs here (no
// FP math, no /V trap forms). Returns 1 = FEN-trap bail: the interpreter
// vectors it.
int CAlphaCPU::jit_fltl(CAlphaCPU *cpu, u32 ins) {
  if (cpu->state.fpen == 0)
    return 1;
  // A function code that is no operate (classify() compiles none) bails
  // unexecuted: the interpreter takes OPCDEC.
  return cpu->fltl_operate(ins, (ins >> 5) & 0x7ff) ? 0 : 1;
}

// JIT VAX-FP arithmetic helper (static). FLTV (0x15) through the interpreter's
// own fltv_operate. FPSTART bail (return 1) if FP disabled. Returns 2 when the
// op delivered an arith trap: vax_trap GO_PALs (sets state.pc + exc_sum), so
// exc_sum != 0 detects it and the caller honors state.pc -- the op runs
// exactly once (no re-execute). Returns 0 on the common no-trap path. Fc==31
// is gated out in classify.
int CAlphaCPU::jit_fltv(CAlphaCPU *cpu, u32 ins) {
  if (cpu->state.fpen == 0)
    return 1; // FEN trap (FPSTART): op not run
  if (!cpu->fltv_operate(ins, (ins >> 5) & 0x7ff))
    return 1; // no operate (classify() compiles none): the interpreter OPCDECs
  return cpu->state.exc_sum
             ? 2
             : 0; // exc_sum != 0 => vax_trap GO_PAL'd -> caller honors state.pc
}

// JIT load-locked helper (static). LDx_L: the verify-checked load PLUS cpu_lock
// -- the LL/SC exclusive monitor. cpu_lock is per-CPU + atomic + idempotent.
int CAlphaCPU::jit_read_locked(CAlphaCPU *cpu, u64 va, int size_bits,
                               u64 *out) {
  cpu->m_jit->note_helper(CJitEngine::HK_LOCKED);
  HELPER_TIMER(cpu, CJitEngine::HK_LOCKED);
  const u64 amask = (u64)(size_bits / 8) - 1;
  if (va & amask)
    return 1; // unaligned: let the interpreter handle it

  u64 phys;
  const u64 vp = va & ~U64(0x1FFF);
  SDataPageCache &dpc = cpu->data_page_cache[0][cpu->dpc_index(va)];
  if (dpc.valid && dpc.virt_page == vp && dpc.cm == cpu->state.cm &&
      dpc.asn == cpu->state.asn0) {
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else if (cpu->dpc_promote(0, cpu->dpc_index(va), vp, cpu->state.cm,
                              cpu->state.asn0)) { // the second level had it
    cpu->m_jit->note_dpc_l2_helper();
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else {
    if (cpu->jit_xlate(va, ACCESS_READ, cpu->state.cm, &phys) != XL_OK)
      return 1; // a TB miss or a fault: the interpreter takes it
    dpc.fill(vp, phys & ~U64(0x1FFF),
             cpu->dpc_host_base(phys),
             cpu->state.cm, cpu->state.asn0);
    cpu->dpc_l2_put(0, dpc);
  }

  if (phys >=
      cpu->dram_size) // MMIO: interpreter handles the I/O-space locked path
    return 1;

  CSystem::CLLSCDRAMGuard llsc_guard(cpu->cSystem, true);
  if (cpu->m_jit_vreplay) {
    if (va != cpu->m_jit_vaddr[cpu->m_jit_vlog_i]) {
      cpu->m_jit_vbad = true; // a wrong address replays a right value
      static std::atomic<int> n{0};
      if (n++ < 50)
        printf("[JIT] LDx_L ADDR MISMATCH: compiled va=%016llx interp "
               "va=%016llx\n",
               (unsigned long long)va,
               (unsigned long long)cpu->m_jit_vaddr[cpu->m_jit_vlog_i]);
    }
    *out = cpu->m_jit_vlog[cpu->m_jit_vlog_i++]; // the interp pass's (already
                                                 // sign-extended) value
  } else {
    const u64 raw = dram_read(cpu->dram_ptr, phys, size_bits);
    *out = (size_bits == 32)
               ? sext_u64_32(raw)
               : raw; // LDL_L sign-extends; LDQ_L is the full quad
  }

  // Establish the LL exclusive monitor (DO_LDx_L's READ_VIRT_LOCK_F makes the
  // same cpu_lock call).
  cpu->cSystem->cpu_lock(cpu->state.iProcNum, phys, *out);
  cpu->m_llsc_clean = 1;
  return 0;
}

// JIT HW_LD VPTE helper (static). HW_LD func 5 (the DTBMISS PTE fetch): a
// virtual read access- checked vs KERNEL, not cm (DO_HW_LDQ case 5).
// Side-effect-free TB probe; bails on miss so the interpreter vectors the
// double miss. Skips the data_page_cache (keyed by current cm).
int CAlphaCPU::jit_read_vpte(CAlphaCPU *cpu, u64 va, int size_bits, u64 *out) {
  const u64 amask = (u64)(size_bits / 8) - 1;
  if (va & amask)
    return 1; // unaligned: let the interpreter handle it

  // A VPTE fetch is checked in kernel mode (virt2phys).
  u64 phys;
  if (cpu->jit_xlate(va, ACCESS_READ | VPTE, 0, &phys) != XL_OK)
    return 1; // a miss (the double miss) or a fault: the interpreter's

  if (phys >= cpu->dram_size) // MMIO: bail before the replay (mirrors jit_read)
    return 1;

  if (cpu->m_jit_vreplay) {
    if (va != cpu->m_jit_vaddr[cpu->m_jit_vlog_i]) {
      cpu->m_jit_vbad = true; // a wrong address replays a right value
      static std::atomic<int> n{0};
      if (n++ < 50)
        printf(
            "[JIT] VPTE ADDR MISMATCH: compiled va=%016llx interp va=%016llx\n",
            (unsigned long long)va,
            (unsigned long long)cpu->m_jit_vaddr[cpu->m_jit_vlog_i]);
    }
    *out = cpu->m_jit_vlog[cpu->m_jit_vlog_i++];
    return 0;
  }

  *out = dram_read(cpu->dram_ptr, phys, size_bits);
  return 0;
}

// JIT HW_LD WrChk helper (static). HW_LD func 0xa (DO_HW_LDL case 10, HRM TYPE
// 1012 WrChk): a longword VIRTUAL read that ALSO requires WRITE access -- the
// interpreter ACVs if read OR write protection is clear (virt2phys WRCHK) and
// faults FOR/FOW. Side-effect-free TB probe mirroring jit_read_vpte but checked
// vs CURRENT mode (not kernel); bails (interp re-runs + vectors the fault) on
// miss/protection/fault/MMIO. Verify replays the value like any load.
int CAlphaCPU::jit_read_wchk(CAlphaCPU *cpu, u64 va, int size_bits, u64 *out) {
  const u64 amask = (u64)(size_bits / 8) - 1;
  if (va & amask)
    return 1; // unaligned: let the interpreter handle it

  u64 phys;
  const int cm = cpu->state.cm;
  // WrChk: read and write access, neither fault bit (virt2phys_fast).
  if (cpu->jit_xlate(va, ACCESS_READ | WRCHK, cm, &phys) != XL_OK)
    return 1; // a miss, an ACV, FOR/FOW: the interpreter vectors it

  if (phys >=
      cpu->dram_size) // MMIO: bail before the replay (mirrors jit_read_vpte)
    return 1;

  if (cpu->m_jit_vreplay) {
    if (va != cpu->m_jit_vaddr[cpu->m_jit_vlog_i]) {
      cpu->m_jit_vbad = true; // a wrong address replays a right value
      static std::atomic<int> n{0};
      if (n++ < 50)
        printf(
            "[JIT] WCHK ADDR MISMATCH: compiled va=%016llx interp va=%016llx\n",
            (unsigned long long)va,
            (unsigned long long)cpu->m_jit_vaddr[cpu->m_jit_vlog_i]);
    }
    *out = cpu->m_jit_vlog[cpu->m_jit_vlog_i++];
    return 0;
  }

  *out = dram_read(cpu->dram_ptr, phys, size_bits);
  return 0;
}

// JIT HW_LD helper (static). Physical read of size_bits at phys -> *out with NO
// translation - the PALmode HW_LD physical longword/quadword forms (func 0/1).
// Aligns like READ_PHYS_NT. Verify replays the interpreter's value (race-free);
// MMIO bails so the interpreter does the ordered device read. Returns 0 on
// success, 1 on a bail.
int CAlphaCPU::jit_read_phys(CAlphaCPU *cpu, u64 phys, int size_bits,
                             u64 *out) {
  cpu->m_jit->note_helper(CJitEngine::HK_READ_PHYS);
  HELPER_TIMER(cpu, CJitEngine::HK_READ_PHYS);
  // MMIO: bail before the replay so verify models production (which bails
  // here). A device read isn't size-truncated, so a replayed+re-truncated value
  // would falsely mismatch. dram_size is page-aligned and the align below only
  // rounds within 8 bytes, so this raw check is exact.
  if (phys >= cpu->dram_size) {
#ifdef JIT_VERIFY
    return 1;
#else
    // Production: the ordered device read happens here, as in the interpreter
    // (see jit_read).
    phys &= ~((u64)(size_bits / 8) - 1); // align like READ_PHYS_NT (ALIGN_PHYS)
    *out = cpu->sys_read(phys, size_bits);
    return 0;
#endif
  }

  if (cpu->m_jit_vreplay) {
    if (phys != cpu->m_jit_vaddr[cpu->m_jit_vlog_i]) {
      cpu->m_jit_vbad = true; // a wrong address replays a right value
      static std::atomic<int> n{0};
      if (n++ < 50)
        printf("[JIT] HW_LD ADDR MISMATCH: compiled pa=%016llx interp "
               "pa=%016llx\n",
               (unsigned long long)phys,
               (unsigned long long)cpu->m_jit_vaddr[cpu->m_jit_vlog_i]);
    }
    *out = cpu->m_jit_vlog[cpu->m_jit_vlog_i++];
    return 0;
  }

  phys &= ~((u64)(size_bits / 8) - 1); // align like READ_PHYS_NT (ALIGN_PHYS)
  *out = dram_read(cpu->dram_ptr, phys, size_bits);
  return 0;
}

// JIT store helper (static). Writes size_bits of value to virtual address va,
// mirroring jit_read's side-effect-free translation. Returns 0 on success, 1 on
// fault/unaligned.
int CAlphaCPU::jit_write(CAlphaCPU *cpu, u64 va, int size_bits, u64 value) {
  // Bit 8 of size_bits selects DTB_ALTMODE access checks (the HW_LD/HW_ST
  // virtual-alt forms); plain loads/stores pass the bare size.
  const int cm = (size_bits & 0x100) ? cpu->state.alt_cm : cpu->state.cm;
  size_bits &= 0xff;
  const u64 amask = (u64)(size_bits / 8) - 1;
  cpu->m_jit->note_helper(CJitEngine::HK_WRITE);
  HELPER_TIMER(cpu, CJitEngine::HK_WRITE);
  if (va & amask) { // unaligned: let the interpreter handle it
    cpu->m_jit->note_bail(true, CJitEngine::BK_UNALIGNED);
    return 1;
  }

  // Verify: the interpreter pass already performed (and recorded) this store.
  // Compare rather than write -- stores change memory, not GPRs, so the
  // differential GPR check can't see them; this is how compiled stores get
  // validated.
  if (cpu->m_jit_vreplay) {
    const u32 i = cpu->m_jit_slog_i++;
    if (va != cpu->m_jit_slog_addr[i] || value != cpu->m_jit_slog_val[i]) {
      cpu->m_jit_vbad = true;
      static std::atomic<int> n{0};
      if (n++ < 50)
        printf("[JIT] STORE MISMATCH: compiled va=%016llx val=%016llx  interp "
               "va=%016llx val=%016llx\n",
               (unsigned long long)va, (unsigned long long)value,
               (unsigned long long)cpu->m_jit_slog_addr[i],
               (unsigned long long)cpu->m_jit_slog_val[i]);
    }
    return 0;
  }

  u64 phys;
  const u64 vp = va & ~U64(0x1FFF);
  SDataPageCache &dpc =
      cpu->data_page_cache[1][cpu->dpc_index(va)]; // direct-mapped by virt page
  if (dpc.valid && dpc.virt_page != vp)
    cpu->m_jit->note_dpc_other(1, unsigned(cpu->dpc_index(va)), vp,
                               dpc.virt_page);
  if (dpc.valid && dpc.virt_page == vp && dpc.cm == cm &&
      dpc.asn == cpu->state.asn0) {
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else if (cpu->dpc_promote(1, cpu->dpc_index(va), vp, cm,
                              cpu->state.asn0)) { // the second level had it
    cpu->m_jit->note_dpc_l2_helper();
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else {
    // virt2phys's translation without its side effects, for the write
    // cache [1]; bail on a TB miss or any fault so the interpreter does the
    // translation that vectors them (and fills this cache).
    const int x = cpu->jit_xlate(va, ACCESS_WRITE, cm, &phys);
    if (x != XL_OK) {
      cpu->m_jit->note_bail(true, x == XL_MISS    ? CJitEngine::BK_TB_MISS
                                  : x == XL_FAULT ? CJitEngine::BK_FAULT
                                                  : CJitEngine::BK_ACV);
      return 1;
    }
    dpc.fill(vp, phys & ~U64(0x1FFF), cpu->dpc_host_base_w(phys), cm,
             cpu->state.asn0);
    cpu->dpc_l2_put(1, dpc);
  }

  if (phys < cpu->dram_size) {
    dram_write(cpu->dram_ptr, phys, size_bits, value);
    cpu->note_dram_write(phys); // compiled code never writes a code page
                                // inline, so its stores land here
  } else if (dpc.host_base)     // device memory offered for direct access
    dram_write((char *)dpc.host_base, phys & U64(0x1FFF), size_bits, value);
  else {
#ifdef JIT_STATS
    note_device_page(phys, true);
#endif
    cpu->sys_write(phys, size_bits, value);
  }
  return 0;
}

// JIT HW_ST helper (static). Physical write of size_bits = value at phys with
// NO translation - the PALmode HW_ST physical longword/quadword forms (func
// 0/1). Aligns like WRITE_PHYS_NT. Verify compares against the interpreter's
// recorded store (stores change memory, not GPRs); MMIO bails so the
// interpreter does the ordered device write. Returns 0 on success, 1 on a bail.
int CAlphaCPU::jit_write_phys(CAlphaCPU *cpu, u64 phys, int size_bits,
                              u64 value) {
  cpu->m_jit->note_helper(CJitEngine::HK_WRITE_PHYS);
  HELPER_TIMER(cpu, CJitEngine::HK_WRITE_PHYS);
  if (cpu->m_jit_vreplay) {
    const u32 i = cpu->m_jit_slog_i++;
    if (phys != cpu->m_jit_slog_addr[i] || value != cpu->m_jit_slog_val[i]) {
      cpu->m_jit_vbad = true;
      static std::atomic<int> n{0};
      if (n++ < 50)
        printf("[JIT] HW_ST STORE MISMATCH: compiled pa=%016llx val=%016llx  "
               "interp pa=%016llx val=%016llx\n",
               (unsigned long long)phys, (unsigned long long)value,
               (unsigned long long)cpu->m_jit_slog_addr[i],
               (unsigned long long)cpu->m_jit_slog_val[i]);
    }
    return 0;
  }

  phys &= ~((u64)(size_bits / 8) - 1); // align like WRITE_PHYS_NT (ALIGN_PHYS)
  if (phys >= cpu->dram_size) {
#ifdef JIT_VERIFY
    return 1; // MMIO: let the interpreter do the ordered write
#else
    // Production: the device write happens here, in instruction order (as
    // jit_write already does for virtual stores).
    cpu->sys_write(phys, size_bits, value);
    return 0;
#endif
  }
  dram_write(cpu->dram_ptr, phys, size_bits, value);
  cpu->note_dram_write(phys); // HW_ST can land on a code page too
  return 0;
}

// JIT store-conditional helper (static). Same-address STx_C uses the emulator's
// CAS-backed MP model; different-address same-line STx_C stores without
// comparing against the LDx_L datum.
u64 CAlphaCPU::jit_stc(CAlphaCPU *cpu, u64 va, int size_bits, u64 value) {
  cpu->m_jit->note_helper(CJitEngine::HK_STC);
  HELPER_TIMER(cpu, CJitEngine::HK_STC);
  if (cpu->m_jit_vreplay) {
    const u32 i = cpu->m_jit_slog_i++;
    const u64 success = cpu->m_jit_slog_success[i];
    if (va != cpu->m_jit_slog_addr[i] ||
        (success && value != cpu->m_jit_slog_val[i])) {
      cpu->m_jit_vbad = true;
      static std::atomic<int> n{0};
      if (n++ < 50)
        printf("[JIT] STx_C MISMATCH: compiled va=%016llx val=%016llx ok=%llu  "
               "interp va=%016llx val=%016llx\n",
               (unsigned long long)va, (unsigned long long)value,
               (unsigned long long)success,
               (unsigned long long)cpu->m_jit_slog_addr[i],
               (unsigned long long)cpu->m_jit_slog_val[i]);
    }
    return success;
  }

  const u64 amask = (u64)(size_bits / 8) - 1;
  if (va & amask)
    return U64(
        0x100); // unaligned (also a page-cross): the interpreter handles it

  // Side-effect-free write-path translation (mirror jit_write); bail to the
  // interpreter on a TB miss / protection / fault-on-write so it does the
  // side-effecting translation.
  u64 phys;
  const u64 vp = va & ~U64(0x1FFF);
  SDataPageCache &dpc = cpu->data_page_cache[1][cpu->dpc_index(va)];
  if (dpc.valid && dpc.virt_page == vp && dpc.cm == cpu->state.cm &&
      dpc.asn == cpu->state.asn0) {
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else if (cpu->dpc_promote(1, cpu->dpc_index(va), vp, cpu->state.cm,
                              cpu->state.asn0)) { // the second level had it
    cpu->m_jit->note_dpc_l2_helper();
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else {
    if (cpu->jit_xlate(va, ACCESS_WRITE, cpu->state.cm, &phys) != XL_OK)
      return U64(0x100); // a miss or a fault: the interpreter takes it
    dpc.fill(vp, phys & ~U64(0x1FFF), cpu->dpc_host_base_w(phys), cpu->state.cm,
             cpu->state.asn0);
    cpu->dpc_l2_put(1, dpc);
  }

  // Shared LL/SC path: consumes the reservation, applies the ABA sequence
  // guard, then CASes RAM or does the MMIO conditional store.
  CSystem::CLLSCDRAMGuard llsc_guard(cpu->cSystem, phys < cpu->dram_size);
  return cpu->cSystem->cpu_stx_c(cpu->state.iProcNum, phys, size_bits, value,
                                 cpu->dram_ptr, cpu->dram_size, cpu,
                                 cpu->llsc_intervened());
}

/* CALL_PAL OPCDEC trap: a privileged function (< 0x40) attempted in user mode.
   Mirrors GO_PAL(OPCDEC) -- save the faulting PC in EXC_ADDR, vector to the
   PALcode OPCDEC entry, and clear the load-lock flag */
void CAlphaCPU::jit_opcdec(CAlphaCPU *cpu, u64 cpc) {
  cpu->state.exc_addr = cpc;
  cpu->set_pc(cpu->state.pal_base | OPCDEC | U64(1));
  cpu->cSystem->cpu_clear_lock(cpu->state.iProcNum);
}

/* HW_MFPR (PALmode): return the IPR selected by (ins>>8)&0xff, as the
 * interpreter's DO_HW_MFPR reads it (hw_mfpr). classify() compiles only the
 * IPRs that are pure reads. */
u64 CAlphaCPU::jit_hw_mfpr(CAlphaCPU *cpu, u32 ins, u64 cur) {
  cpu->m_jit->note_helper(CJitEngine::HK_MFPR);
  HELPER_TIMER(cpu, CJitEngine::HK_MFPR);
  const u32 function = (ins >> 8) & 0xff;
  // ISUM (0x0d) reads the live async interrupt-request lines (eir/slr/crr/pcr),
  // which the differential verify can't re-derive - changes between the interp
  // and compiled passes. Replay the interp value here
  if (cpu->m_jit_vreplay && function == 0x0d)
    return cpu->m_jit_vlog[cpu->m_jit_vlog_i++];
  (void)cur;
  return cpu->hw_mfpr(function, true);
}

/* HW_MTPR (PALmode): the IPR write selected by `function` (value = Rb), as the
 * interpreter's DO_HW_MTPR makes it (hw_mtpr). Pure stores are verify-compared
 * via the IPR snapshot; the TB fills forward to add_tb_i/_d (idempotent, so the
 * verify double-run is safe); the check_int=true kick (IER/CM/SIRR/AST) can
 * only force an interrupt poll, never suppress one. classify() never compiles
 * the 0x40-7f ASN write (bit 0: it flushes the dpc and bumps the asn epoch),
 * PAL_BASE or VA_CTL. */
void CAlphaCPU::jit_hw_mtpr(CAlphaCPU *cpu, u32 function, u64 value) {
  cpu->m_jit->note_helper(CJitEngine::HK_MTPR);
  HELPER_TIMER(cpu, CJitEngine::HK_MTPR);
  cpu->m_jit->note_mtpr(function);
  cpu->hw_mtpr(function, value, true);
}

// JIT indirect-jump chaining helper (static). For a compiled JMP/HW_RET, look
// up the block at the runtime target in this CPU's block cache (the
// dispatcher's own lookup -- validates valid + tag + asn/asm_global) and return
// its chained re-entry point if it's compiled and runnable in the current
// context; else null, so the compiled jump bails to the dispatcher. Keying on
// the actual target lets any number of distinct targets chain without the old
// single-slot link thrashing on varying jumps.
void *CAlphaCPU::jit_indirect(CAlphaCPU *cpu, u64 target) {
  cpu->m_jit->note_jmp_attempt();
  cpu->m_jit->note_helper(CJitEngine::HK_INDIRECT);
  HELPER_TIMER(cpu, CJitEngine::HK_INDIRECT);
  // PAL reset-vector entry: never chain in, so the dispatcher's flush runs.
  if (!g_jit_chain || target == (cpu->state.pal_base | 1))
    return nullptr;
  CJitEngine::JitBlock *b =
      cpu->m_jit->lookup(target, (u32)cpu->state.asn, (uint8_t)cpu->state.cm);
  if (!b || !b->jit_body)
    return nullptr;
  // A block holding a guest debugger's breakpoint is entered only from the
  // dispatcher, which runs it interpreted (jit_run).
  if (cpu->m_debug_on.load(std::memory_order_relaxed) &&
      !cpu->m_debug->may_enter(cpu, target, b->prefix_len))
    return nullptr;
  // Idle pacing only sees passes through an idle/parked loop head that come
  // back to the dispatcher (a beta 2 HAL's HalProcessorIdle returns into the
  // head), so never chain into one.
  if (target == cpu->m_idle_pc || target == cpu->m_park_pc)
    return nullptr;
  if (target & 1) {
    // PALmode target: the I-stream is physically addressed (not paged), so no
    // remap / no stale risk. A PALmode block's shadow-register remap assumes
    // SDE -- honor the dispatcher's guard.
    if (!cpu->state.sde)
      return nullptr;
    cpu->m_jit->note_jmp_hit();
    return b->jit_body;
  }
  // Native target. FAST PATH: validated under the current epoch (lookup proved
  // flush-fresh, so a vgen mismatch here means an ITB invalidate) -- chain with
  // no re-translation.
  const u64 gen = cpu->m_jit->vgen();
  if (b->vgen == gen) {
    if (g_trace_icprobe) {
      u64 live = 0;
      bool asmb;
      if (cpu->virt2phys(target & ~U64(3), &live, ACCESS_EXEC | FAKE, &asmb,
                         0) == 0 &&
          live != b->phys) {
        static std::atomic<int> nj{0};
        if (nj++ < 50)
          printf("[JIT][CPU%d] ICPROBE-CHAIN: jump to %016llx enters code "
                 "compiled from %016llx, live %016llx, icount=%llu\n",
                 (int)cpu->state.iProcNum, (unsigned long long)target,
                 (unsigned long long)b->phys, (unsigned long long)live,
                 (unsigned long long)cpu->state.instruction_count);
      }
    }
    cpu->m_jit->note_jmp_hit();
    cpu->m_jit->ind_cache_fill(target, b);
    return b->jit_body;
  }
  // SLOW PATH (only right after an ITB invalidate): the in-frame chain bypasses
  // the dispatcher's `b->phys == start_phys` staleness check (see the
  // hot-path). Virtual+ASN keying can't see a page remap, so a stale block
  // (same tag+ASN, but the vpage now maps different physical bytes) would
  // tail-execute as wrong code -> OPCDEC / garbage.
  // The live translation, by virt2phys's rules (a superpage too, and the
  // entry's execute right and fault bit in this mode); anything it does not
  // translate is the dispatcher's, which faults it in.
  u64 live;
  if (g_jit08_xlate) {
    int entry;
    if (cpu->virt2phys_fast(target, ACCESS_EXEC, cpu->state.cm, &live, nullptr,
                            &entry) != XL_OK)
      return nullptr;
  } else {
    const int i = cpu->FindTBEntry(target, ACCESS_EXEC);
    if (i < 0)
      return nullptr; // ITB miss: let the dispatcher fault it in
    const auto &e = cpu->state.tb[TB_INDEX_ITB][i];
    live = e.phys | (target & e.keep_mask);
  }
  if (live != b->phys) {
    // Caught a stale block: the page was remapped without flushing the JIT
    // cache. Bail so the dispatcher re-records/recompiles. Rate-limited log --
    // this firing confirms the stale chain.
    static std::atomic<int> n{0};
    if (n++ < 50)
      printf("[JIT][CPU%d] INDIRECT STALE: target=%016llx block_phys=%016llx "
             "-- recompiling\n",
             (int)cpu->state.iProcNum, (unsigned long long)target,
             (unsigned long long)b->phys);
    return nullptr;
  }
  b->vgen = gen; // re-validated -> fast path henceforth
  cpu->m_jit->note_jmp_hit();
  cpu->m_jit->ind_cache_fill(target, b);
  return b->jit_body;
}

// ALPHABOX_JIT_RPCCTEST=1: the inline RPCC stub against jit_misc, the
// helper it replaces. Both are run from the same cycle-counter state and
// must agree on the value returned AND on the state left behind. The live
// clock is the one thing they cannot agree on -- each reads it for itself
// -- so every case starts with cc_last_sync far enough in the past that
// the elapsed time is clamped to its one-second ceiling, which makes the
// arithmetic the same whatever the host clock says. cc_last_sync itself
// is therefore excluded from the comparison.
void CAlphaCPU::jit_rpcc_selftest() {
  struct CcState {
    u64 last_sync, remainder, borrow, last_read, cc;
    u32 offset;
    bool ena;
  };
  auto save = [&]() {
    return CcState{cc_last_sync,  cc_wall_remainder, cc_borrow,
                   cc_last_read,  state.cc,          state.cc_offset,
                   state.cc_ena};
  };
  auto load = [&](const CcState &s) {
    cc_last_sync = s.last_sync;
    cc_wall_remainder = s.remainder;
    cc_borrow = s.borrow;
    cc_last_read = s.last_read;
    state.cc = s.cc;
    state.cc_offset = s.offset;
    state.cc_ena = s.ena;
  };
  const CcState entry = save();

  // A stub built here, so the test runs whatever the emitter would emit.
  // It is an AArch64 thing: the x86-64 emitter still calls the helper, so
  // there is nothing to compare against there.
  typedef u64 (*RpccFn)();
  // JIT_HOST_A64 is private to jitengine.cpp, so key off the architecture
  // the same way that file does.
#if defined(__aarch64__) || defined(_M_ARM64)
  RpccFn stub = m_jit ? (RpccFn)m_jit->a64_rpcc_stub() : nullptr;
#else
  RpccFn stub = nullptr;
#endif
  if (!stub) {
    printf("%%RPCC-F-SELFTEST: no inline stub on this build\n");
    load(entry);
    exit(2);
  }

  const CcState cases[] = {
      // clamped elapsed, counter running, nothing owed
      {0, 0, 0, 0, 0, 0, true},
      // a carry waiting in the sub-cycle remainder
      {0, 0xffffffffull, 0, 0, 12345, 7, true},
      // counter stopped: RPCC must not move it
      {0, 0, 0, 999, 999, 3, false},
      // the floor is ahead of the counter: lend, and report floor + 1
      {~U64(0), 0, 0, 1000000, 5, 0, true},
      // something already lent, to be repaid out of this tick's progress
      {0, 0, 4000, 0, 0, 1, true},
      // offset in the high half, counter near the 32-bit wrap
      {0, 0, 0, 0xfffffff0ull, 0xfffffff0ull, 0x5a5a5a5a, true},
  };

  int bad = 0;
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    load(cases[i]);
    const u64 want = jit_misc(this, 0);
    const CcState after_helper = save();

    load(cases[i]);
    const u64 got = stub();
    const CcState after_stub = save();

    const bool same_value = want == got;
    const bool same_state = after_helper.remainder == after_stub.remainder &&
                            after_helper.borrow == after_stub.borrow &&
                            after_helper.last_read == after_stub.last_read &&
                            after_helper.cc == after_stub.cc &&
                            after_helper.ena == after_stub.ena;
    if (!same_value || !same_state) {
      bad++;
      printf("%%RPCC-F-SELFTEST: case %u\n"
             "   helper %016llx  cc %llu rem %llu borrow %llu floor %llu\n"
             "   stub   %016llx  cc %llu rem %llu borrow %llu floor %llu\n",
             i, (unsigned long long)want, (unsigned long long)after_helper.cc,
             (unsigned long long)after_helper.remainder,
             (unsigned long long)after_helper.borrow,
             (unsigned long long)after_helper.last_read,
             (unsigned long long)got, (unsigned long long)after_stub.cc,
             (unsigned long long)after_stub.remainder,
             (unsigned long long)after_stub.borrow,
             (unsigned long long)after_stub.last_read);
    }
  }
  load(entry);
  printf("%%RPCC-I-SELFTEST: %u cases, %d disagreement(s)\n",
         (unsigned)(sizeof cases / sizeof cases[0]), bad);
  exit(bad ? 1 : 0);
}

#ifdef JIT_VERIFY
// Differential self-test of the compiled inline IEEE FP ops (FLTI 0x16 and the
// ITFP SQRTs) against the interpreter -- SRM boot never executes them, so the
// block verify can't reach them, and it skips its compare exactly when the
// interpreter traps (where a missing compiled bail would hide). Each case
// plants one instruction in a scratch page and runs it from identical state
// through execute() and through a compiled one-instruction PALmode block. The
// compiled contract: complete only with the interpreter's f[Fc], PC and
// exc_sum and an unchanged FPCR (no trap, no new sticky bit); otherwise bail
// with no side effect. ALPHABOX_JIT_FPTEST=1 runs it from init() on CPU0 and
// exits with the verdict (0 = pass).
void CAlphaCPU::jit_fp_selftest() {
  struct Variant {
    const char *name;
    u32 opc, func;
    bool binary;
    u64 native, bail, overbail, fail;
  };
  std::vector<Variant> vs;
  static const struct {
    const char *n;
    u32 base;
  } arith[] = {{"ADDS", 0x00}, {"SUBS", 0x01}, {"MULS", 0x02}, {"DIVS", 0x03},
               {"ADDT", 0x20}, {"SUBT", 0x21}, {"MULT", 0x22}, {"DIVT", 0x23}};
  // qualifiers: /N, /D, /UN, /UD, /SUN, /SUD, then /C and /SUID (interpreted)
  static const u32 arith_q[] = {0x080, 0x0c0, 0x180, 0x1c0,
                                0x580, 0x5c0, 0x000, 0x7c0};
  for (const auto &o : arith)
    for (u32 q : arith_q)
      vs.push_back({o.n, 0x16, q | o.base, true, 0, 0, 0, 0});
  static const struct {
    const char *n;
    u32 f;
  } cmps[] = {{"CMPTUN", 0x0a4},
              {"CMPTEQ", 0x0a5},
              {"CMPTLT", 0x0a6},
              {"CMPTLE", 0x0a7}};
  for (const auto &c : cmps) {
    vs.push_back({c.n, 0x16, c.f, true, 0, 0, 0, 0});
    vs.push_back({c.n, 0x16, c.f | 0x500, true, 0, 0, 0, 0});
  }
  static const struct {
    const char *n;
    u32 opc, f;
  } unary[] = {
      {"CVTQS", 0x16, 0x0bc}, {"CVTQS", 0x16, 0x0fc}, {"CVTQT", 0x16, 0x0be},
      {"CVTQT", 0x16, 0x0fe}, {"CVTTS", 0x16, 0x0ac}, {"CVTTS", 0x16, 0x0ec},
      {"CVTTS", 0x16, 0x1ac}, {"CVTTS", 0x16, 0x5ac}, {"CVTST", 0x16, 0x2ac},
      {"CVTST", 0x16, 0x6ac}, {"CVTTQ", 0x16, 0x02f}, {"CVTTQ", 0x16, 0x0af},
      {"CVTTQ", 0x16, 0x0ef}, {"CVTTQ", 0x16, 0x12f}, {"CVTTQ", 0x16, 0x1af},
      {"CVTTQ", 0x16, 0x1ef}, {"CVTTQ", 0x16, 0x52f}, {"CVTTQ", 0x16, 0x5af},
      {"CVTTQ", 0x16, 0x5ef}, {"SQRTS", 0x14, 0x08b}, {"SQRTS", 0x14, 0x0cb},
      {"SQRTS", 0x14, 0x18b}, {"SQRTS", 0x14, 0x58b}, {"SQRTT", 0x14, 0x0ab},
      {"SQRTT", 0x14, 0x0eb}, {"SQRTT", 0x14, 0x1ab}, {"SQRTT", 0x14, 0x5ab}};
  for (const auto &u : unary)
    vs.push_back({u.n, u.opc, u.f, false, 0, 0, 0, 0});

  auto dbits = [](double d) {
    u64 v;
    memcpy(&v, &d, 8);
    return v;
  };
  auto fbits = [&](float f) { return dbits((double)f); };
  std::vector<u64> fixed = {0,
                            U64(0x8000000000000000),
                            dbits(1.0),
                            dbits(-1.0),
                            dbits(2.0),
                            dbits(0.5),
                            dbits(3.0),
                            dbits(0.1),
                            dbits(1.0 / 3.0),
                            dbits(-2.5),
                            dbits(1.5),
                            dbits(2.5),
                            dbits(-0.5),
                            dbits(10.0),
                            dbits(1e10),
                            dbits(1e-10),
                            dbits(1e300),
                            dbits(-1e300),
                            dbits(1e-300),
                            dbits(1e-160),
                            dbits(1e160),
                            U64(0x7FEFFFFFFFFFFFFF),
                            U64(0x0010000000000000),
                            U64(0x0010000000000001),
                            U64(0x0000000000000001),
                            U64(0x800FFFFFFFFFFFFF),
                            U64(0x7FF0000000000000),
                            U64(0xFFF0000000000000),
                            U64(0x7FF8000000000000),
                            U64(0x7FF0000000000001),
                            U64(0xFFF8000000000001),
                            dbits(9007199254740992.0),
                            dbits(9007199254740994.0),
                            U64(0x43E0000000000000),
                            U64(0xC3E0000000000000),
                            dbits(18446744073709551616.0),
                            dbits(0.49999999999999994),
                            dbits(4503599627370495.5),
                            fbits(3.4028234663852886e38f),
                            dbits(6.8e38),
                            fbits(1.17549435e-38f),
                            fbits(1.4e-45f),
                            dbits(4.0e-46),
                            fbits(1e-40f),
                            dbits(16777217.0),
                            fbits(1.0000001192092896f),
                            dbits(1.00000000001),
                            fbits(0.1f),
                            fbits(-7.25f),
                            // MIN - 1/2 LSB as a product (with MIN) and as
                            // a T value below the S range: rounds to MIN
                            U64(0x3FEFFFFFFFFFFFFF),
                            fbits(0.99999994f),
                            U64(0x380FFFFFE0000000),
                            U64(1),
                            U64(2),
                            U64(0xFFFFFFFFFFFFFFFF),
                            U64(0x7FFFFFFFFFFFFFFF),
                            U64(0x0020000000000001),
                            U64(123456789),
                            U64(0x7FFFFFFFFFFFFE00),
                            U64(0x0000000100000001)};
  u64 seed = U64(0x243F6A8885A308D3);
  auto rnd = [&]() {
    u64 z = (seed += U64(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * U64(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * U64(0x94D049BB133111EB);
    return z ^ (z >> 31);
  };
  auto rval = [&]() -> u64 {
    const u64 r = rnd();
    const u64 sign = r & U64(0x8000000000000000);
    const u64 mant = rnd() & U64(0x000FFFFFFFFFFFFF);
    switch (r % 7) {
    case 0:
      return rnd(); // arbitrary bits
    case 1:
      return sign | ((rnd() % 64) << 52) | mant; // underflow / denormal edge
    case 2: {                                    // any single, widened
      u32 b32 = (u32)rnd();
      float f;
      memcpy(&f, &b32, 4);
      return fbits(f);
    }
    case 3:
      return sign | ((u64)(1023 - 80 + rnd() % 160) << 52) | mant; // ~1.0
    case 4:
      return sign | ((u64)(2047 - 64 + rnd() % 65) << 52) |
             mant; // overflow edge
    case 5:
      return (u64)((s64)rnd() >> (rnd() % 64)); // integer bits (CVTQx)
    default:
      return dbits((double)((s64)rnd() >> (rnd() % 64))); // integral doubles
    }
  };
  static const u64 N = U64(2) << 58, INE = U64(1) << 56;
  static const u64 fpcrs[] = {N,
                              N | INE,
                              INE,                       // DYN = chopped
                              INE | (U64(3) << 58),      // DYN = +inf
                              INE | (U64(1) << 58),      // DYN = -inf
                              N | (U64(0x3f) << 52),     // all sticky bits set
                              N | INE | (U64(1) << 48),  // DNZ
                              N | INE | (U64(7) << 60),  // UNDZ | UNFD | INED
                              N | INE | (U64(1) << 61),  // UNFD: denormals
                              N | INE | (U64(7) << 49)}; // INVD | DZED | OVFD

  const u8 *dram = (const u8 *)dram_ptr;
  const u64 page = (dram_size - 0x10000) & ~U64(0x1FFF);
  u8 saved[4];
  memcpy(saved, dram + page, 4);
  const u64 pc0 = page | 1, next = (page + 4) | 1;
  const u64 SENT = U64(0x5EA1ED0DDEADBEEF);
  u64 total_fail = 0, total_cases = 0;
  int printed = 0, skipped = 0;

  setvbuf(stdout, nullptr, _IOLBF, 0); // progress + failures as they happen
  for (Variant &v : vs) {
    const u32 fa = v.binary ? 1 : 31;
    const u32 ins = (v.opc << 26) | (fa << 21) | (2 << 16) | (v.func << 5) | 3;
    memcpy((u8 *)dram_ptr + page, &ins, 4);
    // Writing guest code behind the code-page map's back: say so, or the
    // flush below decides it has nothing to do and the next variant is
    // measured against this one's compiled block.
    if (m_code_map)
      m_code_map->note_write_all();
    flush_icache(); // drop the stale fetch line; bumps the JIT flush gen too
    CJitEngine::JitBlock *b =
        m_jit->record(pc0, page, 0, (uint8_t)state.cm, true, 1, dram);
    if (!b->compiled)
      m_jit->compile_block(
          b, dram, dram_size, (void *)&CAlphaCPU::jit_read,
          (void *)&CAlphaCPU::jit_write, (void *)&CAlphaCPU::jit_opcdec,
          (void *)&CAlphaCPU::jit_hw_mfpr, (void *)&CAlphaCPU::jit_read_phys,
          (void *)&CAlphaCPU::jit_hw_mtpr, (void *)&CAlphaCPU::jit_write_phys,
          (void *)&CAlphaCPU::jit_indirect, (void *)&CAlphaCPU::jit_read_locked,
          (void *)&CAlphaCPU::jit_stc, (void *)&CAlphaCPU::jit_misc,
          (void *)&CAlphaCPU::jit_read_vpte, (void *)&CAlphaCPU::jit_read_wchk,
          (void *)&CAlphaCPU::jit_itof, (void *)&CAlphaCPU::jit_ftoi,
          (void *)&CAlphaCPU::jit_fltl, (void *)&CAlphaCPU::jit_fp_read,
          (void *)&CAlphaCPU::jit_fp_write, (void *)&CAlphaCPU::jit_fltv);
    if (!b->code || b->prefix_len != 1) {
      skipped++; // classify() keeps this form interpreted
      continue;
    }

    auto run_case = [&](u64 a, u64 bv, u64 fpcr, bool fpen) {
      auto setup = [&]() {
        state.pc = pc0;
        state.fpen = fpen;
        state.fpcr = fpcr;
        state.exc_sum = U64(0x5A5A);
        state.f[1] = a;
        state.f[2] = bv;
        state.f[3] = SENT;
        state.f[31] = 0;
      };
      setup();
      execute();
      const u64 i_pc = state.pc, i_f3 = state.f[3], i_fpcr = state.fpcr,
                i_exc = state.exc_sum;
      setup();
      const u32 done = b->code(this, state.r);
      const u64 j_pc = state.pc, j_f3 = state.f[3], j_fpcr = state.fpcr,
                j_exc = state.exc_sum;
      const bool clean = i_pc == next && i_fpcr == fpcr;
      bool ok = false;
      if (done == 1) {
        ok = clean && j_pc == next && j_f3 == i_f3 && j_fpcr == fpcr &&
             j_exc == i_exc;
        v.native += ok;
      } else if (done == 0) {
        ok = j_pc == pc0 && j_f3 == SENT && j_fpcr == fpcr;
        v.bail += ok;
        v.overbail += ok && clean;
      }
      total_cases++;
      if (!ok) {
        v.fail++;
        total_fail++;
        if (printed++ < 40)
          printf("[JIT][FPTEST] FAIL %s func=%03x fa=%016llx fb=%016llx "
                 "fpcr=%016llx fpen=%d | interp pc=%llx f3=%016llx "
                 "fpcr=%016llx | jit done=%u pc=%llx f3=%016llx\n",
                 v.name, v.func, (unsigned long long)a, (unsigned long long)bv,
                 (unsigned long long)fpcr, (int)fpen, (unsigned long long)i_pc,
                 (unsigned long long)i_f3, (unsigned long long)i_fpcr, done,
                 (unsigned long long)j_pc, (unsigned long long)j_f3);
      }
    };

    printf("[JIT][FPTEST] running %s func=%03x\n", v.name, v.func);
    run_case(fixed[3], fixed[4], N | INE, false); // FP disabled: must bail
    for (u64 fpcr : fpcrs) {
      if (v.binary) {
        for (u64 a : fixed)
          for (u64 bv : fixed)
            run_case(a, bv, fpcr, true);
        for (int k = 0; k < 4000; ++k) {
          const u64 a = rval();
          run_case(a, rval(), fpcr, true);
        }
      } else {
        for (u64 bv : fixed)
          run_case(0, bv, fpcr, true);
        for (int k = 0; k < 20000; ++k)
          run_case(0, rval(), fpcr, true);
      }
    }
  }

  printf("[JIT][FPTEST] %-6s %4s %10s %10s %10s %6s\n", "op", "func", "native",
         "bail", "overbail", "fail");
  for (const Variant &v : vs)
    if (v.native || v.bail || v.fail)
      printf("[JIT][FPTEST] %-6s %03x  %10llu %10llu %10llu %6llu\n", v.name,
             v.func, (unsigned long long)v.native, (unsigned long long)v.bail,
             (unsigned long long)v.overbail, (unsigned long long)v.fail);
  printf("[JIT][FPTEST] %llu cases, %d interpreted-only forms, %llu failures: "
         "%s\n",
         (unsigned long long)total_cases, skipped,
         (unsigned long long)total_fail, total_fail ? "FAIL" : "PASS");
  memcpy((u8 *)dram_ptr + page, saved, 4);
  if (m_code_map)
    m_code_map->note_write_all(); // as above: this restores guest code
  flush_icache();
  fflush(stdout);
  std::_Exit(total_fail ? 1 : 0);
}
#endif // JIT_VERIFY
#endif
