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
#include "TraceEngine.hpp"
#include "cpu_debug.hpp"
#include "cpu_memory.hpp"
#include "cpu_misc.hpp"
#include "cpu_pal.hpp"
#include "diag_rpcc.hpp"
#include "lockstep.hpp"
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
  if (!state.check_int && !state.check_timers)
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

void CAlphaCPU::jit_run(int budget) {
  if (m_jit)
    m_jit->reclaim_if_pending(); // deferred code reclaim, here at a safe point
                                 // (no compiled frame live)
  // A link request from the previous batch's last chain is stale: interrupts
  // or the scheduler may have moved the PC since.
  m_link_from = nullptr;
  // Another CPU flushed its instruction cache (IC_FLUSH / IMB after loading
  // or changing code): drop this CPU's compiled blocks too. Lazy -- unchanged
  // blocks are re-hashed and kept.
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
  if (state.iProcNum == 0) {
    if (now >= next_timer_fire) {
      const u64 period_ns = theAli ? theAli->get_interval_period_ns() : 0;
      if (period_ns) {
        // Count-preserving, paced catch-up: the schedule advances one period
        // per fire so ticks lost to a busy/stalled CPU0 thread are repaid and
        // the guests' tick-counted clocks (VMS never resyncs) stay true to wall
        // time. Repayment is paced to >= half a period between fires (max 2x
        // nominal, never a burst - burst/compressed ticks skew RPCC-vs-tick
        // calibrations). Backlog beyond 1s (debugger pause, host sleep) is
        // dropped.
        // Repay gaps are modulated (tick_next_gap_ns).
        if (now - tick_last_fire >= std::chrono::nanoseconds(tick_gap_ns)) {
          cSystem->interrupt(-1, true);
          tick_last_fire = now;
          tick_gap_ns = tick_next_gap_ns(period_ns);
          next_timer_fire += std::chrono::nanoseconds(period_ns);
          if (now - next_timer_fire > std::chrono::seconds(1))
            next_timer_fire = now;
        }
      } else {
        cSystem->interrupt(-1, true);
        tick_last_fire = now;
        next_timer_fire = now + std::chrono::seconds(1);
      }
    }
  }

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
    while (budget-- > 0) {
      if (StopThread)
        return;
      execute();
    }
    return;
  }

  while (budget > 0) {
    const u64 start_virt = state.pc;
    const u32 start_asn = (u32)state.asn;

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
    if (start_virt & 1) // PALmode: physically addressed, always ASM
    {
      start_phys = start_virt & ~U64(1);
      start_asm = true;
    } else {
      // Fast path: icache hit-probe reads phys straight from the line,
      // read-only (no fill, no fault) -- get_icache's hit geometry minus the
      // side effects. Covers warm code.
      const int ici =
          (int)(((start_virt & ~U64(3)) >> 11) & (ICACHE_ENTRIES - 1));
      if (icache_enabled && state.icache[ici].valid &&
          (state.icache[ici].asn == state.asn || state.icache[ici].asm_bit) &&
          state.icache[ici].address == (start_virt & ICACHE_MATCH_MASK)) {
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
            if (state.check_int)
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

    // Side-effect-free exec virt -> live physical (icache probe, else FAKE
    // virt2phys). Re-resolves a trace segment's live mapping without a TB fill
    // / fault; mirrors the dispatch-top resolution above.
    auto live_exec_phys = [&](u64 v, u64 *op) -> bool {
      v &= ~U64(3); // strip the PALmode tag bit / align -- the physical is
                    // page-determined
      const int li = (int)((v >> 11) & (ICACHE_ENTRIES - 1));
      if (icache_enabled && state.icache[li].valid &&
          (state.icache[li].asn == state.asn || state.icache[li].asm_bit) &&
          state.icache[li].address == (v & ICACHE_MATCH_MASK)) {
        *op = state.icache[li].p_address + (v & ICACHE_BYTE_MASK);
        return true;
      }
      bool ad;
      return virt2phys(v, op, ACCESS_EXEC | FAKE, &ad, 0) == 0;
    };

    // Trace tier: gated lookup BEFORE the block cache. In non-debug release a
    // hot trace runs straight through and side-exits back here with state.pc
    // set, then `continue`; the block cache stays the universal fallback. Under
    // JIT_VERIFY the production run is gated OFF, traces still FORM (below),
    // but they're exercised + compared inside the block verify (next stage)
    // rather than driving execution unchecked.
#ifndef JIT_VERIFY
    // Same run-gates as the block path below: a compiled trace has no
    // per-instruction interrupt poll, so DON'T enter it while an
    // interrupt/timer is pending (the interpreter must service it, or the CPU
    // livelocks -> OS sanity-timer bugcheck), nor a PALmode trace without SDE,
    // nor one that would overrun the budget.
    auto trace_segs_live = [&](CJitEngine::TraceFragment *tf) -> bool {
      // The head's live phys is checked by trace_ok; re-resolve each INTERIOR
      // segment too. An interior page remapped to a different physical with
      // IDENTICAL bytes is invisible to the source hash.
      for (uint32_t s = 1; s < tf->n_segs; ++s) {
        u64 sp;
        if (!live_exec_phys(tf->segs[s].guest_pc, &sp) ||
            sp != tf->segs[s].phys_pc) {
          m_jit->note_trace_stale();
          return false;
        }
      }
      return true;
    };
    if (have_phys && m_jit->traces_enabled() && !state.check_int &&
        !state.check_timers) {
      CJitEngine::TraceFragment *t = m_jit->trace_lookup(start_virt, start_asn);
      if (t && (int)t->n_instr <= budget && (!(t->head_tag & 1) || state.sde) &&
          m_jit->trace_ok(t, start_phys, (const uint8_t *)dram_ptr) &&
          trace_segs_live(t)) {
        m_jit_budget = budget; // ceiling for the trace (its loads/bails honor
                               // it like a block)
#ifdef JIT_STATS
        const uint64_t _trace_t0 = jit_rdtsc();
#endif
        const u32 done = ((CJitEngine::JitFn)t->code)(this, &state.r[0]);
#ifdef JIT_STATS
        const uint64_t _trace_tsc = jit_rdtsc() - _trace_t0;
#endif
        state.r[31] = 0;
        break_seq_icache(); // the trace wrote state.pc natively; drop the stale
                            // cursor
        state.instruction_count += done;
        cc_large += (u64)done * cc_per_instruction;
        budget -= done;
#ifdef JIT_STATS
        cc_last_sync +=
            ns_to_host_ticks(m_jit->note_exec(done, 0, _trace_tsc, 0));
        m_jit->trace_entered();
        if (done < (u32)t->n_instr)
          m_jit->trace_exited(); // ran fewer than the trace's first-pass span
                                 // -> a side-exit/underrun
#endif
        if (done > 0)
          continue; // progress: re-dispatch at the trace's exit PC
      }
    }
#endif

    // Hot path: virtual+ASN lookup, phys-validated (skipped on a translation
    // miss).
    CJitEngine::JitBlock *b =
        have_phys ? m_jit->lookup(start_virt, start_asn, (uint8_t)state.cm)
                  : nullptr;
    if (have_phys && !b) // lazy-flushed survivor? hash-revalidate in place (no
                         // interpreted pass)
      b = m_jit->revalidate_flushed(start_virt, start_asn, (uint8_t)state.cm,
                                    start_phys, (const uint8_t *)dram_ptr);

    // A valid block whose phys no longer matches = a page remap the virtual key
    // can't see. The cold path below re-records it; log it -- it's the smoking
    // gun for stale-chain bugs.
    if (b && b->code && b->phys != start_phys) {
      static int n_stale = 0;
      if (n_stale++ < 20)
        printf("[JIT][CPU%d] DISPATCH STALE: pc=%016llx block_phys=%016llx "
               "live_phys=%016llx\n",
               (int)state.iProcNum, (unsigned long long)start_virt,
               (unsigned long long)b->phys, (unsigned long long)start_phys);
    }

#ifdef JIT_STATS
    int cold_reason = CJitEngine::CR_NO_BLOCK; // why we end up interpreting
#endif
    // Run the compiled safe prefix natively when available -- but not while an
    // interrupt or delayed timer is pending. Compiled blocks don't run the
    // per-instruction polls, so run the interpreter.
    if (b && b->code && b->phys == start_phys && (int)b->prefix_len <= budget &&
        !state.check_int && !state.check_timers &&
        (!(b->tag & 1) ||
         state.sde)) // PALmode block: its shadow-register remap assumes SDE
    {
      b->vgen = m_jit->vgen(); // phys validated + lookup proved flush-fresh:
                               // refresh the chain epoch
      // after a block has dispatched 8x, promote it to a single-block trace (a
      // future trace head). Dispatch-counted for now -- it undercounts chained
      // loops, but they still surface here when a chain breaks
      if (m_jit->traces_enabled() && ++b->hot == 8 &&
          !m_jit->trace_lookup(start_virt, start_asn)) {
        const CJitEngine::HelperSet hs = {(void *)&CAlphaCPU::jit_read,
                                          (void *)&CAlphaCPU::jit_write,
                                          (void *)&CAlphaCPU::jit_opcdec,
                                          (void *)&CAlphaCPU::jit_hw_mfpr,
                                          (void *)&CAlphaCPU::jit_read_phys,
                                          (void *)&CAlphaCPU::jit_hw_mtpr,
                                          (void *)&CAlphaCPU::jit_write_phys,
                                          (void *)&CAlphaCPU::jit_indirect,
                                          (void *)&CAlphaCPU::jit_read_locked,
                                          (void *)&CAlphaCPU::jit_stc,
                                          (void *)&CAlphaCPU::jit_misc,
                                          (void *)&CAlphaCPU::jit_read_vpte,
                                          (void *)&CAlphaCPU::jit_read_wchk,
                                          (void *)&CAlphaCPU::jit_itof,
                                          (void *)&CAlphaCPU::jit_ftoi,
                                          (void *)&CAlphaCPU::jit_fltl,
                                          (void *)&CAlphaCPU::jit_fp_read,
                                          (void *)&CAlphaCPU::jit_fp_write,
                                          (void *)&CAlphaCPU::jit_fltv};
        // Loop/superblock FUSION: follow each block's STATIC taken branch
        // target, growing the trace, until the chain branches back to the head
        // (compile_trace then closes the loop) or hits a non-branch / non-live
        // target / a non-head cycle / the segment cap. Works under verify too
        // (b->link is only set by production chaining).
        CJitEngine::JitBlock *blist[CJitEngine::kMaxTraceSegs] = {b};
        u32 nb = 1;
        for (CJitEngine::JitBlock *cur = b; nb < CJitEngine::kMaxTraceSegs;) {
          const u32 *aw = (const u32 *)((const u8 *)dram_ptr + cur->phys);
          const u32 lop = aw[cur->prefix_len - 1];
          const u32 lopc = lop >> 26;
          if (!(lopc == 0x30 || lopc == 0x34 || (lopc >= 0x38 && lopc <= 0x3f)))
            break; // not PC-relative
          const int64_t disp =
              (int64_t)((uint64_t)(lop & 0x1FFFFF) << 43) >> 43;
          const u64 spc =
              (((cur->tag & ~U64(1)) + 4 * (u64)(cur->prefix_len - 1)) + 4 +
               (u64)(disp * 4)) |
              (cur->tag & 1);
          if (spc == b->tag)
            break; // back-edge to the head -> compile_trace closes the loop
          CJitEngine::JitBlock *succ =
              m_jit->lookup(spc, start_asn, (uint8_t)state.cm);
          if (!succ || succ == b || !succ->code || succ->prefix_len == 0)
            break;
          u64 sp;
          if (!live_exec_phys(spc, &sp) || sp != succ->phys)
            break; // successor remapped since compile -> stale, don't fuse
          bool dup = false;
          for (u32 j = 0; j < nb; ++j)
            if (blist[j] == succ) {
              dup = true;
              break;
            }
          if (dup)
            break;
          blist[nb++] = succ;
          cur = succ;
        }
        m_jit->compile_trace(m_jit->trace_slot(start_virt), blist, nb,
                             (const uint8_t *)dram_ptr, dram_size, hs);
      }
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
      const u32 *vw = (const u32 *)((const u8 *)dram_ptr + b->phys);
      u32 vn = 0; // loads recorded for replay
      u32 sn = 0; // stores recorded for the compiled-pass compare
      u64 vpc = start_virt;
      u32 cur_len = b->prefix_len; // current interpreted block's length + start
                                   // tag; step 3
      u64 cur_tag = b->tag; // advances these per segment across a fused trace
      u32 n_interp = 0; // ops the interp ran over the trace's fused span (vs
                        // t->code's done)
      bool clean = true;
      CJitEngine::TraceFragment *vtr =
          m_jit->traces_enabled() ? m_jit->trace_lookup(start_virt, start_asn)
                                  : nullptr;
      if (vtr && !m_jit->trace_ok(vtr, start_phys, (const uint8_t *)dram_ptr))
        vtr = nullptr; // stale -> verify as a plain block
      const u32 nseg =
          vtr ? vtr->n_segs
              : 1; // interp the trace's full fused span; else just block b
      for (u32 seg = 0; seg < nseg && clean; ++seg) {
        if (vtr) {
          cur_len = vtr->segs[seg].n_instr;
          cur_tag = vtr->segs[seg].guest_pc;
          vw = (const u32 *)((const u8 *)dram_ptr + vtr->segs[seg].phys_pc);
          vpc = cur_tag;
        }
        for (u32 k = 0; k < cur_len; ++k) {
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
                opc == 0x2a || opc == 0x2b || opc == 0x0b || is_hwld ||
                is_isum || is_fpld) &&
               lra != 31) ||
              is_miscrd; // +LDBU/LDWU +LDx_L +LDQ_U +RPCC/RC/RS +ISUM +LDS/LDT
          u64 eva = 0;
          if (isld && !is_miscrd &&
              !is_isum) // misc/ISUM reads have no effective address -- only a
                        // logged value
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
                is_fpld
                    ? state.f[lra]
                    : state.r[RREG(
                          lra)]; // FP dest -> f[]; shadow-aware GPR otherwise
            vn++;
          }
          if (isst && sn < 64) {
            m_jit_slog_addr[sn] = sva;
            m_jit_slog_val[sn] = sval;
            m_jit_slog_success[sn] =
                is_sc
                    ? state.r[RREG(lra)]
                    : (u64)1; // STx_C result (post-execute); ordinary store = 1
            sn++;
          }
        }
        if (!clean)
          break; // fault/divergence in this segment -> stop (compare skipped)
        n_interp += cur_len; // this segment ran fully
        if (vtr && seg + 1 < nseg && state.pc != vtr->segs[seg + 1].guest_pc)
          break; // path left the fused trace -> side-exit
      } // end per-segment interp loop
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
        u64 jr[64]; // 64: a compiled PALmode block may touch the shadow bank
        memcpy(jr, snap, sizeof(jr));
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
        const u64 interp_pc =
            state.pc; // interpreter is authoritative for the PC
        const u32 n_stores_interp =
            sn; // interp's RECORDED store count (sn), NOT the replay cursor
                // m_jit_slog_i; a compiled pass must consume exactly this many
                // (catches a missing/extra store)
        m_jit_vreplay = true;
        m_jit_vlog_i = 0;
        m_jit_slog_i = 0;
        const u32 done =
            b->code(this, jr); // also writes state.pc (the JIT's next PC)
        m_jit_vreplay = false;
        if (!vtr && done == b->prefix_len) {
          if (state.pc != interp_pc) {
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
                   (unsigned long long)state.r[1], (unsigned long long)jr[1],
                   (unsigned long long)state.r[2], (unsigned long long)jr[2],
                   (unsigned long long)snap[5]);
          }
          u64 ipr_jit[27];
          cap_iprs(ipr_jit); // compiled pass wrote IPRs into live state; check
                             // vs interp
          for (int ii = 0; ii < 27; ii++)
            if (ipr_jit[ii] != ipr_interp[ii])
              printf("[JIT][VERIFY] IPR MISMATCH at %016llx slot %d: "
                     "interp=%016llx jit=%016llx\n",
                     (unsigned long long)start_virt, ii,
                     (unsigned long long)ipr_interp[ii],
                     (unsigned long long)ipr_jit[ii]);
          for (int fi = 0; fi < 64; fi++)
            if (state.f[fi] != f_interp[fi])
              printf("[JIT][VERIFY] FP MISMATCH at %016llx f%d: interp=%016llx "
                     "jit=%016llx\n",
                     (unsigned long long)start_virt, fi,
                     (unsigned long long)f_interp[fi],
                     (unsigned long long)state.f[fi]);
          if (m_jit_slog_i != n_stores_interp)
            printf("[JIT][VERIFY] STORE COUNT MISMATCH at %016llx: interp=%u "
                   "jit=%u\n",
                   (unsigned long long)start_virt, n_stores_interp,
                   m_jit_slog_i);
          cc_last_sync += ns_to_host_ticks(
              m_jit->verify_compare(start_virt, state.r, jr, vw,
                                    b->prefix_len) +
              g_diag_excluded_ns); // don't bill the verify progress-print OR
                                   // the PCI decode-off diag stall to the RPCC
          g_diag_excluded_ns =
              0; // consumed here in the verify path so jit_run's later sync
                 // doesn't double-count it
        }
        // verify extension: the production trace-run hook is gated off under
        // JIT_VERIFY, so exercise the trace HERE; re-restore pre-interp state,
        // run t->code on a 2nd scratch (replaying the same logged loads), then
        // compare to the authoritative interp. This makes the trace's own
        // state.pc write + frame visible to the differential harness.
        if (m_jit->traces_enabled()) {
          CJitEngine::TraceFragment *tr =
              m_jit->trace_lookup(start_virt, start_asn);
          if (tr &&
              m_jit->trace_ok(tr, start_phys, (const uint8_t *)dram_ptr) &&
              [&] {
                for (uint32_t s = 1; s < tr->n_segs; ++s) {
                  u64 sp;
                  if (!live_exec_phys(tr->segs[s].guest_pc, &sp) ||
                      sp != tr->segs[s].phys_pc)
                    return false;
                }
                return true;
              }()) { // verify now mirrors the production hook's interior
                     // live-phys check
            memcpy(state.f, f_pre, sizeof(f_pre));
            state.sde = ictl_sde_pre;
            state.hwe = ictl_hwe_pre;
            state.i_ctl_spe = ictl_spe_pre;
            state.i_ctl_va_mode = ictl_vam_pre;
            state.i_ctl_vptb = ictl_vptb_pre;
            state.i_ctl_other = ictl_other_pre;
            state.cm = cm_pre;
            dpc_context_changed();
            state.sir = sir_pre;
            state.aster = aster_pre;
            state.astrr = astrr_pre;
            state.fpen = fpen_pre;
            state.ppcen = ppcen_pre;
            u64 jr_t[64];
            memcpy(jr_t, snap, sizeof(jr_t));
            m_jit_vreplay = true;
            m_jit_vlog_i = 0;
            m_jit_slog_i = 0;
            const u32 done_t = ((CJitEngine::JitFn)tr->code)(this, jr_t);
            m_jit_vreplay = false;
            if (done_t != n_interp)
              printf("[JIT][VERIFY] TRACE COUNT MISMATCH at %016llx: "
                     "interp_span=%u trace_done=%u\n",
                     (unsigned long long)start_virt, n_interp, done_t);
            if (done_t == n_interp) {
              if (state.pc != interp_pc)
                printf("[JIT][VERIFY] TRACE PC MISMATCH at %016llx: "
                       "interp=%016llx trace=%016llx (n=%u)\n",
                       (unsigned long long)start_virt,
                       (unsigned long long)interp_pc,
                       (unsigned long long)state.pc, n_interp);
              u64 ipr_jit_t[27];
              cap_iprs(ipr_jit_t); // trace wrote IPRs into live state; check vs
                                   // interp (parity with the block path)
              for (int ii = 0; ii < 27; ii++)
                if (ipr_jit_t[ii] != ipr_interp[ii])
                  printf("[JIT][VERIFY] TRACE IPR MISMATCH at %016llx slot %d: "
                         "interp=%016llx trace=%016llx\n",
                         (unsigned long long)start_virt, ii,
                         (unsigned long long)ipr_interp[ii],
                         (unsigned long long)ipr_jit_t[ii]);
              for (int fi = 0; fi < 64; fi++)
                if (state.f[fi] != f_interp[fi])
                  printf("[JIT][VERIFY] TRACE FP MISMATCH at %016llx f%d: "
                         "interp=%016llx trace=%016llx\n",
                         (unsigned long long)start_virt, fi,
                         (unsigned long long)f_interp[fi],
                         (unsigned long long)state.f[fi]);
              if (m_jit_slog_i != n_stores_interp)
                printf("[JIT][VERIFY] TRACE STORE COUNT MISMATCH at %016llx: "
                       "interp=%u trace=%u\n",
                       (unsigned long long)start_virt, n_stores_interp,
                       m_jit_slog_i);
              m_jit->verify_compare(start_virt, state.r, jr_t, vw, n_interp);
            }
          }
        }
        state.pc =
            interp_pc; // restore; the block's PC write was only for the check
        put_iprs(ipr_interp); // roll back the compiled pass's live IPR writes
                              // (verify-only)
        memcpy(state.f, f_interp, sizeof(f_interp)); // ...and its FP writes
        break_seq_icache(); // compiled pass + raw pc restore bypassed set_pc
      }
      continue;
#else
      // A predecessor block's epilogue cache-missed and asked to be linked
      // here. Now that we know this block is live and compiled, patch its
      // successor pointer so it jumps straight in instead of returning. Never
      // link into a learned idle/parked loop head: idle pacing (above) only
      // sees passes that come back through here.
      if (m_link_from && (start_virt == m_idle_pc || start_virt == m_park_pc)) {
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
      // PC). Account for the compiled ops: instruction count (+ cc_large for
      // the legacy speed calibration). state.cc (RPCC) is no longer advanced
      // per-instruction. wall-clock * cpu_hz at the jit_run boundary above, so
      // a hot compiled loop can't run the cycle counter ahead of real time
      state.instruction_count += done;
      cc_large += (u64)done * cc_per_instruction;
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
    while (budget > 0) {
      execute();
      --budget;
      ++n;
      expected += 4;
      if (state.pc != expected)
        break;
    }
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
        if (line.valid && (line.asn == state.asn || line.asm_bit) &&
            line.address == ((v | (start_virt & 1)) & ICACHE_MATCH_MASK)) {
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
    // Record only translatable block starts (a translation miss left have_phys
    // false).
    if (have_phys && !src_stale && state.pc != expected) {
      CJitEngine::JitBlock *nb =
          m_jit->record(start_virt, start_phys, start_asn, (uint8_t)state.cm,
                        start_asm, n, (const uint8_t *)dram_ptr);
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
  // virtual-alt forms); plain loads/stores pass the bare size.
  const int cm = (size_bits & 0x100) ? cpu->state.alt_cm : cpu->state.cm;
  size_bits &= 0xff;
  const u64 amask = (u64)(size_bits / 8) - 1;
  cpu->m_jit->note_helper(CJitEngine::HK_READ);
  HELPER_TIMER(cpu, CJitEngine::HK_READ);
  if (va & amask) { // unaligned: let the interpreter handle it
    cpu->m_jit->note_bail(false, CJitEngine::BK_UNALIGNED);
    return 1;
  }

  u64 phys;
  const u64 vp = va & ~U64(0x1FFF);
  SDataPageCache &dpc =
      cpu->data_page_cache[0][dpc_index(va)]; // direct-mapped by virt page
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
    cpu->m_jit->note_dpc_other(0, unsigned(dpc_index(va)), vp, dpc.virt_page);
  if (dpc.valid && dpc.virt_page == vp && dpc.cm == cm &&
      dpc.asn == cpu->state.asn0) {
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else {
    // Side-effect-free TB fast path. NOT virt2phys - that walks the page table
    // and vectors faults as a side effect, which (re-done by the interpreter
    // after we bail) corrupts state. Bail on a TB miss or any access fault and
    // let the interpreter do the side-effect translation ~  it fills this cache
    // so the next compiled run hits. Mirrors virt2phys's read path.
    const int spe = cpu->jit_spe_data(va, cm, &phys);
    if (spe < 0) { // bad VA form / superpage outside kernel mode (ACV)
      cpu->m_jit->note_bail(false, CJitEngine::BK_ACV);
      return 1;
    }
    if (!spe) {
      const int i = cpu->FindTBEntry(va, ACCESS_READ);
      if (i < 0) { // TB miss
        cpu->m_jit->note_bail(false, CJitEngine::BK_TB_MISS);
        return 1;
      }
      const auto &e = cpu->state.tb[TB_INDEX_DATA][i];
      if (!e.access[0][cm]) { // protection (ACV)
        cpu->m_jit->note_bail(false, CJitEngine::BK_ACV);
        return 1;
      }
      if (e.fault[0]) { // fault-on-read (FOR)
        cpu->m_jit->note_bail(false, CJitEngine::BK_FAULT);
        return 1;
      }
      phys = e.phys | (va & e.keep_mask);
    }
    dpc.fill(vp, phys & ~U64(0x1FFF),
             cpu->dpc_host_base(phys),
             cm, cpu->state.asn0);
  }

  // DRAM only: bail on MMIO so the interpreter does device reads (side effects
  // + ordering). This MUST precede the verify replay: production bails here,
  // and a device byte/word read is NOT size-truncated (LDBU/LDWU use READ_VIRT,
  // no sext func), so replaying an MMIO value and then re-truncating it with
  // movzx would falsely mismatch the interpreter. Bailing -> the compiled block
  // stops at the load (done < prefix_len) and the verify skips the compare,
  // matching prod.
  if (phys >= cpu->dram_size) {
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
      static int n = 0;
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
      static int n = 0;
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

// JIT FLTL non-arithmetic helper (static). FPCR moves, sign-copies, FP
// conditional moves and the CVTLQ/CVTQL bit rearrangements -- mirrored verbatim
// from cpu_fp_operate.h; classify only routes these funcs here (no FP math, no
// /V trap forms). Returns 1 = FEN-trap bail.
int CAlphaCPU::jit_fltl(CAlphaCPU *cpu, u32 ins) {
  if (cpu->state.fpen == 0)
    return 1;             // FEN trap: the interpreter vectors it
  cpu->state.exc_sum = 0; // FPSTART
  u64 *f = cpu->state.f;
  const u32 fa = (ins >> 21) & 0x1f, fb = (ins >> 16) & 0x1f, fc = ins & 0x1f;
  switch ((ins >> 5) & 0x7ff) {
  case 0x010: // CVTLQ
    f[fc] = sext_u64_32(((f[fb] >> 32) & 0xC0000000) |
                        ((f[fb] >> 29) & 0x3FFFFFFF));
    break;
  case 0x020:
    f[fc] = (f[fa] & FPR_SIGN) | (f[fb] & ~FPR_SIGN);
    break; // CPYS
  case 0x021:
    f[fc] = ((f[fa] & FPR_SIGN) ^ FPR_SIGN) | (f[fb] & ~FPR_SIGN);
    break;    // CPYSN
  case 0x022: // CPYSE
    f[fc] = (f[fa] & (FPR_SIGN | FPR_EXP)) | (f[fb] & ~(FPR_SIGN | FPR_EXP));
    break;
  case 0x024:
    cpu->write_fpcr_arch(f[fa]);
    break; // MT_FPCR
  case 0x025:
    f[fa] = cpu->read_fpcr_arch();
    break; // MF_FPCR (dest = Fa)
  case 0x02a:
    if ((f[fa] & ~FPR_SIGN) == 0)
      f[fc] = f[fb];
    break; // FCMOVEQ
  case 0x02b:
    if ((f[fa] & ~FPR_SIGN) != 0)
      f[fc] = f[fb];
    break; // FCMOVNE
  case 0x02c:
    if ((f[fa] & FPR_SIGN) && (f[fa] & ~FPR_SIGN) != 0)
      f[fc] = f[fb];
    break; // FCMOVLT
  case 0x02d:
    if (!(f[fa] & FPR_SIGN) || (f[fa] & ~FPR_SIGN) == 0)
      f[fc] = f[fb];
    break; // FCMOVGE
  case 0x02e:
    if ((f[fa] & FPR_SIGN) || (f[fa] & ~FPR_SIGN) == 0)
      f[fc] = f[fb];
    break; // FCMOVLE
  case 0x02f:
    if (!(f[fa] & FPR_SIGN) && (f[fa] & ~FPR_SIGN) != 0)
      f[fc] = f[fb];
    break;      // FCMOVGT
  case 0x030: { // CVTQL (no /V form)
    u64 cvtql_src = f[fb];
    f[fc] = ((cvtql_src & U64(0xC0000000)) << 32) |
            ((cvtql_src & U64(0x3FFFFFFF)) << 29);
    if (FPR_GETSIGN(cvtql_src) ? (cvtql_src < U64(0xFFFFFFFF80000000))
                               : (cvtql_src > U64(0x000000007FFFFFFF)))
      cpu->write_fpcr_arch(cpu->state.fpcr | FPCR_IOV);
    break;
  }
  }
  return 0;
}

// JIT VAX-FP arithmetic helper (static). FLTV (0x15): mirrors the interpreter's
// vax_* dispatch into f[fc]. FPSTART bail (return 1) if FP disabled. Returns 2
// when the op delivered an arith trap: vax_trap GO_PALs (sets state.pc +
// exc_sum), so exc_sum != 0 detects it and the caller honors state.pc -- the op
// runs exactly once (no re-execute). Returns 0 on the common no-trap path.
// Fc==31 is gated out in classify.
int CAlphaCPU::jit_fltv(CAlphaCPU *cpu, u32 ins) {
  if (cpu->state.fpen == 0)
    return 1; // FEN trap (FPSTART): op not run
  cpu->state.exc_sum = 0;
  u64 *f = cpu->state.f;
  const u32 fa = (ins >> 21) & 0x1f, fb = (ins >> 16) & 0x1f, fc = ins & 0x1f;
  const u32 fn = (ins >> 5) & 0x7ff;
  UFP u;
  switch (fn) {
  case 0x0a5:
  case 0x4a5:
    f[fc] =
        (cpu->vax_fcmp(f[fa], f[fb], ins) == 0) ? U64(0x4000000000000000) : 0;
    break; // CMPGEQ
  case 0x0a6:
  case 0x4a6:
    f[fc] =
        (cpu->vax_fcmp(f[fa], f[fb], ins) < 0) ? U64(0x4000000000000000) : 0;
    break; // CMPGLT
  case 0x0a7:
  case 0x4a7:
    f[fc] =
        (cpu->vax_fcmp(f[fa], f[fb], ins) <= 0) ? U64(0x4000000000000000) : 0;
    break; // CMPGLE
  case 0x03c:
  case 0x0bc:
    f[fc] = cpu->vax_cvtif(f[fb], ins, DT_F);
    break; // CVTQF
  case 0x03e:
  case 0x0be:
    f[fc] = cpu->vax_cvtif(f[fb], ins, DT_G);
    break; // CVTQG
  default:
    switch (fn & 0x7f) {
    case 0x000:
      f[fc] = cpu->vax_fadd(f[fa], f[fb], ins, DT_F, false);
      break; // ADDF
    case 0x001:
      f[fc] = cpu->vax_fadd(f[fa], f[fb], ins, DT_F, true);
      break; // SUBF
    case 0x002:
      f[fc] = cpu->vax_fmul(f[fa], f[fb], ins, DT_F);
      break; // MULF
    case 0x003:
      f[fc] = cpu->vax_fdiv(f[fa], f[fb], ins, DT_F);
      break; // DIVF
    case 0x01e:
      cpu->vax_unpack_d(f[fb], &u, ins);
      f[fc] = cpu->vax_rpack(&u, ins, DT_G);
      break; // CVTDG
    case 0x020:
      f[fc] = cpu->vax_fadd(f[fa], f[fb], ins, DT_G, false);
      break; // ADDG
    case 0x021:
      f[fc] = cpu->vax_fadd(f[fa], f[fb], ins, DT_G, true);
      break; // SUBG
    case 0x022:
      f[fc] = cpu->vax_fmul(f[fa], f[fb], ins, DT_G);
      break; // MULG
    case 0x023:
      f[fc] = cpu->vax_fdiv(f[fa], f[fb], ins, DT_G);
      break; // DIVG
    case 0x02c:
      cpu->vax_unpack(f[fb], &u, ins);
      f[fc] = cpu->vax_rpack(&u, ins, DT_F);
      break; // CVTGF
    case 0x02d:
      cpu->vax_unpack(f[fb], &u, ins);
      f[fc] = cpu->vax_rpack_d(&u, ins);
      break; // CVTGD
    case 0x02f:
      f[fc] = cpu->vax_cvtfi(f[fb], ins);
      break; // CVTGQ
    }
  }
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
  SDataPageCache &dpc = cpu->data_page_cache[0][dpc_index(va)];
  if (dpc.valid && dpc.virt_page == vp && dpc.cm == cpu->state.cm &&
      dpc.asn == cpu->state.asn0) {
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else {
    const int spe = cpu->jit_spe_data(va, cpu->state.cm, &phys);
    if (spe < 0)
      return 1; // bad VA form / superpage outside kernel mode (ACV)
    if (!spe) {
      const int i = cpu->FindTBEntry(va, ACCESS_READ);
      if (i < 0)
        return 1; // TB miss
      const auto &e = cpu->state.tb[TB_INDEX_DATA][i];
      if (!e.access[0][cpu->state.cm])
        return 1; // protection (ACV)
      if (e.fault[0])
        return 1; // fault-on-read (FOR)
      phys = e.phys | (va & e.keep_mask);
    }
    dpc.fill(vp, phys & ~U64(0x1FFF),
             cpu->dpc_host_base(phys),
             cpu->state.cm, cpu->state.asn0);
  }

  if (phys >=
      cpu->dram_size) // MMIO: interpreter handles the I/O-space locked path
    return 1;

  CSystem::CLLSCDRAMGuard llsc_guard(cpu->cSystem, true);
  if (cpu->m_jit_vreplay) {
    if (va != cpu->m_jit_vaddr[cpu->m_jit_vlog_i]) {
      static int n = 0;
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

  const int i = cpu->FindTBEntry(va, ACCESS_READ);
  if (i < 0)
    return 1; // TB miss (double miss)
  const auto &e = cpu->state.tb[TB_INDEX_DATA][i];
  if (!e.access[0][0])
    return 1; // protection: kernel read access
  if (e.fault[0])
    return 1; // fault-on-read (FOR)
  const u64 phys = e.phys | (va & e.keep_mask);

  if (phys >= cpu->dram_size) // MMIO: bail before the replay (mirrors jit_read)
    return 1;

  if (cpu->m_jit_vreplay) {
    if (va != cpu->m_jit_vaddr[cpu->m_jit_vlog_i]) {
      static int n = 0;
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
  const int spe = cpu->jit_spe_data(va, cm, &phys);
  if (spe < 0)
    return 1; // bad VA form / superpage outside kernel mode (ACV)
  if (!spe) {
    const int i = cpu->FindTBEntry(va, ACCESS_READ);
    if (i < 0)
      return 1; // TB miss
    const auto &e = cpu->state.tb[TB_INDEX_DATA][i];
    if (!e.access[0][cm])
      return 1; // no read access (ACV)
    if (!e.access[1][cm])
      return 1; // no write access -- WrChk fails (ACV)
    if (e.fault[0] || e.fault[1])
      return 1; // FOR/FOW: bail so the interpreter vectors the fault
    phys = e.phys | (va & e.keep_mask);
  }

  if (phys >=
      cpu->dram_size) // MMIO: bail before the replay (mirrors jit_read_vpte)
    return 1;

  if (cpu->m_jit_vreplay) {
    if (va != cpu->m_jit_vaddr[cpu->m_jit_vlog_i]) {
      static int n = 0;
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
      static int n = 0;
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
      static int n = 0;
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
      cpu->data_page_cache[1][dpc_index(va)]; // direct-mapped by virt page
  if (dpc.valid && dpc.virt_page != vp)
    cpu->m_jit->note_dpc_other(1, unsigned(dpc_index(va)), vp, dpc.virt_page);
  if (dpc.valid && dpc.virt_page == vp && dpc.cm == cm &&
      dpc.asn == cpu->state.asn0) {
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else {
    // Side-effect-free TB fast path on the write cache [1]; bail on a TB miss
    // or access fault so the interpreter does the side-effecting translation
    // (filling this cache, so the next compiled run hits). NOT virt2phys -- it
    // vectors faults as a side effect.
    const int spe = cpu->jit_spe_data(va, cm, &phys);
    if (spe < 0) { // bad VA form / superpage outside kernel mode (ACV)
      cpu->m_jit->note_bail(true, CJitEngine::BK_ACV);
      return 1;
    }
    if (!spe) {
      const int i = cpu->FindTBEntry(va, ACCESS_WRITE);
      if (i < 0) { // TB miss
        cpu->m_jit->note_bail(true, CJitEngine::BK_TB_MISS);
        return 1;
      }
      const auto &e = cpu->state.tb[TB_INDEX_DATA][i];
      if (!e.access[1][cm]) { // protection (ACV)
        cpu->m_jit->note_bail(true, CJitEngine::BK_ACV);
        return 1;
      }
      if (e.fault[1]) { // fault-on-write (FOW)
        cpu->m_jit->note_bail(true, CJitEngine::BK_FAULT);
        return 1;
      }
      phys = e.phys | (va & e.keep_mask);
    }
    dpc.fill(vp, phys & ~U64(0x1FFF), cpu->dpc_host_base_w(phys), cm,
             cpu->state.asn0);
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
      static int n = 0;
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
      static int n = 0;
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
  SDataPageCache &dpc = cpu->data_page_cache[1][dpc_index(va)];
  if (dpc.valid && dpc.virt_page == vp && dpc.cm == cpu->state.cm &&
      dpc.asn == cpu->state.asn0) {
    phys = dpc.phys_base | (va & U64(0x1FFF));
  } else {
    const int spe = cpu->jit_spe_data(va, cpu->state.cm, &phys);
    if (spe < 0)
      return U64(0x100); // bad VA form / superpage outside kernel mode (ACV)
    if (!spe) {
      const int i = cpu->FindTBEntry(va, ACCESS_WRITE);
      if (i < 0)
        return U64(0x100); // TB miss
      const auto &e = cpu->state.tb[TB_INDEX_DATA][i];
      if (!e.access[1][cpu->state.cm])
        return U64(0x100); // protection (ACV)
      if (e.fault[1])
        return U64(0x100); // fault-on-write (FOW)
      phys = e.phys | (va & e.keep_mask);
    }
    dpc.fill(vp, phys & ~U64(0x1FFF), cpu->dpc_host_base_w(phys), cpu->state.cm,
             cpu->state.asn0);
  }

  // Shared LL/SC path: consumes the reservation, applies the ABA sequence
  // guard, then CASes RAM or does the MMIO conditional store.
  CSystem::CLLSCDRAMGuard llsc_guard(cpu->cSystem, phys < cpu->dram_size);
  return cpu->cSystem->cpu_stx_c(cpu->state.iProcNum, phys, size_bits, value,
                                 cpu->dram_ptr, cpu->dram_size, cpu);
}

/* CALL_PAL OPCDEC trap: a privileged function (< 0x40) attempted in user mode.
   Mirrors GO_PAL(OPCDEC) -- save the faulting PC in EXC_ADDR, vector to the
   PALcode OPCDEC entry, and clear the load-lock flag */
void CAlphaCPU::jit_opcdec(CAlphaCPU *cpu, u64 cpc) {
  cpu->state.exc_addr = cpc;
  cpu->set_pc(cpu->state.pal_base | OPCDEC | U64(1));
  cpu->cSystem->cpu_clear_lock(cpu->state.iProcNum);
}

/* HW_MFPR (PALmode): return the IPR selected by (ins>>8)&0xff. */
u64 CAlphaCPU::jit_hw_mfpr(CAlphaCPU *cpu, u32 ins, u64 cur) {
  cpu->m_jit->note_helper(CJitEngine::HK_MFPR);
  HELPER_TIMER(cpu, CJitEngine::HK_MFPR);
  const auto &state = cpu->state;
  const u32 function = (ins >> 8) & 0xff;

  // ISUM (0x0d) reads the live async interrupt-request lines (eir/slr/crr/pcr),
  // which the differential verify can't re-derive - changes between the interp
  // and compiled passes. Replay the interp value here
  if (cpu->m_jit_vreplay && function == 0x0d)
    return cpu->m_jit_vlog[cpu->m_jit_vlog_i++];

  if ((function & 0xc0) == 0x40) // PCTX
    return ((u64)state.asn << 39) | ((u64)state.astrr << 9) |
           ((u64)state.aster << 5) | (state.fpen ? U64(0x1) << 2 : 0) |
           (state.ppcen ? U64(0x1) << 1 : 0);

  switch (function) {
  case 0x05:
    return state.pmpc; // PMPC
  case 0x06:
    return state.exc_addr; // EXC_ADDR
  case 0x07:
    return cpu->va_form(state.exc_addr, true); // IVA_FORM
  case 0x08:
  case 0x09:
  case 0x0a:
  case 0x0b: // IER_CM / CM / IER
    return (((u64)state.eien) << 33) | (((u64)state.slen) << 32) |
           (((u64)state.cren) << 31) | (((u64)state.pcen) << 29) |
           (((u64)state.sien) << 13) | (((u64)state.asten) << 13) |
           (((u64)state.cm) << 3);
  case 0x0c:
    return ((u64)state.sir) << 13; // SIRR
  case 0x0d: {                     // ISUM (production path: read the live async
    // interrupt-request lines; the verify replays via the m_jit_vreplay
    // short-circuit at the top).
    const u64 isum = (((u64)(state.eir & state.eien)) << 33) |
                     (((u64)(state.slr & state.slen)) << 32) |
                     (((u64)(state.crr & state.cren)) << 31) |
                     (((u64)(state.pcr & state.pcen)) << 29) |
                     (((u64)(state.sir & state.sien)) << 13) |
                     (((u64)(((U64(0x1) << (state.cm + 1)) - 1) & state.aster &
                             state.astrr & (state.asten * 0x3)))
                      << 3) |
                     (((u64)(((U64(0x1) << (state.cm + 1)) - 1) & state.aster &
                             state.astrr & (state.asten * 0xc)))
                      << 7);
    cpu->irq_trace_ipr("jISUM", function, isum);
    return isum;
  }
  case 0x0f:
    return state.exc_sum; // EXC_SUM
  case 0x10:
    return state.pal_base; // PAL_BASE
  case 0x11:               // I_CTL
    return state.i_ctl_other | (((u64)cpu->m_model->chip_id) << 24) |
           (u64)state.i_ctl_vptb | (((u64)state.i_ctl_va_mode) << 15) |
           (state.hwe ? U64(0x1) << 12 : 0) | (state.sde ? U64(0x1) << 7 : 0) |
           (((u64)state.i_ctl_spe) << 3);
  case 0x14:
    return state.pctr_ctl; // PCTR_CTL
  case 0x16:
    return state.i_stat; // I_STAT
  case 0x27:
    return state.mm_stat; // MM_STAT
  case 0x2a:
    return state.dc_stat; // DC_STAT
  case 0x2b:
    return 0; // C_DATA
  case 0xc0:
    return (((u64)state.cc_offset) << 32) | (state.cc & U64(0xffffffff)); // CC
  case 0xc2:
    return state.fault_va; // VA
  case 0xc3:
    return cpu->va_form(state.va_form_va, false); // VA_FORM
  }
  (void)cur;
  return 0; // unknown IPR: read-zero, matching DO_HW_MFPR (classify() never
            // compiles these)
}

/* HW_MTPR (PALmode): the IPR write selected by `function` (value = Rb). Mirrors
 * DO_HW_MTPR (cpu_pal.h) verbatim. Pure stores are verify-compared via the IPR
 * snapshot; the TB fills forward to add_tb_i/_d (idempotent, so the verify
 * double-run is safe); the check_int=true kick (IER/CM/SIRR/AST) can only force
 * an interrupt poll, never suppress one. The 0x40-7f ASN write (bit 0) is
 * excluded -- classify() never compiles it (it flushes the dpc + bumps the asn
 * epoch). */
void CAlphaCPU::jit_hw_mtpr(CAlphaCPU *cpu, u32 function, u64 value) {
  cpu->m_jit->note_helper(CJitEngine::HK_MTPR);
  HELPER_TIMER(cpu, CJitEngine::HK_MTPR);
  cpu->m_jit->note_mtpr(function);
  // 0x40-0x7f bitmask group: ASTER/ASTRR/PPCEN/FPEN field stores (+check_int
  // for the AST bits). The ASN write (bit 0, dpc flush + asn-epoch bump) is
  // never compiled -- classify() routes it to OP_NONE.
  if ((function & 0xc0) == 0x40) {
    if (function & 2) {
      cpu->state.aster = (int)(value >> 5) & 0xf;
      if (cpu->int_deliverable())
        cpu->state.check_int = true;
    }
    if (function & 4) {
      cpu->state.astrr = (int)(value >> 9) & 0xf;
      if (cpu->int_deliverable())
        cpu->state.check_int = true;
    }
    if (function & 8)
      cpu->state.ppcen = (int)(value >> 1) & 1;
    if (function & 16)
      cpu->state.fpen = (int)(value >> 2) & 1;
    return;
  }
  switch (function) {
  case 0x00:
    cpu->state.last_tb_virt = value;
    break; // ITB_TAG
  case 0x01:
    cpu->add_tb_i(cpu->state.last_tb_virt, value);
    break; // ITB_PTE (ITB fill)
  case 0x02:
    cpu->tbiap(ACCESS_EXEC);
    break; // ITB_IAP (process ITB invalidate)
  case 0x03:
    cpu->tbia(ACCESS_EXEC);
    break; // ITB_IA (invalidate all ITB)
  case 0x04:
    cpu->tbis(value, ACCESS_EXEC);
    break; // ITB_IS (single ITB invalidate)
  case 0x13:
    cpu->flush_icache();
    break;   // IC_FLUSH (lazy flush + deferred reclaim)
  case 0x0e: // HW_INT_CLR
    cpu->state.pcr &= ~((value >> 29) & U64(0x3));
    cpu->state.crr &= ~((value >> 31) & U64(0x1));
    cpu->state.slr &= ~((value >> 32) & U64(0x1));
    break;
  case 0x24: // DTB_IS0
    cpu->tbis_d(value, cpu->state.asn0);
    break;
  case 0xa4: // DTB_IS1
    cpu->tbis_d(value, cpu->state.asn1);
    break;
  case 0xa3: // DTB_IA
    cpu->tbia(ACCESS_READ);
    break;
  case 0x25: // DTB_ASN0
    cpu->state.asn0 = (int)(value >> 56);
    cpu->dpc_context_changed();
    cpu->flush_data_page_cache();
    break;
  case 0xa5: // DTB_ASN1
    cpu->state.asn1 = (int)(value >> 56);
    cpu->flush_data_page_cache();
    break;
  case 0x09: // CM (current mode)
    cpu->state.cm = (int)(value >> 3) & 3;
    cpu->dpc_context_changed();
    if (cpu->int_deliverable())
      cpu->state.check_int = true;
    cpu->irq_trace_ipr("jMTPR", function, value);
    break;
  case 0x0b: // IER_CM: write CM, then fall into IER
    cpu->state.cm = (int)(value >> 3) & 3;
    cpu->dpc_context_changed();
    if (cpu->int_deliverable())
      cpu->state.check_int = true;
    [[fallthrough]];
  case 0x0a: // IER
    cpu->state.asten = (int)(value >> 13) & 1;
    cpu->state.sien = (int)(value >> 13) & 0xfffe;
    cpu->state.pcen = (int)(value >> 29) & 3;
    cpu->state.cren = (int)(value >> 31) & 1;
    cpu->state.slen = (int)(value >> 32) & 1;
    cpu->state.eien = (int)(value >> 33) & 0x3f;
    if (cpu->int_deliverable())
      cpu->state.check_int = true; // newly enabled pending ints must be polled
    cpu->irq_trace_ipr("jMTPR", function, value);
    break;
  case 0x0c: // SIRR (software interrupt request)
    cpu->state.sir = (int)(value >> 13) & 0xfffe;
    if (cpu->int_deliverable())
      cpu->state.check_int = true;
    cpu->irq_trace_ipr("jMTPR", function, value);
    break;
  case 0x11: // I_CTL (terminator; mirrors DO_HW_MTPR)
    cpu->state.i_ctl_other =
        (value & U64(0x00000000006e2f67)) |
        U64(0x0000000000100000); // bit 20 hardwired-on (EV6/EV68)
    cpu->state.i_ctl_vptb = sext_u64_48(value & U64(0x0000ffffc0000000));
    cpu->state.i_ctl_spe = (int)((value >> 3) & 7);
    cpu->state.sde = (value >> 7) & 1;
    cpu->state.hwe = (value >> 12) & 1;
    cpu->state.i_ctl_va_mode = (int)(value >> 15) & 3;
    break;
  case 0x14:
    cpu->state.pctr_ctl = value & U64(0xffffffffffffffdf);
    break; // PCTR_CTL
  case 0x20:
    cpu->last_dtb_virt[0] = value;
    break; // DTB_TAG0
  case 0x21:
    cpu->add_tb_d(cpu->last_dtb_virt[0], value, 0);
    break; // DTB_PTE0 (DTB fill)
  case 0x26:
    cpu->state.alt_cm = (int)(value & 3);
    break; // DTB_ALTMODE
  case 0x29:
    cpu->state.dc_ctl = value;
    break; // DC_CTL
  case 0xa0:
    cpu->last_dtb_virt[1] = value;
    break; // DTB_TAG1
  case 0xa1:
    cpu->add_tb_d(cpu->last_dtb_virt[1], value, 1);
    break; // DTB_PTE1 (DTB fill)
  case 0xc0:
    cpu->state.cc_offset = (u32)(value >> 32);
    break; // CC
  }
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
  if (target == (cpu->state.pal_base | 1))
    return nullptr;
  CJitEngine::JitBlock *b =
      cpu->m_jit->lookup(target, (u32)cpu->state.asn, (uint8_t)cpu->state.cm);
  if (!b || !b->jit_body)
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
    cpu->m_jit->note_jmp_hit();
    cpu->m_jit->ind_cache_fill(target, b);
    return b->jit_body;
  }
  // SLOW PATH (only right after an ITB invalidate): the in-frame chain bypasses
  // the dispatcher's `b->phys == start_phys` staleness check (see the
  // hot-path). Virtual+ASN keying can't see a page remap, so a stale block
  // (same tag+ASN, but the vpage now maps different physical bytes) would
  // tail-execute as wrong code -> OPCDEC / garbage.
  const int i = cpu->FindTBEntry(target, ACCESS_EXEC);
  if (i < 0)
    return nullptr; // ITB miss: let the dispatcher fault it in
  const auto &e = cpu->state.tb[TB_INDEX_ITB][i];
  if ((e.phys | (target & e.keep_mask)) != b->phys) {
    // Caught a stale block: the page was remapped without flushing the JIT
    // cache. Bail so the dispatcher re-records/recompiles. Rate-limited log --
    // this firing confirms the stale chain.
    static int n = 0;
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
