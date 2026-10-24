/* ES40 emulator.
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2023-2026 gdwnldsKSC and the ES40-Emu contributors
 * Copyright (C) 2026 Artur Goulão
 *
 * WWW    : http://www.es40.org
 *          https://github.com/ES40-Emu/es40
 *          https://github.com/artur/alphabox
 * E-mail : camiel@es40.org
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
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 */

/**
 * \file
 * Contains the code for the emulated DecChip 21264CB EV68 Alpha processor.
 **/
#include "AlphaCPU.hpp"
#include "StdAfx.hpp"
#ifdef ES40_JIT
#include "jit/jitengine.hpp"
#endif
#include "AliM1543C.hpp"
#include "DebugTarget.hpp"
#include "cpu_arith.hpp"
#include "cpu_bwx.hpp"
#include "cpu_control.hpp"
#include "cpu_debug.hpp"
#include "cpu_fp_branch.hpp"
#include "cpu_fp_memory.hpp"
#include "cpu_fp_operate.hpp"
#include "cpu_logical.hpp"
#include "cpu_memory.hpp"
#include "cpu_misc.hpp"
#include "cpu_mvi.hpp"
#include "cpu_pal.hpp"
#include "cpu_vax.hpp"
#include "diag_rpcc.hpp"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <set>
#include <tuple>
#include <utility>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h> // _mm_setcsr: pin host MXCSR for the JIT SSE FP path
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <cfenv> // fesetenv: pin the host FPCR for the JIT FP path
#ifdef ALPHABOX_HVF
#include "HvRuntime.hpp"
#endif
#endif

void CAlphaCPU::release_threads() { mySemaphore.set(); }

thread_local CAlphaCPU *t_running_cpu = nullptr;

bool CAlphaCPU::s_trace_calls = false;

/**
 * Report a subroutine call, the first time each call site reaches each
 * routine on each processor (ALPHABOX_TRACE_CALLS).
 *
 * Bringing up a console is largely the question "which of its routines ran,
 * and which did not": a firmware image carries a name for every routine
 * (docs/platforms/ds10.md shows how to recover them), so this reads as the
 * console's own call graph. One line per pair keeps a whole boot readable.
 * Interpreter only -- compiled blocks do not pass through here.
 **/
void CAlphaCPU::trace_call(u64 from, u64 to) {
  static std::mutex lock;
  // Per processor: a secondary running the console's code shows its own
  // path, not only the calls the primary has not made yet.
  static std::set<std::tuple<int, u64, u64>> seen;

  std::lock_guard<std::mutex> guard(lock);
  if (!seen.insert(std::make_tuple(get_cpuid(), from, to)).second)
    return;
  printf("%%CPU-T-CALL: cpu%d %011" PRIx64 " -> %011" PRIx64 "\n", get_cpuid(),
         from, to);
}

void CAlphaCPU::run() {
  try {
    t_running_cpu = this;
    mySemaphore.wait();
    while (*const_cast<volatile bool *>(&state.wait_for_start)) {
      if (StopThread)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // ... and acquire what the processor that released us published: the PC
    // it set, and whatever else it arranged before saying go.
    std::atomic_thread_fence(std::memory_order_acquire);
    printf("*** CPU%d *** STARTING ***\n", get_cpuid());

#if defined(_M_X64) || defined(__x86_64__)
    // Pin host SSE state for the JIT FP path: round-nearest, exceptions masked,
    // FTZ/DAZ off (denormal results must materialize to hit the interp-bail).
    _mm_setcsr(0x1F80);
#elif defined(__aarch64__) || defined(_M_ARM64)
    // Same for the AArch64 JIT FP path: the default FP environment is FPCR=0
    // (round-nearest, traps off, flush-to-zero off).
    std::fesetenv(FE_DFL_ENV);
#endif

    // Re-base the timing epoch to when execution actually begins: a
    // secondary parks (wait_for_start) before the primary releases it.
    start_time = std::chrono::steady_clock::now();
    next_timer_fire = start_time;
    tick_last_fire = start_time;
    cc_last_sync = host_ticks();
    state.instruction_count = 0;

  } catch (CException &e) {
    printf("Exception in CPU thread: %s.\n", e.displayText().c_str());
    myThreadDead.store(true);
    return;
  }

#ifdef ALPHABOX_HVF
  if (hv::enabled()) {
    // The whole dispatch loop -- interpreter, JIT compiler, compiled code,
    // the device models it calls -- runs at EL1 inside the VM on this
    // thread's vCPU; only system calls leave it. See docs/hypervisor.md.
    if (hv::call(hv_run_loop, this) == ~0ULL) {
      printf("%%HV-F-CPU%d: the VM stopped this CPU; its thread ends\n",
             get_cpuid());
      myThreadDead.store(true);
    }
    return;
  }
#endif
  run_loop();
}

#ifdef ALPHABOX_HVF
void *CAlphaCPU::operator new(size_t n) {
  if (hv::enabled()) {
    if (void *p = hv::alloc(n))
      return p;
  }
  return ::operator new(n);
}
void CAlphaCPU::operator delete(void *p) noexcept {
  if (hv::enabled())
    return; // the VM's allocator releases everything at exit
  ::operator delete(p);
}
#endif

// The system-bus calls, performed on the host thread. hv::escape() lands
// here with the argument block the caller built; see sys_read/sys_write.
u64 CAlphaCPU::sys_read_out(void *p) {
  SysCall *c = (SysCall *)p;
  return c->sys->ReadMem(c->addr, c->size, c->src);
}

u64 CAlphaCPU::sys_write_out(void *p) {
  SysCall *c = (SysCall *)p;
  c->sys->WriteMem(c->addr, c->size, c->data, c->src);
  return 0;
}

// ALPHABOX_RATE=<seconds>: print this processor's instruction rate every
// <seconds> of wall time. One number, comparable between arms of the same
// binary: how fast guest code actually runs -- and accurate enough to be the
// measurement itself (cpu_bench.sh reads it), which is why the clock is read
// once every 256 batches rather than every one. A batch is 2000 instructions,
// half a microsecond of guest time here, and a steady_clock read is tens of
// nanoseconds: sampling it every batch would have been several per cent of
// the number being reported. Every 256th leaves the window boundaries a
// fraction of a millisecond wide, against a period measured in seconds.
void CAlphaCPU::rate_tick() {
  // Fractional seconds are allowed and are what a short measurement needs:
  // a four-instruction loop is over in a second and a half, which is one
  // window at a period of one second and six at a quarter of one.
  static const double period =
      getenv("ALPHABOX_RATE") ? atof(getenv("ALPHABOX_RATE")) : 0.0;
  if (period <= 0.0)
    return;
  if ((++m_rate_batches & 0xff) != 0)
    return;
  const auto now = std::chrono::steady_clock::now();
  if (m_rate_last == std::chrono::steady_clock::time_point{}) {
    m_rate_last = now;
    m_rate_icount = state.instruction_count;
    return;
  }
  const double secs =
      std::chrono::duration<double>(now - m_rate_last).count();
  if (secs < period)
    return;
  const u64 done = state.instruction_count - m_rate_icount;
#ifdef ALPHABOX_HVF
  const hv::Stats &hs = hv::stats();
  const u64 esc = hs.escapes - m_rate_escapes, ent = hs.entries - m_rate_entries;
  m_rate_escapes = hs.escapes;
  m_rate_entries = hs.entries;
  fprintf(stderr,
          "%%CPU%d-I-RATE: %.2f MIPS (%llu instructions in %.4f s); "
          "%.0f escapes/s, %.0f VM exits/s, %.1f%% of a second in exits at "
          "1 us each\n",
          get_cpuid(), done / secs / 1e6, (unsigned long long)done, secs,
          esc / secs, ent / secs, ent / secs / 1e6 * 100.0);
  {
    const u64 bulk = m_bulk_served - m_rate_bulk;
    m_rate_bulk = m_bulk_served;
    if (bulk)
      fprintf(stderr, "%%CPU%d-I-BULK: %.0f transfers/s served inside\n",
              get_cpuid(), bulk / secs);
  }
  {
    // Which exception classes those exits were. 0x16 is our own HVC (a
    // fault handled at EL1, or an escape), 0x18 a trapped system register,
    // 0x20/0x21 instruction aborts, 0x24/0x25 data aborts.
    char buf[256];
    int n = snprintf(buf, sizeof buf, "%%CPU%d-I-EXITS:", get_cpuid());
    for (int ec = 0; ec < 64 && n < (int)sizeof buf - 24; ec++)
      if (hs.by_ec[ec])
        n += snprintf(buf + n, sizeof buf - n, " EC%02x=%llu", ec,
                      (unsigned long long)hs.by_ec[ec]);
    n += snprintf(buf + n, sizeof buf - n, " | EL1:");
    for (int ec = 0; ec < 64 && n < (int)sizeof buf - 24; ec++)
      if (hs.el1_ec[ec])
        n += snprintf(buf + n, sizeof buf - n, " EC%02x=%llu", ec,
                      (unsigned long long)hs.el1_ec[ec]);
    fprintf(stderr,
            "%s; sysreg iss=%llx pc %llx; last EL1 far %llx pc %llx\n", buf,
            (unsigned long long)hs.last_sysreg, (unsigned long long)hs.last_pc,
            (unsigned long long)hs.el1_far, (unsigned long long)hs.el1_pc);
  }
#else
  // Four decimals on the window: it is what the rate is computed from, and
  // cpu_bench.sh divides by it. At a tenth of a second, a quarter-second
  // window prints as "0.3 s" and reads seventeen per cent slow.
  fprintf(stderr, "%%CPU%d-I-RATE: %.2f MIPS (%llu instructions in %.4f s)\n",
          get_cpuid(), done / secs / 1e6, (unsigned long long)done, secs);
#endif
  m_rate_last = now;
  m_rate_icount = state.instruction_count;
  {
    u64 d[3];
    for (int i = 0; i < 3; i++) {
      d[i] = m_misc_calls[i] - m_rate_misc[i];
      m_rate_misc[i] = m_misc_calls[i];
    }
    // The cycle counter alongside: a guest that spins reading RPCC is
    // waiting for time to pass, and if the counter is frozen -- disabled
    // through CC_CTL, or not advancing -- it waits forever.
    const u64 cc_now = state.cc;
    const u64 cc_moved = cc_now - m_rate_cc;
    m_rate_cc = cc_now;
    if (d[0] + d[1] + d[2])
      fprintf(stderr,
              "%%CPU%d-I-MISC: %.0f RPCC/s, %.0f RC/s, %.0f RS/s  "
              "(%.2f per 100 instructions); cc_ena=%d cc advanced %llu\n",
              get_cpuid(), d[0] / secs, d[1] / secs, d[2] / secs,
              done ? 100.0 * (d[0] + d[1] + d[2]) / done : 0.0,
              state.cc_ena ? 1 : 0, (unsigned long long)cc_moved);
  }
  {
    // What the guest's IMBs cost: how many asked for a flush, how many of
    // those had anything to flush, and how many pages hold code at all.
    const u64 skipped = m_flush_skipped - m_rate_flush_skipped;
    const u64 done_f = m_flush_done - m_rate_flush_done;
    m_rate_flush_skipped = m_flush_skipped;
    m_rate_flush_done = m_flush_done;
    if (m_stall_skips || m_stall_capped) {
      const u64 sk = m_stall_skips - m_rate_stall_skips;
      const u64 cy = m_stall_cycles - m_rate_stall_cycles;
      m_rate_stall_skips = m_stall_skips;
      m_rate_stall_cycles = m_stall_cycles;
      // What the guest asked to wait for, against the time it actually had:
      // a second of stalls handed over inside a second of wall clock is the
      // whole boot spent waiting.
      fprintf(stderr,
              "%%CPU%d-I-STALL: %.0f waits/s skipped, %.1f%% of the window "
              "handed over (%llu too long)\n",
              get_cpuid(), sk / secs, 100.0 * (double)cy / (double)cpu_hz / secs,
              (unsigned long long)m_stall_capped);
    }
    if (skipped + done_f)
      fprintf(stderr,
              "%%CPU%d-I-FLUSH: %.0f IMB/s, %.1f%% with nothing to flush; "
              "%llu code pages\n",
              get_cpuid(), (skipped + done_f) / secs,
              100.0 * (double)skipped / (double)(skipped + done_f),
              (unsigned long long)(m_code_map ? m_code_map->code_pages() : 0));
  }
}

void CAlphaCPU::run_loop() {
  try {
#ifdef ALPHABOX_HVF
    // Phase 2a: the interpreter is what runs inside the VM. The compiler
    // must not: it emits into MAP_JIT memory, which is the one kind of page
    // the framework will not share with a guest, so compiled blocks would
    // be executed from a stale private copy. See docs/hypervisor.md.
    // ALPHABOX_INTERP=1 forces the same interpreter OUTSIDE the VM: the
    // control arm for measuring what running inside costs, in one binary.
    // Inside the VM the compiler's output now lives in the VM allocator's
    // memory (docs/hypervisor.md), so compiled code can run there too --
    // ALPHABOX_HV_JIT=1 turns it on. Default is still the interpreter,
    // which is the configuration that is known good.
    const bool hv_jit =
        getenv("ALPHABOX_HV_JIT") && atoi(getenv("ALPHABOX_HV_JIT")) == 1;
    const bool interp_only =
        (hv::enabled() && !hv_jit) ||
        (getenv("ALPHABOX_INTERP") && atoi(getenv("ALPHABOX_INTERP")) == 1);
#else
    const bool interp_only =
        getenv("ALPHABOX_INTERP") && atoi(getenv("ALPHABOX_INTERP")) == 1;
#endif
    m_interp_only = interp_only;
    clock_anchor();
    for (;;) {
      if (StopThread)
        return;
#ifdef ES40_JIT
      jit_run(2000);
      rate_tick();
#else
      // execute() runs a 512-instruction batch itself; calling it 2000 times
      // here made one scheduler turn ~1M instructions, starving the other CPUs
      // and device threads. With a guest debugger attached (src/debug) the
      // batch is taken down to one instruction while a breakpoint is set.
      if (m_debug_on.load(std::memory_order_relaxed))
        m_debug->interp_run(this, 512);
      else
        execute();
      std::this_thread::yield();
#endif
      if (cSystem && cSystem->IsSystemResetRequested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
    }
  } catch (CException &e) {
    printf("Exception in CPU thread: %s.\n", e.displayText().c_str());
    myThreadDead.store(true);

    // Let the thread die...
  }
}

#ifdef ALPHABOX_HVF
uint64_t CAlphaCPU::hv_run_loop(void *self) {
  ((CAlphaCPU *)self)->run_loop();
  return 0;
}
#endif

/**
 * Constructor.
 **/
CAlphaCPU::CAlphaCPU(CConfigurator *cfg, CSystem *system)
    : CSystemComponent(cfg, system) {
#ifdef ALPHABOX_HVF
  // Under ALPHABOX_HV=1 the compiled code lives in the VM allocator's
  // memory, which our stage-1 entries mark executable INSIDE the VM and
  // which the host may only read and write. Anything that runs this
  // processor outside the VM -- the ROM decompression at startup does,
  // on the main thread -- must therefore interpret. run_loop() clears
  // this once it is executing inside.
  m_interp_only = hv::enabled();
#endif
  s_trace_calls = getenv("ALPHABOX_TRACE_CALLS") != nullptr;
  // The configuration class names the part ("ev68cb").
  m_model = find_cpu_model(cfg->get_myValue());
  if (!m_model)
    FAILURE_1(Configuration, "Unknown Alpha processor %s", cfg->get_myValue());
  // A board takes processors of one core family: an EV7 has its memory
  // controller and router on chip and no Tsunami can drive one, and an EV68
  // has neither of those for a Marvel console to find.
  {
    const cpu_model *board = find_cpu_model(system->platform().cpu_model);
    if (board && board->family != m_model->family) {
      if (!cpu_any_board())
        FAILURE_2(Configuration, "the %s takes %s processors",
                  system->platform().description, board->part);
      printf("%%CPU-W-ANYBOARD: a %s on the %s, which takes %s processors "
             "(the ALPHABOX_CPU_ANY_BOARD test hook)\n",
             m_model->part, system->platform().description, board->part);
    }
  }
}

/**
 * Initialize the CPU.
 **/
/**
 * Draw the minimum spacing before the next catch-up fire. Repay gaps are
 * modulated so consecutive SRM cycles-per-tick windows (~100 ticks) never agree
 * during a repay stretch: per-fire noise alone averages out over a window, the
 * triangle wave's ~2.8-window wavelength survives the averaging, and the noise
 * breaks symmetric-alignment ties. Always between half and ~1.2 periods.
 **/
u64 CAlphaCPU::tick_next_gap_ns(u64 period_ns) {
  tick_fire_idx++;
  const u32 ph = tick_fire_idx % 277;
  const u32 tri = (ph <= 138) ? ph : (277 - ph);
  tick_pace_lcg = tick_pace_lcg * 1664525u + 1013904223u;
  return period_ns / 2 + period_ns * tri / 400 +
         period_ns * ((tick_pace_lcg >> 24) & 0x3f) / 1024;
}

/**
 * The instruction-paced envelope (timer.max_instr_per_tick) is full and the
 * wall-clock interval tick isn't due yet: hold this CPU thread until the tick
 * lands (or, on CPU0, until it must fire it), an interrupt is raised, or the
 * thread is stopped.
 **/
CAlphaCPU::TickHold CAlphaCPU::tick_hold(u64 period_ns) {
  using namespace std::chrono;
  const u32 seq = tick_seen_seq;
  // Secondaries: backstop in case CPU0 is late firing the tick.
  auto deadline = steady_clock::now() + nanoseconds(2 * period_ns);
  if (state.iProcNum == 0) {
    deadline = next_timer_fire;
    if (tick_last_fire + nanoseconds(tick_gap_ns) > deadline)
      deadline = tick_last_fire + nanoseconds(tick_gap_ns);
  }
  for (;;) {
    if (cSystem->get_tick_seq() != seq)
      return TickHold::Ticked;
    if (state.check_int || StopThread || cSystem->IsSystemResetRequested())
      return TickHold::Doorbell;
    const auto now = steady_clock::now();
    if (now >= deadline)
      return state.iProcNum == 0 ? TickHold::Ticked : TickHold::Expired;
    auto nap = deadline - now;
    if (nap > microseconds(100))
      nap = microseconds(100);
    std::this_thread::sleep_for(nap);
  }
}

/// A page this processor had cached as inline-writable has become a code
/// page (some processor compiled a block from it). Compiled code checks the
/// cached tag, not the map, so those cached translations have to go: the
/// refill asks dpc_host_base_w() again and excludes the page, and from then
/// on stores to it take the helper that reports to the map.
void CAlphaCPU::honour_new_code_pages() {
  m_code_pages_seen = m_code_map->code_pages();
  flush_data_page_cache();
  // Until this moment compiled code could still have stored into that page
  // inline through a translation just dropped, unreported. Count one write,
  // as the check_timers path does, so this flush and the next do their work
  // and any such store is caught by the source hash.
  if (!m_nopflush_break) // the break hook silences every report
    m_code_map->note_write_all();
}

void CAlphaCPU::init() {
  memset(&state, 0, sizeof(state));
  tb_idx_rebuild();
  {
    const char *e = getenv("ALPHABOX_DPC_KEEP");
    m_dpc_keep = !(e && strcmp(e, "0") == 0);
    e = getenv("ALPHABOX_LLSC_MEMOP");
    m_llsc_memop = !(e && strcmp(e, "0") == 0);
  }
  cc_last_read = 0; // the rpcc_read floor tracks state.cc: reset together
  cc_borrow = 0;
  cc_wall_remainder = 0;
  last_dtb_virt[0] = last_dtb_virt[1] = 0;

  cpu_hz = myCfg->get_num_value("speed", true, 500000000);
  // Guest cycles per host tick, settled once, so an RPCC read is a multiply.
  cc_tick_hz = host_tick_hz();
  cc_cycles_per_tick_q32 =
      (u64)(((__uint128_t)cpu_hz << 32) / (cc_tick_hz ? cc_tick_hz : 1));
  // Instruction-paced interval-timer cap (timer.max_instr_per_tick, 0 = off).
  m_max_instr_per_tick =
      myCfg->get_num_value("timer.max_instr_per_tick", false, 0);
  tick_last_icount = 0;
  tick_seen_seq = 0;

  state.iProcNum = cSystem->RegisterCPU(this);
  // The processor's ID: its number, except where the chipset places it in
  // a mesh (an EV7's PID follows from its coordinates).
  m_pid = cSystem->chipset()->cpu_pid(state.iProcNum);

  // An IMB with nothing to flush costs nothing: the machine's code-page map
  // says whether anything has been written to a page code was compiled from.
  // ALPHABOX_JIT_NOPFLUSH=0 restores the unconditional flush.
  m_code_map = cSystem->code_pages();
  {
    const char *e = getenv("ALPHABOX_JIT_NOPFLUSH");
    m_nopflush = !(e && e[0] == '0');
    m_nopflush_audit = (e && e[0] == '2');
    m_nopflush_break = getenv("ALPHABOX_JIT_NOPFLUSH_BREAK") != nullptr;
    if (m_nopflush_break)
      printf("%%CPU-W-NOPFLUSH: this processor's stores are NOT being "
             "reported to the code-page map (test hook)\n");
  }

#ifdef ES40_JIT
  if (!m_jit) {
    m_jit = new CJitEngine((int)state.iProcNum, &m_jit_epoch);
    m_jit_ind_base = m_jit->ind_cache_base();
    m_jit->set_cpu_identity(m_model->amask, m_model->implver);
    m_jit->set_dpc_flush_counter(&m_stat_dpc_flushes);
    m_jit->set_code_page_map(m_code_map);
    m_jit->set_nopflush_audit(m_nopflush_audit ? &m_flush_was_clean : nullptr);
  }
  {
    // Tell the JIT the byte offsets (from `this`) of the fields its inline load
    // fast path reads, so compiled code can address them via [cpu + offset].
    CJitEngine::JitOffsets o;
    // Slot [0][0] of the read cache; the inline load adds
    // dpc_index(va)*dpc_stride to reach the indexed slot, so these are the base
    // (slot 0) field offsets.
    o.dpc_valid =
        (uint32_t)((char *)&data_page_cache[0][0].valid - (char *)this);
    o.dpc_virt_page =
        (uint32_t)((char *)&data_page_cache[0][0].virt_page - (char *)this);
    o.dpc_tag = (uint32_t)((char *)&data_page_cache[0][0].tag - (char *)this);
    o.dpc_bias = (uint32_t)((char *)&data_page_cache[0][0].bias - (char *)this);
    o.dpc_key = (uint32_t)((char *)&m_dpc_key - (char *)this);
    o.dpc_phys_base =
        (uint32_t)((char *)&data_page_cache[0][0].phys_base - (char *)this);
    o.dpc_host_base =
        (uint32_t)((char *)&data_page_cache[0][0].host_base - (char *)this);
    o.dpc_cm = (uint32_t)((char *)&data_page_cache[0][0].cm - (char *)this);
    o.dpc_asn = (uint32_t)((char *)&data_page_cache[0][0].asn - (char *)this);
    o.dpc_stride = (uint32_t)sizeof(data_page_cache[0][0]);
    o.dpc_mask = (uint32_t)kDpcMask;
    o.dpc_c_tag = (uint32_t)((char *)&dpc_l1[0][0].tag - (char *)this);
    o.dpc_c_write_row =
        (uint32_t)((char *)&dpc_l1[0][1] - (char *)&dpc_l1[0][0]);
    o.dpc_c_stride = (uint32_t)sizeof(dpc_l1[0]);
    o.dpc_write_row = (uint32_t)((char *)&data_page_cache[1][0] -
                                 (char *)&data_page_cache[0][0]);
    o.dpc2_base = (uint32_t)((char *)&data_page_cache2[0][0] - (char *)this);
    o.dpc2_row = (uint32_t)((char *)&data_page_cache2[1][0] -
                            (char *)&data_page_cache2[0][0]);
    o.dpc2_bits = kDpc2Bits;
    o.dpc2_gen = (uint32_t)((char *)&m_dpc2_gen - (char *)this);
    o.dpc_slot_gen = (uint32_t)offsetof(SDataPageCache, gen);
    // ALPHABOX_JIT_OFFSETS=1: whether the inline page-cache probe can still
    // reach both rows with one displacement. Past that the emitter falls back
    // to computing the slot address, which costs every memory op -- the limit
    // on how large this cache can grow without restructuring it.
    if (getenv("ALPHABOX_JIT_OFFSETS")) {
      const uint32_t bias_rel = o.dpc_bias - o.dpc_tag;
      printf("[JIT] dpc: tag=%u bias_rel=%u stride=%u entries=%d write_row=%u "
             "| one-displacement read=%d write=%d (limit 32760)\n",
             o.dpc_tag, bias_rel, o.dpc_stride, kDpcEntries, o.dpc_write_row,
             (int)(o.dpc_tag + bias_rel <= 32760),
             (int)(o.dpc_tag + o.dpc_write_row + bias_rel <= 32760));
    }
    o.state_cm = (uint32_t)((char *)&state.cm - (char *)this);
    o.state_asn0 = (uint32_t)((char *)&state.asn0 - (char *)this);
    o.dram_ptr = (uint32_t)((char *)&dram_ptr - (char *)this);
    o.dram_size = (uint32_t)((char *)&dram_size - (char *)this);
    o.code_page_bits = (uint32_t)((char *)&m_code_page_bits - (char *)this);
    o.state_pc = (uint32_t)((char *)&state.pc - (char *)this);
    o.state_current_pc = (uint32_t)((char *)&state.current_pc - (char *)this);
    o.jit_budget = (uint32_t)((char *)&m_jit_budget - (char *)this);
    o.check_int = (uint32_t)((char *)&state.check_int - (char *)this);
    o.check_timers = (uint32_t)((char *)&state.check_timers - (char *)this);
    o.link_from = (uint32_t)((char *)&m_link_from - (char *)this);
    o.link_target = (uint32_t)((char *)&m_link_target - (char *)this);
    o.fpen = (uint32_t)((char *)&state.fpen - (char *)this);
    o.exc_sum = (uint32_t)((char *)&state.exc_sum - (char *)this);
    o.f_base = (uint32_t)((char *)&state.f[0] - (char *)this);
    o.fpcr = (uint32_t)((char *)&state.fpcr - (char *)this);
    o.cc_last_sync = (uint32_t)((char *)&cc_last_sync - (char *)this);
    o.cc_tick_hz = (uint32_t)((char *)&cc_tick_hz - (char *)this);
    o.cc_q32 = (uint32_t)((char *)&cc_cycles_per_tick_q32 - (char *)this);
    o.cc_remainder = (uint32_t)((char *)&cc_wall_remainder - (char *)this);
    o.cc_borrow = (uint32_t)((char *)&cc_borrow - (char *)this);
    o.cc_last_read = (uint32_t)((char *)&cc_last_read - (char *)this);
    o.state_cc = (uint32_t)((char *)&state.cc - (char *)this);
    o.cc_ena = (uint32_t)((char *)&state.cc_ena - (char *)this);
    o.cc_offset = (uint32_t)((char *)&state.cc_offset - (char *)this);
    o.exc_addr = (uint32_t)((char *)&state.exc_addr - (char *)this);
    o.pal_base = (uint32_t)((char *)&state.pal_base - (char *)this);
    o.sde = (uint32_t)((char *)&state.sde - (char *)this);
    o.ier_asten = (uint32_t)((char *)&state.asten - (char *)this);
    o.ier_sien = (uint32_t)((char *)&state.sien - (char *)this);
    o.ier_pcen = (uint32_t)((char *)&state.pcen - (char *)this);
    o.ier_cren = (uint32_t)((char *)&state.cren - (char *)this);
    o.ier_slen = (uint32_t)((char *)&state.slen - (char *)this);
    o.ier_eien = (uint32_t)((char *)&state.eien - (char *)this);
    o.sir = (uint32_t)((char *)&state.sir - (char *)this);
    o.eir = (uint32_t)((char *)&state.eir - (char *)this);
    o.aster = (uint32_t)((char *)&state.aster - (char *)this);
    o.astrr = (uint32_t)((char *)&state.astrr - (char *)this);
    o.regs = (uint32_t)((char *)&state.r[0] - (char *)this);
    o.jit_epoch = (uint32_t)((char *)&m_jit_epoch - (char *)this);
    o.llsc_clean = (uint32_t)((char *)&m_llsc_clean - (char *)this);
    o.int_resignal = (uint32_t)((char *)&m_int_resignal - (char *)this);
    o.jit_ind_base = (uint32_t)((char *)&m_jit_ind_base - (char *)this);
#if defined(__aarch64__)
    // The AArch64 emitter addresses every guest register, shadow bank
    // included, as one load from the cpu pointer, 64-bit or 32-bit: a scaled
    // 12-bit offset reaches 16380 bytes for the second. Past that the blocks
    // fail to encode and quietly run in the interpreter -- measured at 6-28
    // times slower, while SRM and JIT_VERIFY still pass.
    if (o.regs + 64 * 8 > 16380 || o.jit_epoch > 32760 ||
        o.jit_ind_base > 32760 || o.llsc_clean > 32760 ||
        o.int_resignal > 32760) {
      printf("%%CPU-F-JITLAYOUT: state.r at %u is out of reach of compiled "
             "code; CAlphaCPU's layout needs fixing\n",
             o.regs);
      exit(1);
    }
#endif
#ifdef JIT_STATS
    if (state.iProcNum == 0)
      printf("[JIT][STATS] offsets: dpc_virt_page=%u dpc_host=%u dpc_cm=%u "
             "stride=%u write_row=%u state_cm=%u regs=%u dram_ptr=%u "
             "dram_size=%u\n",
             o.dpc_virt_page, o.dpc_host_base, o.dpc_cm, o.dpc_stride,
             o.dpc_write_row, o.state_cm,
             (uint32_t)((char *)&state.r[0] - (char *)this), o.dram_ptr,
             o.dram_size);
#endif
    m_jit->set_offsets(o);
  }
#endif

  state.wait_for_start = (state.iProcNum == 0) ? false : true;
  skip_memtest_hack = myCfg->get_bool_value("skip_memtest_hack", false);
#ifdef ES40_JIT
  // The hack patches the SRM memory test at a few PCs as the interpreter
  // reaches them; compiled blocks run the test loops without passing those
  // PCs, so only part of the test gets skipped and SRM then reports memory
  // errors (seen on a warm "init").
  if (skip_memtest_hack && state.iProcNum == 0)
    printf("%%CPU-W-MEMTEST: skip_memtest_hack is ignored in JIT builds.\n");
  skip_memtest_hack = false;
#endif
  icache_enabled = true;
  flush_icache();

  tbia(ACCESS_READ);
  tbia(ACCESS_EXEC);

  //  state.fpcr = U64(0x8ff0000000000000);
  state.fpen = true;
  // I_CTL as the SROM leaves it, SDE<1> apart: state.sde holds that, and
  // stays clear until the console writes I_CTL, so a read says so too.
  state.i_ctl_other = U64(0x502006);
  state.smc = 1;

  // SROM imitation...
  add_tb(0, 0, U64(0xff61), ACCESS_READ, state.asn0);

  myThread = nullptr;

  start_time = std::chrono::steady_clock::now();
  next_timer_fire = start_time;

  state.r[22] = state.r[22 + 32] = state.iProcNum;

  dram_ptr = cSystem->PtrToMem(0);
  dram_size = U64(1) << cSystem->get_memory_bits();
  m_code_page_bits = cSystem->code_pages()->page_bits();

  flush_data_page_cache();

  seq_line_ptr = nullptr;
  seq_offset = 0;
  seq_remaining = 0;
  seq_next_pc = 0;

  printf("%s(%d): $Id$\n", devid_string, state.iProcNum);
  if (state.iProcNum == 0 && m_max_instr_per_tick)
    printf("%%CPU-I-PACING: interval timer paced to %llu guest instructions "
           "per tick\n",
           (unsigned long long)m_max_instr_per_tick);

#if defined(ES40_JIT) && defined(JIT_VERIFY)
  if (state.iProcNum == 0 && getenv("ALPHABOX_JIT_FPTEST"))
    jit_fp_selftest(); // exits with the verdict
#endif
#if defined(ES40_JIT)
  // The inline RPCC stub is the one piece of emitted code JIT_VERIFY
  // cannot check (it replays the helper's result, and the stub does not
  // take part), so it gets its own comparison against that helper.
  if (state.iProcNum == 0 && getenv("ALPHABOX_JIT_RPCCTEST"))
    jit_rpcc_selftest(); // exits with the verdict
#endif
}

void CAlphaCPU::ResetForSystemReset() {
  const int savedProcNum = state.iProcNum;

  memset(&state, 0, sizeof(state));
  tb_idx_rebuild();
  cc_last_read = 0; // the rpcc_read floor tracks state.cc: reset together
  cc_borrow = 0;
  cc_wall_remainder = 0;
  last_dtb_virt[0] = last_dtb_virt[1] = 0;
  state.iProcNum = savedProcNum;

  cpu_hz = myCfg->get_num_value("speed", true, 500000000);
  // Guest cycles per host tick, settled once, so an RPCC read is a multiply.
  cc_tick_hz = host_tick_hz();
  cc_cycles_per_tick_q32 =
      (u64)(((__uint128_t)cpu_hz << 32) / (cc_tick_hz ? cc_tick_hz : 1));
  // Instruction-paced interval-timer cap (timer.max_instr_per_tick, 0 = off).
  m_max_instr_per_tick =
      myCfg->get_num_value("timer.max_instr_per_tick", false, 0);
  tick_last_icount = 0;
  tick_seen_seq = 0;

  state.wait_for_start = (state.iProcNum == 0) ? false : true;
  icache_enabled = true;
  flush_icache();

  tbia(ACCESS_READ);
  tbia(ACCESS_EXEC);

  state.fpen = true;
  // I_CTL as the SROM leaves it, SDE<1> apart: state.sde holds that, and
  // stays clear until the console writes I_CTL, so a read says so too.
  state.i_ctl_other = U64(0x502006);
  state.smc = 1;

  // SROM imitation...
  add_tb(0, 0, U64(0xff61), ACCESS_READ, state.asn0);

  myThread = nullptr;

  start_time = std::chrono::steady_clock::now();
  next_timer_fire = start_time;

  state.r[22] = state.r[22 + 32] = state.iProcNum;

  dram_ptr = cSystem->PtrToMem(0);
  dram_size = U64(1) << cSystem->get_memory_bits();
  m_code_page_bits = cSystem->code_pages()->page_bits();

  flush_data_page_cache();

  seq_line_ptr = nullptr;
  seq_offset = 0;
  seq_remaining = 0;
  seq_next_pc = 0;
}

void CAlphaCPU::start_threads() {
  char buffer[5];
  mySemaphore.try_wait_for(std::chrono::milliseconds(1));
  if (!myThread) {
    sprintf(buffer, "cpu%d", state.iProcNum);
    printf(" %s", buffer);
    StopThread = false;
    myThread = std::make_unique<std::thread>([this]() { this->run(); });
  }
}

void CAlphaCPU::stop_threads() {
  char buffer[5];
  StopThread = true;
  if (myThread) {
    mySemaphore.set();
    sprintf(buffer, "cpu%d", state.iProcNum);
    printf(" %s", buffer);
    myThread->join();
    myThread = nullptr;
  }

  mySemaphore.try_wait_for(std::chrono::milliseconds(1));
}

// ============================================================================
//
// Alpha FPCR layout (all meaningful bits are in the upper 32 bits):
//
//   63    SUM      - Summary Bit (SUM).
//   62    INED     - Inexact Disable (INED).
//   61    UNFD     - Underflow Disable (UNFD)
//   60    UNDZ     - Underflow to Zero (UNDZ)
//   59:58 DYN      -  Dynamic Rounding Mode (DYN)
//   57    IOV      - Integer Overflow (IOV)
//   56    INE      - Inexact Result (INE)
//   55    UNF      - Underflow (UNF).
//   54    OVF      - Overflow (OVF)
//   53    DZE      - Division by Zero (DZE)
//   52    INV      - Invalid Operation (INV)
//   51    OVFD     - Overflow Disable (OVFD)
//   50    DZED     - Division by Zero Disable (DZED)
//   49    INVD     - Invalid Operation Disable (INVD)
//   48    DNZ      - Denormal Operands to Zero (DNZ)
//   47    DNOD     - Denormal Operand Exception Disable (DNOD)
//   46:0  Reserved, must be read as zero
//
//   Alpha Architecture Reference Manual, 4th ed. [ARM]:
//     Section 4.7.8   - Floating-Point Control Register (FPCR)
//     Section 4.7.8.1 - Accessing the FPCR
//     Section 4.7.8.2 - Default Values of the FPCR
//     Section 4.10.4  - Move from/to Floating-Point Control Register
//   at least according to this one
//   https://download.majix.org/dec/alpha_arch_ref.pdf
// ============================================================================

u64 CAlphaCPU::read_fpcr_arch() const {
  u64 val = state.fpcr & ~(FPCR_RAZ | U64(0x00000000FFFFFFFF));

  if (val & FPCR_ERR)
    val |= FPCR_SUM;
  else
    val &= ~FPCR_SUM;

  return val;
}

void CAlphaCPU::write_fpcr_arch(u64 arch_val) {
  u64 val = arch_val & ~(FPCR_RAZ | U64(0x00000000FFFFFFFF));

  if (val & FPCR_ERR)
    val |= FPCR_SUM;
  else
    val &= ~FPCR_SUM;

  state.fpcr = val;
}

/**
 * Destructor.
 **/
// ALPHABOX_TRACE_ICFLUSH=1: histogram of the PC that issued each icache flush,
// printed at exit. An IC_FLUSH rejects every cached block link, and the census
// says the guest issues one per ~1000 instructions, so the question of WHICH
// instruction does it decides the whole invalidation design.
static const bool s_trace_icflush = getenv("ALPHABOX_TRACE_ICFLUSH") != nullptr;
static std::map<uint64_t, uint64_t> s_icflush_pc;
static std::mutex s_icflush_lock;
static std::map<uint64_t, uint64_t> s_icflush_from; // R23 at the flush
void CAlphaCPU::note_ic_flush_pc() {
  if (!s_trace_icflush)
    return;
  std::lock_guard<std::mutex> g(s_icflush_lock);
  const bool first = s_icflush_pc[state.current_pc]++ == 0;
  // The caller: the PALcode's R23, which a CALL_PAL loads with its return
  // address (CALL_PAL does not write EXC_ADDR, HRM 5.2.7).
  const u64 from = state.r[(state.sde ? 32 : 0) + 23] & ~U64(3);
  const bool first_from = s_icflush_from[from]++ == 0;
  // First sighting of a caller: the words before its return address, so the
  // CALL_PAL (and what surrounds it) can be read. Firmware runs identity
  // mapped in low memory; anything else is skipped rather than translated.
  if (first_from && dram_ptr && from < dram_size && from >= 48) {
    printf("%%CPU-I-ICFLUSH: first flush entered from R23 %016" PRIx64 "\n",
           from);
    for (uint64_t a = from - 48; a < from + 16; a += 4) {
      uint32_t w;
      memcpy(&w, (const char *)dram_ptr + a, 4);
      printf("%%CPU-I-ICFLUSH:   %s %08llx: %08x  op=%02x ra=%02x rb=%02x "
             "fn=%04x\n",
             a == from - 4 ? "->" : "  ", (unsigned long long)a, w, w >> 26,
             (w >> 21) & 31, (w >> 16) & 31, w & 0xffff);
    }
  }
  // First sighting of a flushing PC: dump the words around it NOW, while the
  // PAL image that holds it is still in memory (a firmware PAL is gone by the
  // time the CPU is destroyed). PALmode PC == physical, bit 0 the PAL flag.
  if (first && dram_ptr) {
    const uint64_t phys = state.current_pc & ~(uint64_t)3;
    const uint64_t from = (phys >= 32) ? phys - 32 : 0;
    printf("%%CPU-I-ICFLUSH: first flush from %016" PRIx64 " (exc_addr %016" PRIx64
           ", pal_base %016" PRIx64 ")\n",
           state.current_pc, state.exc_addr, state.pal_base);
    for (uint64_t a = from; a < phys + 40 && a + 4 <= dram_size; a += 4) {
      uint32_t w;
      memcpy(&w, (const char *)dram_ptr + a, 4);
      printf("%%CPU-I-ICFLUSH:   %s %08llx: %08x  op=%02x ra=%02x rb=%02x "
             "fn=%04x\n",
             a == phys ? "->" : "  ", (unsigned long long)a, w, w >> 26,
             (w >> 21) & 31, (w >> 16) & 31, w & 0xffff);
    }
  }
}
void CAlphaCPU::dump_ic_flush_pcs() {
  if (!s_trace_icflush)
    return;
  std::lock_guard<std::mutex> g(s_icflush_lock);
  std::vector<std::pair<uint64_t, uint64_t>> v(s_icflush_pc.begin(),
                                               s_icflush_pc.end());
  std::sort(v.begin(), v.end(),
            [](const std::pair<uint64_t, uint64_t> &a,
               const std::pair<uint64_t, uint64_t> &b) {
              return a.second > b.second;
            });
  uint64_t tot = 0;
  for (auto &e : v)
    tot += e.second;
  printf("%%CPU-I-ICFLUSH: %llu icache flushes from %zu distinct PCs\n",
         (unsigned long long)tot, v.size());
  for (size_t i = 0; i < v.size() && i < 12; ++i)
    printf("%%CPU-I-ICFLUSH:   %016llx  %llu (%.1f%%)\n",
           (unsigned long long)v[i].first, (unsigned long long)v[i].second,
           tot ? 100.0 * (double)v[i].second / (double)tot : 0.0);
  // Who entered PALcode for it: R23 at the flush is the return address of
  // the CALL_PAL that led there.
  {
    std::vector<std::pair<uint64_t, uint64_t>> f(s_icflush_from.begin(),
                                                 s_icflush_from.end());
    std::sort(f.begin(), f.end(),
              [](const std::pair<uint64_t, uint64_t> &a,
                 const std::pair<uint64_t, uint64_t> &b) {
                return a.second > b.second;
              });
    printf("%%CPU-I-ICFLUSH: entered from %zu distinct R23s\n", f.size());
    for (size_t i = 0; i < f.size() && i < 12; ++i)
      printf("%%CPU-I-ICFLUSH:   R23 %016llx  %llu (%.1f%%)\n",
             (unsigned long long)f[i].first, (unsigned long long)f[i].second,
             tot ? 100.0 * (double)f[i].second / (double)tot : 0.0);
  }
  // The instruction words around the busiest site, so the PAL routine can be
  // identified (PALmode PC == physical, bit 0 is the PAL flag).
  if (!v.empty() && dram_ptr) {
    const uint64_t phys = (v[0].first & ~(uint64_t)3) & ~(uint64_t)1;
    const uint64_t from = (phys >= 32) ? phys - 32 : 0;
    for (uint64_t a = from; a < phys + 40 && a + 4 <= dram_size; a += 4) {
      uint32_t w;
      memcpy(&w, (const char *)dram_ptr + a, 4);
      printf("%%CPU-I-ICFLUSH:   %s %08llx: %08x  op=%02x ra=%02x rb=%02x "
             "fn=%04x\n",
             a == phys ? "->" : "  ", (unsigned long long)a, w, w >> 26,
             (w >> 21) & 31, (w >> 16) & 31, w & 0xffff);
    }
  }
}

#ifdef JIT_STATS
void dump_device_pages();
#endif
CAlphaCPU::~CAlphaCPU() {
  stop_threads();
  dump_ic_flush_pcs();
#ifdef JIT_STATS
  dump_device_pages();
#endif
#ifdef JIT_VERIFY
  printf("%%CPU-I-TBINDEX: %llu probes, %llu false negatives\n",
         (unsigned long long)m_tb_idx_probes,
         (unsigned long long)m_tb_idx_false_neg);
#endif
}

#if defined(MIPS_ESTIMATE)

// MIPS_INTERVAL must take longer than 1 second to execute
// or estimate will generate a divide-by-zero error
#define MIPS_INTERVAL 0xfffffff
static time_t saved = 0;
static u64 count;
static double min_mips = 999999999999999.0;
static double max_mips = 0.0;
#include <time.h>
#endif

/**
 * Check if threads are still running.
 *
 * Calibrate the CPU timing loop.
 **/
/// %CPU-W-OPCDEC (UNKNOWN1/UNKNOWN2): the first time each opcode and function
/// raises OPCDEC. A guest that emulates an instruction through the trap, or
/// the instruction oracle's OPCDEC cases, takes the same one again and again,
/// and a line each time flooded the output.
void CAlphaCPU::report_opcdec(u32 opcode, int function) {
  static std::mutex mx;
  static std::set<u32> seen;
  const u32 key = (opcode << 17) | (u32)(function + 1);
  {
    std::lock_guard<std::mutex> g(mx);
    if (!seen.insert(key).second)
      return;
  }
  if (function < 0)
    printf("%%CPU-W-OPCDEC: unknown opcode %02x at pc=%016" PRIx64
           " (reported once)\n",
           opcode, state.current_pc);
  else
    printf("%%CPU-W-OPCDEC: unknown opcode %02x.%02x at pc=%016" PRIx64
           " (reported once)\n",
           opcode, function, state.current_pc);
}

void CAlphaCPU::check_state() {
  if (myThreadDead.load())
    FAILURE(Thread, "CPU thread has died");

  // Debug aid: ALPHABOX_PC_SAMPLE=1 prints the guest PC on every check_state
  // poll (~100 ms) -- identifies guest-side hangs/loops on headless runs.
  static const char *pc_sample = getenv("ALPHABOX_PC_SAMPLE");
  if (pc_sample && *pc_sample == '1')
    // ra/pv locate the caller of a hot helper (lock, timer, wait loop).
    fprintf(stderr, "PCSAMPLE cpu%d pc=%016llx icount=%llu ra=%llx pv=%llx\n",
            (int)state.iProcNum, (unsigned long long)state.pc,
            (unsigned long long)state.instruction_count,
            (unsigned long long)state.r[26], (unsigned long long)state.r[27]);

  return;
}

/**
 * \brief Called each clock-cycle.
 *
 * This is where the actual CPU emulation takes place. Each clocktick, one
 *instruction is processed by the processor. The instruction pipeline is not
 *emulated, things are complicated enough as it is. The one exception is the
 *instruction cache, which is implemented, to accomodate self-modifying code.
 *The instruction cache can be disabled if self-modifying code is not expected.
 **/
void CAlphaCPU::execute() {
  u32 ins;
  int i = 0;
  u64 phys_address;
  u64 temp_64;
  u64 temp_64_1;
  u64 temp_64_2;
  UFP ufp1;
  UFP ufp2;

  bool pbc = false;

  int opcode;
  int function;

#ifndef ES40_JIT
  // ---- Batch loop: execute up to 512 instructions before returning ----
  int _batch_budget = m_exec_batch;
  int _icount_accum = 0; // accumulated instruction count
#endif

#ifndef ES40_JIT
  {
    const auto now = std::chrono::steady_clock::now();

    // Wall-clock RPCC at batch granularity - same semantics as the JIT build.
    // The old per-instruction advance ran at a cycles-per-instruction rate,
    // which lagged real time while a feedback loop converged; SRM's
    // cycles-per-tick speed calibration measured that lag consistently and
    // locked in a low CPU speed.
    sync_cc_wallclock();

    // Poll the wall-clock Cchip interval timer once per execute() batch
    // (~512 instructions) rather than every 32;
    if (state.iProcNum == 0) {
      if (now >= next_timer_fire) {
        const u64 period_ns = theAli ? theAli->get_interval_period_ns()
                                     : cSystem->chipset()->interval_period_ns();
        if (period_ns) {
          // Count-preserving, paced catch-up: the schedule advances one period
          // per fire so ticks lost to a busy/stalled CPU0 thread are repaid and
          // the guests' tick-counted clocks (VMS never resyncs) stay true to
          // wall time. Repayment is paced to >= half a period between fires
          // (max 2x nominal, never a burst - burst/compressed ticks skew
          // RPCC-vs-tick calibrations). Backlog beyond 1s (debugger pause, host
          // sleep) is dropped.
          // Repay gaps are modulated (tick_next_gap_ns).
          if (now - tick_last_fire >= std::chrono::nanoseconds(tick_gap_ns)) {
            cSystem->interval_tick();
            tick_last_fire = now;
            tick_gap_ns = tick_next_gap_ns(period_ns);
            next_timer_fire += std::chrono::nanoseconds(period_ns);
            if (now - next_timer_fire > std::chrono::seconds(1))
              next_timer_fire = now;
          }
        } else {
          cSystem->interval_tick();
          tick_last_fire = now;
          // No period yet. With the ALi, once a second until its timer is
          // programmed; a chipset that sets the period itself (Marvel's
          // RBOX_IT) is looked at again within a millisecond, so its first
          // tick is not a second late.
          next_timer_fire = now + (theAli ? std::chrono::nanoseconds(1000000000)
                                          : std::chrono::nanoseconds(1000000));
        }
      }
    }
  }

_next_instruction:
  if (--_batch_budget <= 0) {
    // Flush the accumulated instruction count before returning. state.cc is
    // wall-clock (advanced at batch top).
    state.instruction_count += _icount_accum;
    return;
  }
#endif

#if defined(MIPS_ESTIMATE)

  // Calculate simulated performance statistics
  if (++count >= MIPS_INTERVAL) {
    clock_t current = clock();

    if (saved > 0) {
      double secs = (current - saved) / (double)CLOCKS_PER_SEC;
      double ips = MIPS_INTERVAL / secs;
      double mips = ips / 1000000.0;
      if (max_mips < mips)
        max_mips = mips;
      if (min_mips > mips)
        min_mips = mips;
      printf("Alphabox MIPS (%3.1f sec):: current: %5.3f, min: %5.3f, max: "
             "%5.3f\n",
             secs, mips, min_mips, max_mips);
    }

    saved = current;
    count = 0;
  }
#endif
  state.current_pc = state.pc;

  //--------------------------------------------------------------------------------
  // This section skips the memory check in SRM. Set the define in config_debug
  // for the memory check to run.
  //--------------------------------------------------------------------------------
  // All five SRM mem-test patch points are in page 0x8b000 and only fire
  // during early SRM boot. Gate on the page so every other instruction pays
  // one compare instead of five.
  if (skip_memtest_hack && (state.current_pc & ~U64(0xFFF)) == U64(0x8b000)) {
    if (state.current_pc == U64(0x8bb90)) {
      if (state.r[5] != U64(0xaaaaaaaaaaaaaaaa)) {
        printf("wrong memory check skip!\n");
      } else {
        state.r[0] = state.r[4];
      }
    }

    if (state.current_pc == U64(0x8bbe0)) {
      if (state.r[5] != U64(0xaaaaaaaaaaaaaaaa)) {
        printf("wrong memory check skip!\n");
      } else {
        state.r[16] = 0;
      }
    }

    if (state.current_pc == U64(0x8bc28)) {
      if (state.r[5] != U64(0xaaaaaaaaaaaaaaaa)) {
        printf("wrong memory check skip!\n");
      } else {
        state.r[8] = state.r[4];
      }
    }

    if (state.current_pc == U64(0x8bc70)) {
      if (state.r[7] != U64(0x5555555555555555)) {
        printf("wrong memory check skip1!\n");
      } else {
        state.r[0] = 0;
      }
    }

    if (state.current_pc == U64(0x8bcb0)) {
      if (state.r[7] != U64(0x5555555555555555)) {
        printf("wrong memory check skip2!\n");
      } else {
        state.r[3] = state.r[4];
      }
    }
  }
  //--------------------------------------------------------------------------------
  // end of skip memory test section
  //--------------------------------------------------------------------------------

  // Service interrupts
#ifndef ES40_JIT
  // We're actually executing code. Cycle counter should be updated, interrupt
  // and interrupt timer status needs to be checked, and the next instruction
  // should be fetched from the instruction cache. Increase the cycle counter
  // if it is currently enabled.
  _icount_accum++;

  if ((_batch_budget & 31) == 0) {
    // Flush the accumulated instruction count to state. state.cc is
    // wall-clock (batch top).
    state.instruction_count += _icount_accum;
    _icount_accum = 0;
    // Another CPU's IMB (AlphaCPU.hpp, g_icache_flush); the JIT build looks
    // at the start of each dispatch batch instead.
    honour_remote_icache_flush();

    // There are one or more active delayed irq_h interrupts. Go through the 6
    // irq_h timers, decrease them as needed, and set the interrupt if the
    // timer reaches 0. Batch to reduce memory ops.
    if (state.check_timers) {
      state.check_timers = false;
      if (m_dpc_flush_req.exchange(false, std::memory_order_acq_rel)) {
        // The direct range changed, or a page became a code page (see
        // CSystem::request_code_page_flush).
        flush_data_page_cache();
        if (m_code_map) {
          // Until this moment compiled code could still have stored into
          // that page inline through the translation just dropped. Count
          // one write, so the next flush does its work and any such store
          // is caught by the source hash rather than assumed away.
          if (!m_nopflush_break) // the break hook silences every report
            m_code_map->note_write_all();
          m_code_pages_seen = m_code_map->code_pages();
        }
      }
      for (int j = 0; j < 6; j++)
        count_down_irq(j, 32);
    }
  }
#else
  // New unwound interpreter / future JIT path

  state.instruction_count++;
  // state.cc (RPCC) is pinned to wall-clock * cpu_hz at the jit_run boundary,
  // not advanced per-instruction here - interpreter can't run RPCC ahead of
  // wall time.

  // Process delayed irq_h timers one instruction at a time.
  if (state.check_timers) {
    state.check_timers = false;
    // The same page-cache flush request the interpreter arm above honours.
    // It lived only there, so in a JIT build -- which is every build that
    // matters -- request_dpc_flush() set a flag nothing ever read: the
    // direct framebuffer window could move or be withdrawn while compiled
    // code still wrote through a cached translation to where it used to
    // be, and the code-page broadcast never reached the other processors.
    if (m_dpc_flush_req.exchange(false, std::memory_order_acq_rel)) {
      flush_data_page_cache();
      if (m_code_map) {
        if (!m_nopflush_break) // the break hook silences every report
          m_code_map->note_write_all();
        m_code_pages_seen = m_code_map->code_pages();
      }
    }
    for (int ti = 0; ti < 6; ti++)
      count_down_irq(ti, 1);
  }
#endif

  if (state.check_int && !(state.pc & 1)) {
    // Clear the interrupt doorbell up front. A remote irq_h() (e.g. an IPI
    // from another CPU) sets eir then re-raises check_int; clearing first
    // guarantees a cross-thread re-raise is re-checked next tick rather than
    // lost by a late clear.
    state.check_int = false;

    // One or more of the variables that affect interrupt status have changed,
    // and we are not currently inside PALmode. It is not certain that this
    // means we hava an interrupt to service, but we might have. This needs to
    // be checked; a pending one is taken at the PALcode's interrupt entry
    // point.
    //        if (state.eir & 8)
    //        {
    //          printf("%s: IP interrupt received%s...\n",devid_string,
    //          (state.eien&8)?"(enabled)":"(masked)");
    //        }
    if ((state.eien & state.eir) || (state.sien & state.sir) ||
        (state.asten &&
         (state.aster & state.astrr & ((1 << (state.cm + 1)) - 1)))) {
      { // ALPHABOX_IRQSTATS: what the guest is taking interrupts for
        const u64 pend = (u64)(state.eien & state.eir);
        for (int b = 0; b < 6; b++)
          if (pend & (U64(1) << b))
            g_irqstats.cpu_eir[b].fetch_add(1, std::memory_order_relaxed);
        if (state.sien & state.sir)
          g_irqstats.cpu_sw.fetch_add(1, std::memory_order_relaxed);
        if (state.asten &&
            (state.aster & state.astrr & ((1 << (state.cm + 1)) - 1)))
          g_irqstats.cpu_ast.fetch_add(1, std::memory_order_relaxed);
        g_irqstats.cpu_int.fetch_add(1, std::memory_order_relaxed);
        irq_trace_entry();
      }
      GO_PAL(INTERRUPT);
      // check_int was cleared to take this one; what is still pending is
      // signalled again when the PALcode returns to native mode (DO_HW_RET).
      m_int_resignal = 1;
      seq_remaining = 0;
#ifndef ES40_JIT
      goto _next_instruction;
#else
      return;
#endif
    }
  }

  // If profiling is enabled, increase the profiling counter for the current
  // block of addresses.
#if defined(PROFILE)
  PROFILE_DO(state.pc);
#endif

#ifndef ES40_JIT
  // ---- Fast sequential icache path ----
  // If PC matches expected sequential address and we have words remaining
  // in the current icache line, read directly without any lookup.
  if (state.pc == seq_next_pc && seq_remaining > 0) {
    ins = endian_32(seq_line_ptr[seq_offset]);
#if defined(DEBUG_ARC)
    if (state.current_pc < 0x10000) {
      printf("PC=%016" PRIx64 " ins=%08x\n", state.current_pc, ins);
    }
#endif
    seq_offset++;
    seq_remaining--;
    seq_next_pc += 4;
    // state.pc_phys += 4;
  } else {
    // PAL reset-vector entry: drop stale icache lines (in-place image
    // rewrite by the firmware updater)
    if (state.pc == (state.pal_base | 1)) {
      JIT_FLUSH_CAUSE(EPOCH_PALRST);
      flush_icache();
    }
    // Full icache lookup
    if (get_icache(state.pc, &ins))
      goto _next_instruction;

    // Set up sequential tracking from the cache hit
    if (icache_enabled && !(state.pc & 1)) {
      int _siq_line = state.last_found_icache;
      seq_line_ptr = state.icache[_siq_line].data;
      int _siq_word = (int)((state.pc >> 2) & ICACHE_INDEX_MASK);
      seq_offset = _siq_word + 1;
      seq_remaining = ICACHE_LINE_SIZE - seq_offset;
      seq_next_pc = (state.pc & ~U64(0x3)) + 4;
    } else {
      seq_remaining = 0;
    }
  }
#else
  // Fast sequential icache path -- the same fast path the batched interpreter
  // uses above. The JIT lane calls execute() once per instruction, so this
  // cursor persists across calls within an interpreted run. A compiled block
  // can't remap (it runs no PAL/TB ops), and any flush or IMB resets the
  // cursor through break_seq_icache(), so stale lines can't be read. A
  // TB-miss returns to the dispatcher (which re-dispatches at the fault
  // handler).
  if (state.pc == seq_next_pc && seq_remaining > 0) {
    ins = endian_32(seq_line_ptr[seq_offset]);
    seq_offset++;
    seq_remaining--;
    seq_next_pc += 4;
  } else {
    // PAL reset-vector entry: drop stale icache lines
    if (state.pc == (state.pal_base | 1)) {
      JIT_FLUSH_CAUSE(EPOCH_PALRST);
      flush_icache();
    }
    if (get_icache(state.pc, &ins))
      return;

    if (icache_enabled && !(state.pc & 1)) {
      int _siq_line = state.last_found_icache;
      seq_line_ptr = state.icache[_siq_line].data;
      int _siq_word = (int)((state.pc >> 2) & ICACHE_INDEX_MASK);
      seq_offset = _siq_word + 1;
      seq_remaining = ICACHE_LINE_SIZE - seq_offset;
      seq_next_pc = (state.pc & ~U64(0x3)) + 4;
    } else {
      seq_remaining = 0;
    }
  }
#endif

  // Increase the program counter. The current value is retained in
  // state.current_pc. This must go through next_pc(): the icache-disabled
  // fetch path in get_icache() relies on next_pc() keeping pc_phys and
  // rem_ins_in_page in sync with pc. A bare pc += 4 leaves pc_phys frozen,
  // so every subsequent fetch on the page re-reads the same physical word
  // (SRM runs away into pattern-filled memory during the console banner).
  next_pc();

  // Clear "always zero" registers. The last instruction might have written
  // something to one of these registers.
  state.r[31] = 0;
  state.f[31] = 0;

  // Decode and dispatch opcode. This is kept very compact using the OP macro
  // defined in cpu_debug.hpp, which calls the DO_<mnemonic> macro defined in
  // one of the other cpu_*.hpp files and ends the instruction.
  opcode = ins >> 26;
  switch (opcode) {
  case 0x00: // CALL_PAL
    function = ins & 0x1fffffff;
    OP(CALL_PAL, PAL);

  case 0x08:
    OP(LDA, MEM);

  case 0x09:
    OP(LDAH, MEM);

  case 0x0a:
    OP(LDBU, MEM);

  case 0x0b:
    OP(LDQ_U, MEM);

  case 0x0c:
    OP(LDWU, MEM);

  case 0x0d:
    OP(STW, MEM);

  case 0x0e:
    OP(STB, MEM);

  case 0x0f:
    OP(STQ_U, MEM);

  case 0x10: // INTA* instructions
    function = (ins >> 5) & 0x7f;
    switch (function) {
    case 0x40:
      OP(ADDL_V, R12_R3);
    case 0x00:
      OP(ADDL, R12_R3);
    case 0x02:
      OP(S4ADDL, R12_R3);
    case 0x49:
      OP(SUBL_V, R12_R3);
    case 0x09:
      OP(SUBL, R12_R3);
    case 0x0b:
      OP(S4SUBL, R12_R3);
    case 0x0f:
      OP(CMPBGE, R12_R3);
    case 0x12:
      OP(S8ADDL, R12_R3);
    case 0x1b:
      OP(S8SUBL, R12_R3);
    case 0x1d:
      OP(CMPULT, R12_R3);
    case 0x60:
      OP(ADDQ_V, R12_R3);
    case 0x20:
      OP(ADDQ, R12_R3);
    case 0x22:
      OP(S4ADDQ, R12_R3);
    case 0x69:
      OP(SUBQ_V, R12_R3);
    case 0x29:
      OP(SUBQ, R12_R3);
    case 0x2b:
      OP(S4SUBQ, R12_R3);
    case 0x2d:
      OP(CMPEQ, R12_R3);
    case 0x32:
      OP(S8ADDQ, R12_R3);
    case 0x3b:
      OP(S8SUBQ, R12_R3);
    case 0x3d:
      OP(CMPULE, R12_R3);
    case 0x4d:
      OP(CMPLT, R12_R3);
    case 0x6d:
      OP(CMPLE, R12_R3);
    default:
      UNKNOWN2;
    }
    break;

  case 0x11: // INTL* instructions
    function = (ins >> 5) & 0x7f;
    switch (function) {
    case 0x00:
      OP(AND, R12_R3);
    case 0x08:
      OP(BIC, R12_R3);
    case 0x14:
      OP(CMOVLBS, R12_R3);
    case 0x16:
      OP(CMOVLBC, R12_R3);
    case 0x20:
      OP(BIS, R12_R3);
    case 0x24:
      OP(CMOVEQ, R12_R3);
    case 0x26:
      OP(CMOVNE, R12_R3);
    case 0x28:
      OP(ORNOT, R12_R3);
    case 0x40:
      OP(XOR, R12_R3);
    case 0x44:
      OP(CMOVLT, R12_R3);
    case 0x46:
      OP(CMOVGE, R12_R3);
    case 0x48:
      OP(EQV, R12_R3);
    case 0x61:
      OP(AMASK, R2_R3);
    case 0x64:
      OP(CMOVLE, R12_R3);
    case 0x66:
      OP(CMOVGT, R12_R3);
    case 0x6c:
      OP(IMPLVER, X_R3);
    default:
      UNKNOWN2;
    }
    break;

  case 0x12: // INTS* instructions
    function = (ins >> 5) & 0x7f;
    switch (function) {
    case 0x02:
      OP(MSKBL, R12_R3);
    case 0x06:
      OP(EXTBL, R12_R3);
    case 0x0b:
      OP(INSBL, R12_R3);
    case 0x12:
      OP(MSKWL, R12_R3);
    case 0x16:
      OP(EXTWL, R12_R3);
    case 0x1b:
      OP(INSWL, R12_R3);
    case 0x22:
      OP(MSKLL, R12_R3);
    case 0x26:
      OP(EXTLL, R12_R3);
    case 0x2b:
      OP(INSLL, R12_R3);
    case 0x30:
      OP(ZAP, R12_R3);
    case 0x31:
      OP(ZAPNOT, R12_R3);
    case 0x32:
      OP(MSKQL, R12_R3);
    case 0x34:
      OP(SRL, R12_R3);
    case 0x36:
      OP(EXTQL, R12_R3);
    case 0x39:
      OP(SLL, R12_R3);
    case 0x3b:
      OP(INSQL, R12_R3);
    case 0x3c:
      OP(SRA, R12_R3);
    case 0x52:
      OP(MSKWH, R12_R3);
    case 0x57:
      OP(INSWH, R12_R3);
    case 0x5a:
      OP(EXTWH, R12_R3);
    case 0x62:
      OP(MSKLH, R12_R3);
    case 0x67:
      OP(INSLH, R12_R3);
    case 0x6a:
      OP(EXTLH, R12_R3);
    case 0x72:
      OP(MSKQH, R12_R3);
    case 0x77:
      OP(INSQH, R12_R3);
    case 0x7a:
      OP(EXTQH, R12_R3);
    default:
      UNKNOWN2;
    }
    break;

  case 0x13: // INTM* instructions
    function = (ins >> 5) & 0x7f;
    switch (function) // ignore /V for now
    {
    case 0x40:
      OP(MULL_V, R12_R3);
    case 0x00:
      OP(MULL, R12_R3);
    case 0x60:
      OP(MULQ_V, R12_R3);
    case 0x20:
      OP(MULQ, R12_R3);
    case 0x30:
      OP(UMULH, R12_R3);
    default:
      UNKNOWN2;
    }
    break;

  case 0x14: // ITFP* instructions
    function = (ins >> 5) & 0x7ff;
    switch (function) {
    case 0x004:
      OP(ITOFS, R1_F3);

    case 0x00a:
    case 0x08a:
    case 0x10a:
    case 0x18a:
    case 0x40a:
    case 0x48a:
    case 0x50a:
    case 0x58a:
      OP(SQRTF, F2_F3);

    case 0x00b:
    case 0x04b:
    case 0x08b:
    case 0x0cb:
    case 0x10b:
    case 0x14b:
    case 0x18b:
    case 0x1cb:
    case 0x50b:
    case 0x54b:
    case 0x58b:
    case 0x5cb:
    case 0x70b:
    case 0x74b:
    case 0x78b:
    case 0x7cb:
      OP(SQRTS, F2_F3);

    case 0x014:
      OP(ITOFF, R1_F3);

    case 0x024:
      OP(ITOFT, R1_F3);

    case 0x02a:
    case 0x0aa:
    case 0x12a:
    case 0x1aa:
    case 0x42a:
    case 0x4aa:
    case 0x52a:
    case 0x5aa:
      OP(SQRTG, F2_F3);

    case 0x02b:
    case 0x06b:
    case 0x0ab:
    case 0x0eb:
    case 0x12b:
    case 0x16b:
    case 0x1ab:
    case 0x1eb:
    case 0x52b:
    case 0x56b:
    case 0x5ab:
    case 0x5eb:
    case 0x72b:
    case 0x76b:
    case 0x7ab:
    case 0x7eb:
      OP(SQRTT, F2_F3);

    default:
      UNKNOWN2;
    }
    break;

  case 0x15: // FLTV* instructions (fltv_operate, which compiled code calls too)
    function = (ins >> 5) & 0x7ff;
    if (!fltv_operate(ins, (u32)function)) {
      UNKNOWN2;
    }
    ES40_EXECUTE_END();

  case 0x16: // FLTI* instructions
    function = (ins >> 5) & 0x7ff;
    switch (function) {
    case 0x0a4:
    case 0x5a4:
      OP(CMPTUN, F12_F3);

    case 0x0a5:
    case 0x5a5:
      OP(CMPTEQ, F12_F3);

    case 0x0a6:
    case 0x5a6:
      OP(CMPTLT, F12_F3);

    case 0x0a7:
    case 0x5a7:
      OP(CMPTLE, F12_F3);

    case 0x2ac:
    case 0x6ac:
      OP(CVTST, F2_F3);

    default:
      if (((function & 0x600) == 0x200) || ((function & 0x500) == 0x400)) {
        UNKNOWN2;
      }

      switch (function & 0x3f) {
      case 0x00:
        OP(ADDS, F12_F3);
      case 0x01:
        OP(SUBS, F12_F3);
      case 0x02:
        OP(MULS, F12_F3);
      case 0x03:
        OP(DIVS, F12_F3);
      case 0x20:
        OP(ADDT, F12_F3);
      case 0x21:
        OP(SUBT, F12_F3);
      case 0x22:
        OP(MULT, F12_F3);
      case 0x23:
        OP(DIVT, F12_F3);
      case 0x2c:
        OP(CVTTS, F2_F3);
      case 0x2f:
        OP(CVTTQ, F2_F3);
      case 0x3c:
        if ((function & 0x300) == 0x100) {
          UNKNOWN2;
        }
        OP(CVTQS, F2_F3);
      case 0x3e:
        if ((function & 0x300) == 0x100) {
          UNKNOWN2;
        }
        OP(CVTQT, F2_F3);
      default:
        UNKNOWN2;
      }
      break;
    }
    break;

  case 0x17: // FLTL* instructions (fltl_operate, which compiled code calls too)
    function = (ins >> 5) & 0x7ff;
    if (!fltl_operate(ins, (u32)function)) {
      UNKNOWN2;
    }
    ES40_EXECUTE_END();

  case 0x18: // MISC* instructions
    function = (ins & 0xffff);
    switch (function) {
    case 0x0000:
      OP(TRAPB, NOP);
    case 0x0400:
      OP(EXCB, NOP);
    case 0x4000:
      OP(MB, NOP);
    case 0x4400:
      OP(WMB, NOP);
    case 0x4800: // reserved for weaker barriers; until defined they "must
    case 0x4C00: // operate as a Memory Barrier instruction" (ARM C.13)
      OP(MB, NOP);
    case 0x8000:
      OP(FETCH, NOP);
    case 0xA000:
      OP(FETCH_M, NOP);
    case 0xC000:
      OP(RPCC, X_R1);
    case 0xE000:
      OP(RC, X_R1);
    case 0xE800:
      OP(ECB, NOP);
    case 0xF000:
      OP(RS, X_R1);
    case 0xF800:
      OP(WH64, NOP);
    case 0xFC00:
      OP(WH64EN, NOP);
    default:
      // No instruction here. The 21264 completes it as a no-op, where the
      // 21164 trapped; the architecture allows both (cpu-fidelity.md).
      // The JIT leaves these words to this switch.
      if (cpu_family_unused_misc_is_nop(m_model->family)) {
        ES40_EXECUTE_END();
      }
      UNKNOWN2;
    }
    break;

  case 0x19: // HW_MFPR (PALRES)
    /* HRM 6.4 / 6.8.2: PALRES opcodes (0x19/0x1B/0x1D/0x1E/0x1F) raise
     * OPCDEC unless executing in PALmode or in kernel mode with I_CTL[HWE]
     * set. Matches brokenpipe palres_access_check(). */
    if (!(state.pc & 1) && !(state.cm == 0 && state.hwe)) {
      GO_PAL(OPCDEC);
      ES40_EXECUTE_END();
    }
    function = (ins >> 8) & 0xff;
    OP(HW_MFPR, MFPR);

  case 0x1a: // JSR* instructions
    OP(JMP, JMP);

  case 0x1b: // PAL reserved - HW_LD (PALRES)
    if (!(state.pc & 1) && !(state.cm == 0 && state.hwe)) {
      GO_PAL(OPCDEC);
      ES40_EXECUTE_END();
    }
    function = (ins >> 12) & 0xf;
    if (function & 1) {
      OP(HW_LDQ, HW_LD);
    } else {
      OP(HW_LDL, HW_LD);
    }

  case 0x1c: // FPTI* instructions
    function = (ins >> 5) & 0x7f;
    switch (function) {
    case 0x00:
      OP(SEXTB, R2_R3);
    case 0x01:
      OP(SEXTW, R2_R3);
    case 0x30:
      OP(CTPOP, R2_R3);
    case 0x31:
      OP(PERR, R2_R3);
    case 0x32:
      OP(CTLZ, R2_R3);
    case 0x33:
      OP(CTTZ, R2_R3);
    case 0x34:
      OP(UNPKBW, R2_R3);
    case 0x35:
      OP(UNPKBL, R2_R3);
    case 0x36:
      OP(PKWB, R2_R3);
    case 0x37:
      OP(PKLB, R2_R3);
    case 0x38:
      OP(MINSB8, R12_R3);
    case 0x39:
      OP(MINSW4, R12_R3);
    case 0x3a:
      OP(MINUB8, R12_R3);
    case 0x3b:
      OP(MINUW4, R12_R3);
    case 0x3c:
      OP(MAXUB8, R12_R3);
    case 0x3d:
      OP(MAXUW4, R12_R3);
    case 0x3e:
      OP(MAXSB8, R12_R3);
    case 0x3f:
      OP(MAXSW4, R12_R3);
    case 0x70:
      OP(FTOIT, F1_R3);
    case 0x78:
      OP(FTOIS, F1_R3);
    default:
      UNKNOWN2;
    }
    break;

  case 0x1d: // HW_MTPR (PALRES)
    if (!(state.pc & 1) && !(state.cm == 0 && state.hwe)) {
      GO_PAL(OPCDEC);
      ES40_EXECUTE_END();
    }
    function = (ins >> 8) & 0xff;
    OP(HW_MTPR, MTPR);

  case 0x1e: // HW_RET (PALRES)
    if (!(state.pc & 1) && !(state.cm == 0 && state.hwe)) {
      GO_PAL(OPCDEC);
      ES40_EXECUTE_END();
    }
    OP(HW_RET, RET);

  case 0x1f: // HW_ST (PALRES)
    if (!(state.pc & 1) && !(state.cm == 0 && state.hwe)) {
      GO_PAL(OPCDEC);
      ES40_EXECUTE_END();
    }
    function = (ins >> 12) & 0xf;
    if (function & 1) {
      OP(HW_STQ, HW_ST);
    } else {
      OP(HW_STL, HW_ST);
    }

  case 0x20:
    OP(LDF, FMEM);

  case 0x21:
    OP(LDG, FMEM);

  case 0x22:
    OP(LDS, FMEM);

  case 0x23:
    OP(LDT, FMEM);

  case 0x24:
    OP(STF, FMEM);

  case 0x25:
    OP(STG, FMEM);

  case 0x26:
    OP(STS, FMEM);

  case 0x27:
    OP(STT, FMEM);

  case 0x28:
    OP(LDL, MEM);

  case 0x29:
    OP(LDQ, MEM);

  case 0x2a:
    OP(LDL_L, MEM);

  case 0x2b:
    OP(LDQ_L, MEM);

  case 0x2c:
    OP(STL, MEM);

  case 0x2d:
    OP(STQ, MEM);

  case 0x2e:
    OP(STL_C, MEM);

  case 0x2f:
    OP(STQ_C, MEM);

  case 0x30:
    OP(BR, BR);

  case 0x31:
    OP(FBEQ, FCOND);

  case 0x32:
    OP(FBLT, FCOND);

  case 0x33:
    OP(FBLE, FCOND);

  case 0x34:
    OP(BSR, BSR);

  case 0x35:
    OP(FBNE, FCOND);

  case 0x36:
    OP(FBGE, FCOND);

  case 0x37:
    OP(FBGT, FCOND);

  case 0x38:
    OP(BLBC, COND);

  case 0x39:
    OP(BEQ, COND);

  case 0x3a:
    OP(BLT, COND);

  case 0x3b:
    OP(BLE, COND);

  case 0x3c:
    OP(BLBS, COND);

  case 0x3d:
    OP(BNE, COND);

  case 0x3e:
    OP(BGE, COND);

  case 0x3f:
    OP(BGT, COND);

  default:
    UNKNOWN1;
  }

#ifndef ES40_JIT
  goto _next_instruction;
#else
  return;
#endif
}

/**
 * The FLTV (opcode 0x15, VAX floating point) and FLTL (0x17) operates with
 * function code `function`, for both engines: execute() above and compiled
 * code's helpers (jit_fltv, jit_fltl). Each DO_ macro starts with FPSTART,
 * which takes the FEN trap when floating point is disabled and leaves the
 * switch. False for a function code that is no operate (the interpreter
 * takes OPCDEC; classify() compiles none of those).
 **/
bool CAlphaCPU::fltv_operate(u32 ins, u32 function) {
  UFP ufp2;
  switch (function) {
  case 0x0a5:
  case 0x4a5:
    DO_CMPGEQ;
    break;
  case 0x0a6:
  case 0x4a6:
    DO_CMPGLT;
    break;
  case 0x0a7:
  case 0x4a7:
    DO_CMPGLE;
    break;
  case 0x03c:
  case 0x0bc:
    DO_CVTQF;
    break;
  case 0x03e:
  case 0x0be:
    DO_CVTQG;
    break;
  default:
    if (function & 0x200)
      return false;
    switch (function & 0x7f) {
    case 0x000:
      DO_ADDF;
      break;
    case 0x001:
      DO_SUBF;
      break;
    case 0x002:
      DO_MULF;
      break;
    case 0x003:
      DO_DIVF;
      break;
    case 0x01e:
      DO_CVTDG;
      break;
    case 0x020:
      DO_ADDG;
      break;
    case 0x021:
      DO_SUBG;
      break;
    case 0x022:
      DO_MULG;
      break;
    case 0x023:
      DO_DIVG;
      break;
    case 0x02c:
      DO_CVTGF;
      break;
    case 0x02d:
      DO_CVTGD;
      break;
    case 0x02f:
      DO_CVTGQ;
      break;
    default:
      return false;
    }
    break;
  }
  return true;
}

bool CAlphaCPU::fltl_operate(u32 ins, u32 function) {
  switch (function) {
  case 0x010:
    DO_CVTLQ;
    break;
  case 0x020:
    DO_CPYS;
    break;
  case 0x021:
    DO_CPYSN;
    break;
  case 0x022:
    DO_CPYSE;
    break;
  case 0x024:
    DO_MT_FPCR;
    break;
  case 0x025:
    DO_MF_FPCR;
    break;
  case 0x02a:
    DO_FCMOVEQ;
    break;
  case 0x02b:
    DO_FCMOVNE;
    break;
  case 0x02c:
    DO_FCMOVLT;
    break;
  case 0x02d:
    DO_FCMOVGE;
    break;
  case 0x02e:
    DO_FCMOVLE;
    break;
  case 0x02f:
    DO_FCMOVGT;
    break;
  case 0x030:
  case 0x130:
  case 0x530:
    DO_CVTQL;
    break;
  default:
    return false;
  }
  return true;
}

static u32 cpu_magic1 = 0x2126468C;
static u32 cpu_magic2 = 0xC8646212;

/**
 * Save state to a Virtual Machine State file.
 **/
int CAlphaCPU::SaveState(FILE *f) {
  long ss = sizeof(state);

  fwrite(&cpu_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&cpu_magic2, sizeof(u32), 1, f);
  printf("%s: %d bytes saved.\n", devid_string, (int)ss);
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CAlphaCPU::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  size_t r;

  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m1 != cpu_magic1) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }

  r = fread(&ss, sizeof(long), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m2 != cpu_magic2) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }

  printf("%s: %d bytes restored.\n", devid_string, (int)ss);
  last_dtb_virt[0] = last_dtb_virt[1] = 0;
  // The RPCC floor isn't part of the save-state format: rebase it on the
  // restored counter and a fresh host epoch so a restore can't jump the clock
  // or bill the paused wall time.
  cc_last_read = state.cc;
  cc_borrow = 0;
  cc_wall_remainder = 0;
  cc_last_sync = host_ticks();
  // RAM and TB state now belong to the restored state: drop the data page
  // cache, the sequential icache cursor and every compiled block (the epoch
  // bump makes them re-hash against the restored RAM before they run).
  flush_data_page_cache();
  break_seq_icache();
#ifdef ES40_JIT
  // The recognized idle and parked-loop heads are learned from RAM and are
  // NOT part of the saved state, so they would otherwise survive into a
  // restored image that has different code at those addresses -- and idle
  // pacing would then sleep on a PC that is not an idle loop. Re-learn them.
  tb_idx_rebuild(); // derived from the state just read
  m_idle_pc = m_park_pc = 0;
  m_idle_streak = 0;
  m_link_from = nullptr; // pending link request into pre-restore code
  if (m_jit)
    m_jit->flush();
#endif

  return 0;
}

/***************************************************************************/

/**
 * \name TB
 * Translation Buffer related functions
 ******************************************************************************/

//\{

/**
 * \brief Find translation-buffer entry
 *
 * Try to find a translation-buffer entry that maps the page inside which
 * the specified virtual address lies.
 *
 * \param virt    Virtual address to find in translation buffer.
 * \param flags   ACCESS_EXEC determines which translation buffer to use.
 * \return        Number of matching entry, or -1 if no match found.
 **/
int CAlphaCPU::FindTBEntry(u64 virt, int flags) {

  // Use ITB (tb[1]) if ACCESS_EXEC is set, otherwise the unified Dstream TB
  // (tb[0]).
  int t = (flags & ACCESS_EXEC) ? TB_INDEX_ITB : TB_INDEX_DATA;
  int asn = state.asn;

  int rw = (flags & ACCESS_WRITE) ? 1 : 0;

#define TB_ASN_MATCH(entry)                                                    \
  ((entry).asm_bit || ((entry).asn == ((t == TB_INDEX_ITB) ? asn : state.asn0)))

  // Try last match first; this is a good quess, especially in the ITB
  int i = state.last_found_tb[t][rw];
  if (state.tb[t][i].valid &&
      !((state.tb[t][i].virt ^ virt) & state.tb[t][i].match_mask) &&
      TB_ASN_MATCH(state.tb[t][i]))
    return i;

  // The exact index (see m_tb_idx): two validated probes, and a miss means
  // the page is not in the TB -- unless an entry with a granularity hint is
  // live, which the index cannot represent, or the switch is off.
  if (m_tb_gh_live[t] == 0) {
    const u8 *w = m_tb_idx[t][tb_idx_set(virt)];
#ifdef JIT_VERIFY
    m_tb_idx_probes++;
#endif
    for (int k = 0; k < kTbIdxWays && w[k]; k++) {
      i = (int)w[k] - 1;
      if (state.tb[t][i].valid &&
          !((state.tb[t][i].virt ^ virt) & state.tb[t][i].match_mask) &&
          TB_ASN_MATCH(state.tb[t][i])) {
        state.last_found_tb[t][rw] = i;
        return i;
      }
    }
    // An overflowed set: the entry may be live and only missing from the
    // index. Scan, and put a find back at the front of its set.
    if (m_tb_idx_lost[t][tb_idx_set(virt)]) {
      for (int j = 0; j < TB_ENTRIES; j++)
        if (state.tb[t][j].valid &&
            !((state.tb[t][j].virt ^ virt) & state.tb[t][j].match_mask) &&
            TB_ASN_MATCH(state.tb[t][j])) {
          tb_idx_insert(t, state.tb[t][j].virt, j);
          state.last_found_tb[t][rw] = j;
          return j;
        }
    }
#ifdef JIT_VERIFY
    // Oracle: the index is derived state no differential test can see, so
    // the scan checks every "absent" here. A find is a false negative: an
    // index bug, or three live pages in one set (counted, and served).
    for (int j = 0; j < TB_ENTRIES; j++)
      if (state.tb[t][j].valid &&
          !((state.tb[t][j].virt ^ virt) & state.tb[t][j].match_mask) &&
          TB_ASN_MATCH(state.tb[t][j])) {
        if (m_tb_idx_false_neg++ < 8)
          printf("%%CPU-W-TBINDEX: false negative va %016" PRIx64
                 " (tb %d slot %d, set %d ways %u/%u/%u/%u..)\n",
                 virt, t, j, tb_idx_set(virt), w[0], w[1], w[2], w[3]);
        state.last_found_tb[t][rw] = j;
        return j;
      }
#endif
  } else {
    // Otherwise, loop through the TB entries to find a match.
    for (i = 0; i < TB_ENTRIES; i++) {
      if (state.tb[t][i].valid &&
          !((state.tb[t][i].virt ^ virt) & state.tb[t][i].match_mask) &&
          TB_ASN_MATCH(state.tb[t][i])) {
        state.last_found_tb[t][rw] = i;
        return i;
      }
    }
  }

#undef TB_ASN_MATCH
  // Nothing in the TB. Before this becomes a DTB miss delivered to PALcode,
  // see whether PALcode inserted this page before and the TB merely evicted
  // it -- then put it back ourselves, as its handler would.
  if (t == TB_INDEX_DATA) {
    const int j = tb_refill_from_shadow(virt, state.asn0);
    if (j >= 0) {
      state.last_found_tb[t][rw] = j;
      return j;
    }
  }
  return -1;
}

// Refill the data TB from the shadow: the same slot choice as add_tb (a
// round-robin victim), the same eviction rule (the evicted page stays in the
// data page cache unless ALPHABOX_DPC_KEEP=0 -- see add_tb), then the entry
// copied back whole. Emptying it here too kept the page cache no larger than
// the TB on exactly the code that refills from the shadow on every access.
int CAlphaCPU::tb_refill_from_shadow(u64 virt, int asn) {
  const STBEntry &sh = m_tb_shadow[tb_shadow_index(virt)];
  if (!sh.valid || sh.virt != (virt & sh.match_mask) ||
      !(sh.asm_bit || sh.asn == asn))
    return -1;
  const int t = TB_INDEX_DATA;
  const int i = state.next_tb[t];
  state.next_tb[t] = (i + 1 == TB_ENTRIES) ? 0 : i + 1;
  const STBEntry victim = state.tb[t][i];
  if (victim.valid) {
    if (!m_dpc_keep)
      flush_data_page_cache_range(victim.virt, victim.match_mask);
    tb_idx_drop(t, i);
  }
  state.tb[t][i] = sh;
  tb_idx_add(t, i);
  if (m_dpc_keep)
    drop_unmapped_page(victim); // kept only while the shadow still has it
  m_tb_shadow_refills++;
  return i;
}

static inline u64 alpha_sext_u64_43(u64 a) {
  return (a & U64(0x0000040000000000)) ? (a | U64(0xfffff80000000000))
                                       : (a & U64(0x000007ffffffffff));
}

// Shared with the JIT's load and store helpers (AlphaCPU_jit.cpp), which have
// to reject a malformed virtual address before they translate one, exactly as
// virt2phys does. Declared in AlphaCPU.hpp.
bool alpha_valid_va_form(u64 virt, bool va48) {
  return (va48 ? sext_u64_48(virt) : alpha_sext_u64_43(virt)) == virt;
}

// ALPHABOX_IRQTRACE=<n>: log interrupt entries n..n+39 and the IER/SIRR/CM
// writes and ISUM reads between them (interrupt-storm diagnosis).
static const long long g_irqtrace_start = [] {
  const char *e = getenv("ALPHABOX_IRQTRACE");
  return e ? atoll(e) : -1LL;
}();
static std::atomic<long long> g_irqtrace_entries{0};
static std::atomic<bool> g_irqtrace_on{false};

void CAlphaCPU::irq_trace_entry() {
  if (g_irqtrace_start < 0)
    return;
  const long long n = ++g_irqtrace_entries;
  const bool on = n >= g_irqtrace_start && n < g_irqtrace_start + 40;
  g_irqtrace_on.store(on, std::memory_order_relaxed);
  if (on)
    printf("IRQT INT #%lld pc=%016llx cm=%d sir=%04x sien=%04x eir&eien=%02llx "
           "asten=%d aster=%x astrr=%x\n",
           n, (unsigned long long)state.pc, state.cm, state.sir, state.sien,
           (unsigned long long)(state.eir & state.eien), state.asten,
           state.aster, state.astrr);
}

void CAlphaCPU::irq_trace_ipr(const char *what, u32 fn, u64 val) {
  if (!g_irqtrace_on.load(std::memory_order_relaxed))
    return;
  printf("IRQT   %s %02x val=%016llx exc=%016llx -> cm=%d sir=%04x "
         "sien=%04x\n",
         what, fn, (unsigned long long)val, (unsigned long long)state.exc_addr,
         state.cm, state.sir, state.sien);
}

int CAlphaCPU::initiate_acv_fault(u64 virt, int flags, u32 ins) {
  state.exc_addr = state.current_pc;
  // An exception other than a TB miss clears the lock flag, as GO_PAL does.
  cSystem->cpu_clear_lock(state.iProcNum);

  if (flags & ACCESS_EXEC) {
    state.exc_sum = 0;
    set_pc(state.pal_base + IACV + 1);
    return -1;
  }

  state.fault_va = virt;
  state.va_form_va = virt;
  state.exc_sum = ((u64)REG_1 & 0x1f)
                  << 8; /* HRM 5.2.13: EXC_SUM REG[12:8] is 5 bits */

  u32 opcode = I_GETOP(ins);
  state.mm_stat = ((opcode == 0x1b || opcode == 0x1f) ? opcode - 0x18 : opcode)
                      << 4 |
                  (flags & ACCESS_WRITE) | 2;
  set_pc(state.pal_base + DFAULT + 1);
  return -1;
}

/**
 * \brief Translate a virtual address to a physical address.
 *
 * Translate a 64-bit virtual address into a 64-bit physical address, using
 * the page table buffers.
 *
 * The following steps are taken to resolve the address:
 *  - See if the address can be found in the translation buffer.
 *  - If not, try to load the right page table entry into the translation
 *    buffer, if this is not possible, trap to the OS.
 *  - Check access privileges.
 *  - Check fault bits.
 *  .
 *
 * \param virt    Virtual address to be translated.
 * \param phys    Pointer to where the physical address is to be returned.
 * \param flags   Set of flags that determine the exact functioning of the
 *                function. A combination of the following flags:
 *                  - ACCESS_READ   Data-read-access.
 *                  - ACCESS_WRITE  Data-write-access.
 *                  - ACCESS_EXEC   Code-read-access.
 *                  - NO_CHECK      Do not perform access checks.
 *                  - VPTE          VPTE access; if this misses, it's a double
 *miss.
 *                  - FAKE          Access is not initiated by executing code,
 *but by the debugger. If a translation can't be found through the translation
 *buffer, don't bother.
 *                  - ALT           Use alt_cm for access checks instead of cm.
 *                  .
 * \param asm_bit Status of the ASM (address space match) bit in the
 *page-table-entry. \param ins     Instruction currently being executed.
 *Important for the correct handling of traps.
 *
 * \return        0 on success, -1 if address could not be converted without
 *                help (in this case state.pc contains the address of the
 *                next instruction to execute (PALcode or OS entry point).
 **/
int CAlphaCPU::virt2phys(u64 virt, u64 *phys, int flags, bool *asm_bit,
                         u32 ins) {
  int t = (flags & ACCESS_EXEC) ? TB_INDEX_ITB : TB_INDEX_DATA;
  int i;

  int spe = (flags & ACCESS_EXEC) ? state.i_ctl_spe : state.m_ctl_spe;
  /*
   * Access-check current mode selection.
   *  - VPTE (HRM 6.4.1 Table 6-3): Virtual/VPTE accesses are LD_VPTE
   *    page-table fetches and use *kernel mode* for permission checks
   *    regardless of executing CM.
   *  - ALT (HRM 6.4.1 Table 6-3 row 1102, Table 6-4 row 1102): the
   *    /alt variants use DTB_ALT_MODE for permission checks.
   *  - Otherwise: current mode.
   * Order matters: VPTE wins over ALT (VPTE is never combined with ALT in
   * any valid HW_LD encoding, but the precedence is clear).
   */
  int cm = (flags & VPTE) ? 0 : (flags & ALT) ? state.alt_cm : state.cm;
  bool forreal = !(flags & FAKE);
  bool va48 = (flags & ACCESS_EXEC) ? (state.i_ctl_va_mode & 1)
                                    : (state.va_ctl_va_mode & 1);

#if defined(DEBUG_TB)
  if (forreal)
    printf("TB %" PRIx64 ",%x: ", virt, flags);
#endif

  if (!alpha_valid_va_form(virt, va48)) {
#if defined(DEBUG_TB)
    if (forreal)
      printf("acv-va-form\n");
#endif
    if (!forreal)
      return -1;
    return initiate_acv_fault(virt, flags, ins);
  }

  // try superpage first.
  if (spe) {
    bool spe_hit = false;
    u64 spe_phys = 0;

#if defined(DEBUG_TB)
    if (forreal)
      printf("try spe...");
#endif

    // HRM 5.3.9: SPE[2], when set, enables superpage mapping when VA[47:46]
    // = 2. In this mode, VA[43:13] are mapped directly to PA[43:13] and
    // VA[45:44] are ignored.
    if (((virt & SPE_2_MASK) == SPE_2_MATCH) && (spe & 4)) {
      spe_hit = true;
      spe_phys = virt & SPE_2_MAP;
    }

    // SPE[1], when set, enables superpage mapping when VA[47:41] = 7E. In
    // this mode, VA[40:13] are mapped directly to PA[40:13] and PA[43:41] are
    // copies of PA[40] (sign extension).
    else if (((virt & SPE_1_MASK) == SPE_1_MATCH) && (spe & 2)) {
      spe_hit = true;
      spe_phys = (virt & SPE_1_MAP) | ((virt & SPE_1_TEST) ? SPE_1_ADD : 0);
    }

    // SPE[0], when set, enables superpage mapping when VA[47:30] = 3FFFE.
    // In this mode, VA[29:13] are mapped directly to PA[29:13] and PA[43:30]
    // are cleared.
    else if (((virt & SPE_0_MASK) == SPE_0_MATCH) && (spe & 1)) {
      spe_hit = true;
      spe_phys = virt & SPE_0_MAP;
    }

    if (spe_hit) {
      if (cm) {
#if defined(DEBUG_TB)
        if (forreal)
          printf("SPE-ACV\n");
#endif
        if (!forreal)
          return -1;
        return initiate_acv_fault(virt, flags, ins);
      }

      *phys = spe_phys;
      // A superpage mapping doesn't involve the TB or the ASN at all, so it
      // matches every address space. Reporting it as ASM lets instruction
      // cache lines survive a process switch and lets the JIT keep one
      // compiled copy of kernel code for all processes (reporting it per-ASN
      // recompiled the NT kernel on every context switch).
      if (asm_bit)
        *asm_bit = true;
#if defined(DEBUG_TB)
      if (forreal)
        printf("SPE\n");
#endif
      return 0;
    }
  }

  // try to find it in the translation buffer
  i = FindTBEntry(virt, flags);

  if (i < 0) // not found: trap to PALcode
  {
    if (!forreal) // debugger-lookup of the address
      return -1;  // report failure, and don't look any further
    // transfer execution to PALcode
    state.exc_addr = state.current_pc;
    if (flags & VPTE) {
      // HRM 5.1.3: VA is NOT written for LD_VPTE misses
      state.va_form_va = virt;
      state.exc_sum = ((u64)REG_1 & 0x1f)
                      << 8; /* HRM 5.2.13: EXC_SUM REG[12:8] is 5 bits */
      /*
       * I_CTL[VA_48] selects the DTB double-miss PAL entry.
       * state.i_ctl_va_mode packs bits [16:15] of I_CTL, so bit 0
       * here is the architectural VA_48 bit.
       */
      set_pc(state.pal_base +
             ((state.i_ctl_va_mode & 1) ? DTBM_DOUBLE_4 : DTBM_DOUBLE_3) + 1);
    } else if (flags & ACCESS_EXEC) {
      set_pc(state.pal_base + ITB_MISS + 1);
    } else {
      state.fault_va = virt;
      state.va_form_va = virt;
      state.exc_sum = ((u64)REG_1 & 0x1f)
                      << 8; /* HRM 5.2.13: EXC_SUM REG[12:8] is 5 bits */

      u32 opcode = I_GETOP(ins);
      state.mm_stat =
          ((opcode == 0x1b || opcode == 0x1f) ? opcode - 0x18 : opcode) << 4 |
          (flags & ACCESS_WRITE);
      set_pc(state.pal_base + DTBM_SINGLE + 1);
    }

    return -1;
  }

  // If we get here, the number of the matching TB entry is in i.
#if defined(DEBUG_TB)
  else {
    if (forreal)
      printf("entry %d - ", i);
  }
#endif
  if (!(flags & NO_CHECK)) {

    // check if requested access is allowed.
    // WRCHK (HRM 6.4.1: HW_LD type 1012/1112 WrChk variants) requires that
    // BOTH read and write protection pass for the access mode -- fail if
    // the natural-direction access bit is clear OR (when WRCHK is set on a
    // read) the write access bit is also clear.
    if (!state.tb[t][i].access[flags & ACCESS_WRITE][cm] ||
        ((flags & WRCHK) && !state.tb[t][i].access[1][cm])) {
#if defined(DEBUG_TB)
      if (forreal)
        printf("acv\n");
#endif
      if (!forreal) // FAKE probe: report failure, don't vector the fault —
                    // caller re-runs it under the interpreter
        return -1;
      if (flags & ACCESS_EXEC) {

        // handle I-stream access violation
        state.exc_addr = state.current_pc;
        cSystem->cpu_clear_lock(state.iProcNum); // as GO_PAL does
        state.exc_sum = 0;
        set_pc(state.pal_base + IACV + 1);
        return -1;
      } else {

        // Handle D-stream access violation
        state.exc_addr = state.current_pc;
        cSystem->cpu_clear_lock(state.iProcNum); // as GO_PAL does
        state.fault_va = virt;
        /* HRM 5.1.5/D.24: VA_FORM is derived from VA, which is written
           on every D-stream fault (incl. DFAULT), not just TB miss.
           Keep the VA_FORM snapshot current so the OS fault handler
           computes the right PTE. */
        state.va_form_va = virt;
        state.exc_sum = ((u64)REG_1 & 0x1f)
                        << 8; /* HRM 5.2.13: EXC_SUM REG[12:8] is 5 bits */

        u32 opcode = I_GETOP(ins);
        state.mm_stat =
            ((opcode == 0x1b || opcode == 0x1f) ? opcode - 0x18 : opcode) << 4 |
            (flags & ACCESS_WRITE) | 2;
        set_pc(state.pal_base + DFAULT + 1);
        return -1;
      }
    }

    // check if requested access doesn't fault.
    // WRCHK additionally requires that the FOW bit be clear -- HRM 6.4.1
    // WrChk variants check both FOR and FOW.
    if (state.tb[t][i].fault[flags & ACCESS_MODE] ||
        ((flags & WRCHK) && state.tb[t][i].fault[1])) {
#if defined(DEBUG_TB)
      if (forreal)
        printf("fault\n");
#endif
      if (!forreal) // FAKE probe: report failure, don't vector the fault —
                    // caller re-runs it under the interpreter
        return -1;
      if (flags & ACCESS_EXEC) {

        // handle I-stream access fault
        state.exc_addr = state.current_pc;
        cSystem->cpu_clear_lock(state.iProcNum); // as GO_PAL does
        state.exc_sum = 0;
        set_pc(state.pal_base + IACV + 1);
        return -1;
      } else {

        // handle D-stream access fault
        state.exc_addr = state.current_pc;
        cSystem->cpu_clear_lock(state.iProcNum); // as GO_PAL does
        state.fault_va = virt;
        /* HRM 5.1.5/D.24: VA_FORM is derived from VA, which is written
           on every D-stream fault (including FOR/FOW DFAULT), not just TB
           miss. Keep the VA_FORM snapshot current so the OS fault
           handler computes the right PTE address. */
        state.va_form_va = virt;
        state.exc_sum = ((u64)REG_1 & 0x1f)
                        << 8; /* HRM 5.2.13: EXC_SUM REG[12:8] is 5 bits */

        /* HRM 5.3.8 MM_STAT: FOR [bit 2] is set when a fault-on-read
         * error occurs during a read transaction with PTE[FOR] set.
         * FOW [bit 3] is set when a fault-on-write error occurs.
         * For HW_LD WrChk variants the chip is performing both a
         * read AND a write-protection check, so PTE[FOW] (if set)
         * is reportable too
         */
        u32 opcode = I_GETOP(ins);
        int for_bit =
            ((flags & ACCESS_WRITE) == 0) && state.tb[t][i].fault[0] ? 4 : 0;
        int fow_bit = (((flags & ACCESS_WRITE) || (flags & WRCHK)) &&
                       state.tb[t][i].fault[1])
                          ? 8
                          : 0;
        state.mm_stat =
            ((opcode == 0x1b || opcode == 0x1f) ? opcode - 0x18 : opcode) << 4 |
            (flags & ACCESS_WRITE) | for_bit | fow_bit;
        set_pc(state.pal_base + DFAULT + 1);
        return -1;
      }
    }
  }

  // No access violations or faults
  // Return the converted address
  *phys = state.tb[t][i].phys | (virt & state.tb[t][i].keep_mask);
  if (asm_bit)
    *asm_bit = state.tb[t][i].asm_bit ? true : false;

#if defined(DEBUG_TB)
  if (forreal)
    printf("phys: %" PRIx64 " - OK\n", *phys);
#endif
  return 0;
}

/*
 * EV68CB/EV68DC HRM:
 *  - 5.3.1, Figure 5-26: DTB_TAG0/1 contain VA[47:13].
 *  - 5.3.2, Figure 5-27: DTB_PTE0/1 contain PA[43:13] and GH[1:0].
 * GH widens the granule by 3 bits per step, so the low 13/16/19/22 bits
 * are kept from the virtual address and excluded from the tag/PFN masks.
 */
#define GH_0_MATCH U64(0x0000ffffffffe000) /* VA <47:13> */
#define GH_0_PHYS U64(0x00000fffffffe000)  /* PA <43:13> */
#define GH_0_KEEP U64(0x0000000000001fff)  /* VA <12:0>  */

#define GH_1_MATCH U64(0x0000ffffffff0000) /* VA <47:16> */
#define GH_1_PHYS U64(0x00000fffffff0000)  /* PA <43:16> */
#define GH_1_KEEP U64(0x000000000000ffff)  /* VA <15:0>  */
#define GH_2_MATCH U64(0x0000fffffff80000) /* VA <47:19> */
#define GH_2_PHYS U64(0x00000ffffff80000)  /* PA <43:19> */
#define GH_2_KEEP U64(0x000000000007ffff)  /* VA <18:0>  */
#define GH_3_MATCH U64(0x0000ffffffc00000) /* VA <47:22> */
#define GH_3_PHYS U64(0x00000fffffc00000)  /* PA <43:22> */
#define GH_3_KEEP U64(0x00000000003fffff)  /* VA <21:0>  */

/**
 * \brief Add translation-buffer entry
 *
 * Add a translation-buffer entry to one of the translation buffers.
 *
 * \param virt    Virtual address.
 * \param pte     Translation in DTB_PTE format (see add_tb_d).
 * \param flags   ACCESS_EXEC determines which translation buffer to use.
 * \param asn     Address space number latched by the PAL fill port.
 **/
void CAlphaCPU::tb_idx_drop(int t, int slot) {
  if (state.tb[t][slot].match_mask == GH_0_MATCH)
    tb_idx_remove(t, state.tb[t][slot].virt, slot);
  else
    m_tb_gh_live[t]--;
}

void CAlphaCPU::tb_idx_add(int t, int slot) {
  if (state.tb[t][slot].match_mask == GH_0_MATCH)
    tb_idx_insert(t, state.tb[t][slot].virt, slot);
  else
    m_tb_gh_live[t]++;
}

void CAlphaCPU::tb_idx_rebuild() {
  memset(m_tb_idx, 0, sizeof(m_tb_idx));
  memset(m_tb_idx_lost, 0, sizeof(m_tb_idx_lost));
  m_tb_gh_live[0] = m_tb_gh_live[1] = 0;
  for (int t = 0; t < 2; t++)
    for (int i = 0; i < TB_ENTRIES; i++)
      if (state.tb[t][i].valid)
        tb_idx_add(t, i);
}

bool CAlphaCPU::dtb_still_maps(const STBEntry &e) const {
  // Only 8 KB entries are kept anywhere but the TB; a larger page that left
  // the TB is gone.
  if (e.match_mask != GH_0_MATCH)
    return false;
  const auto same = [&e](const STBEntry &x) {
    return x.valid && x.match_mask == GH_0_MATCH && x.virt == e.virt &&
           (x.asm_bit ? e.asm_bit : (!e.asm_bit && x.asn == e.asn));
  };
  if (same(m_tb_shadow[tb_shadow_index(e.virt)]))
    return true;
  // The TB, through its index: every 8 KB entry is in it.
  const u8 *w = m_tb_idx[TB_INDEX_DATA][tb_idx_set(e.virt)];
  for (int k = 0; k < kTbIdxWays && w[k]; k++)
    if (same(state.tb[TB_INDEX_DATA][w[k] - 1]))
      return true;
  if (m_tb_idx_lost[TB_INDEX_DATA][tb_idx_set(e.virt)])
    for (int j = 0; j < TB_ENTRIES; j++)
      if (same(state.tb[TB_INDEX_DATA][j]))
        return true;
  return false;
}

void CAlphaCPU::add_tb(u64 virt, u64 pte_phys, u64 pte_flags, int flags,
                       int asn) {
  int t = (flags & ACCESS_EXEC) ? TB_INDEX_ITB : TB_INDEX_DATA;
  int rw = (flags & ACCESS_WRITE) ? 1 : 0;
  u64 match_mask = 0;
  u64 keep_mask = 0;
  u64 phys_mask = 0;
  int i;

  switch (pte_flags & 0x60) // granularity hint
  {
  case 0:
    match_mask = GH_0_MATCH;
    phys_mask = GH_0_PHYS;
    keep_mask = GH_0_KEEP;
    break;

  case 0x20:
    match_mask = GH_1_MATCH;
    phys_mask = GH_1_PHYS;
    keep_mask = GH_1_KEEP;
    break;

  case 0x40:
    match_mask = GH_2_MATCH;
    phys_mask = GH_2_PHYS;
    keep_mask = GH_2_KEEP;
    break;

  case 0x60:
    match_mask = GH_3_MATCH;
    phys_mask = GH_3_PHYS;
    keep_mask = GH_3_KEEP;
    break;
  }

  i = -1;
  for (int j = 0; j < TB_ENTRIES; j++) {
    if (state.tb[t][j].valid &&
        !((state.tb[t][j].virt ^ virt) & state.tb[t][j].match_mask) &&
        (state.tb[t][j].asm_bit || state.tb[t][j].asn == asn)) {
      i = j;
      break;
    }
  }

#ifdef ES40_JIT
  // A same-(virt,asn) ITB overwrite with a DIFFERENT physical is a code-page
  // remap performed WITHOUT a separate TBIS; chained JIT blocks compiled from
  // the old mapping must re-validate, so bump the generation. A next_tb
  // eviction (i reassigned below) replaces a DIFFERENT vpage (not a remap of
  // this one) and must NOT bump.
  const bool itb_remap = (t == TB_INDEX_ITB) && (i >= 0) &&
                         (state.tb[t][i].phys != (pte_phys & phys_mask));
#endif

  if (i < 0) {
    i = state.next_tb[t];
    state.next_tb[t]++;
    if (state.next_tb[t] == TB_ENTRIES)
      state.next_tb[t] = 0;
  }
  // The entry being replaced (a same-page refill, or the round-robin victim):
  // its cached data translation must not outlive it.
  const bool old_valid = state.tb[t][i].valid;
  const u64 old_virt = state.tb[t][i].virt;
  const u64 old_mask = state.tb[t][i].match_mask;
  const STBEntry old_entry = state.tb[t][i];
  STBEntry old_shadow{}; // what the shadow slot held, if this fill replaces it

  state.tb[t][i].match_mask = match_mask;
  state.tb[t][i].keep_mask = keep_mask;
  state.tb[t][i].virt = virt & match_mask;
  state.tb[t][i].phys = pte_phys & phys_mask;
  state.tb[t][i].fault[0] = (int)pte_flags & 2;
  state.tb[t][i].fault[1] = (int)pte_flags & 4;
  state.tb[t][i].fault[2] = (int)pte_flags & 8;
  state.tb[t][i].access[0][0] = (int)pte_flags & 0x100;
  state.tb[t][i].access[1][0] = (int)pte_flags & 0x1000;
  state.tb[t][i].access[0][1] = (int)pte_flags & 0x200;
  state.tb[t][i].access[1][1] = (int)pte_flags & 0x2000;
  state.tb[t][i].access[0][2] = (int)pte_flags & 0x400;
  state.tb[t][i].access[1][2] = (int)pte_flags & 0x4000;
  state.tb[t][i].access[0][3] = (int)pte_flags & 0x800;
  state.tb[t][i].access[1][3] = (int)pte_flags & 0x8000;
  state.tb[t][i].asm_bit = (int)pte_flags & 0x10;
  state.tb[t][i].asn = asn;
  state.tb[t][i].valid = true;
  if (old_valid) { // the slot's previous page leaves the index
    if (old_mask == GH_0_MATCH)
      tb_idx_remove(t, old_virt, i);
    else
      m_tb_gh_live[t]--;
  }
  tb_idx_add(t, i);
  // Keep an 8 KB data translation in the shadow (see m_tb_shadow): what the
  // TB evicts later can then be refilled without a trap to PALcode.
  if (t == TB_INDEX_DATA && match_mask == GH_0_MATCH) {
    STBEntry &sh = m_tb_shadow[tb_shadow_index(virt)];
    old_shadow = sh;
    sh = state.tb[t][i];
  }
  state.last_found_tb[t][rw] = i;

#ifdef ES40_JIT
  if (itb_remap && m_jit)
    m_jit->note_itb_invalidate(
        CJitEngine::EPOCH_REMAP); // code page remapped in place -> chains
                                  // re-validate
#endif

  if (t == TB_INDEX_DATA) {
    // Only the new entry's page can change translation. The entry it evicts
    // translates exactly as before: a page cache that keeps it is a TB that
    // held it longer, which the architecture allows -- software must
    // invalidate after changing a PTE, and tbis/tbia/tbiap still empty the
    // cache. It is the rule the translation shadow (m_tb_shadow) already
    // rests on. Emptying it here bounded the page cache by the 128-entry
    // DTB: on makecab 43% of the inline probe's misses found such an empty
    // slot (docs/performance.md). ALPHABOX_DPC_KEEP=0 restores it.
    //
    // Held longer, but not past the point where the DTB itself would miss:
    // the evicted entry, and what this fill pushed out of the shadow, stay
    // cached only while the TB or the shadow still has them (see
    // dtb_still_maps).
    if (old_valid && !m_dpc_keep)
      flush_data_page_cache_range(old_virt, old_mask);
    else if (old_valid)
      drop_unmapped_page(old_entry);
    if (m_dpc_keep)
      drop_unmapped_page(old_shadow);
    flush_data_page_cache_range(virt, match_mask);
  }

#if defined(DEBUG_TB_)
  {
    printf("Add TB---------------------------------------\n");
    printf("Map VIRT    %016" PRIx64 "\n", state.tb[t][i].virt);
    printf("Matching    %016" PRIx64 "\n", state.tb[t][i].match_mask);
    printf("And keeping %016" PRIx64 "\n", state.tb[t][i].keep_mask);
    printf("To PHYS     %016" PRIx64 "\n", state.tb[t][i].phys);
    printf("Read : %c%c%c%c %c\n", state.tb[t][i].access[0][0] ? 'K' : '-',
           state.tb[t][i].access[0][1] ? 'E' : '-',
           state.tb[t][i].access[0][2] ? 'S' : '-',
           state.tb[t][i].access[0][3] ? 'U' : '-',
           state.tb[t][i].fault[0] ? 'F' : '-');
    printf("Write: %c%c%c%c %c\n", state.tb[t][i].access[1][0] ? 'K' : '-',
           state.tb[t][i].access[1][1] ? 'E' : '-',
           state.tb[t][i].access[1][2] ? 'S' : '-',
           state.tb[t][i].access[1][3] ? 'U' : '-',
           state.tb[t][i].fault[1] ? 'F' : '-');
    printf("Exec : %c%c%c%c %c\n", state.tb[t][i].access[1][0] ? 'K' : '-',
           state.tb[t][i].access[1][1] ? 'E' : '-',
           state.tb[t][i].access[1][2] ? 'S' : '-',
           state.tb[t][i].access[1][3] ? 'U' : '-',
           state.tb[t][i].fault[1] ? 'F' : '-');
  }
#endif
}

/**
 * \brief Add translation-buffer entry to the DTB
 *
 * The format of the PTE field is:
 * \code
 *   63 62           32 31     16  15  14  13  12  11  10  9   8  7 6  5  4  3
 *2   1  0
 *  +--+---------------+---------+---+---+---+---+---+---+---+---+-+----+---+-+---+---+-+
 *  |  |  PA <43:13>   |         |UWE|SWE|EWE|KWE|URE|SRE|ERE|KRE| | GH |ASM|
 *|FOW|FOR| |
 *  +--+---------------+---------+---+---+---+---+---+---+---+---+-+----+---+-+---+---+-+
 *                               +-------------------------------+    |   |
 *+-------+ |        |   |       |
 *  (user,supervisor,executive,kernel)(read,write)enable ----+        |   | |
 *                                              granularity hint -----+   | |
 *                                               address space match -----+ |
 *                                                      fault-on-(read,write)
 *----+ \endcode
 *
 * \param virt    Virtual address.
 * \param pte     Translation in DTB_PTE format.
 * \param dtb     DTB fill port number (0 = DTB_PTE0, 1 = DTB_PTE1).
 **/
void CAlphaCPU::add_tb_d(u64 virt, u64 pte, int dtb) {
  add_tb(virt, pte >> (32 - 13), pte, ACCESS_READ,
         dtb ? state.asn1 : state.asn0);
}

/**
 * \brief Add translation-buffer entry to the ITB
 *
 * The format of the PTE field is:
 * \code
 *   63              44 43           13 12  11  10  9   8  7 6  5  4  3   0
 *  +------------------+---------------+--+---+---+---+---+-+----+---+-----+
 *  |                  |  PA <43:13>   |  |URE|SRE|ERE|KRE| | GH |ASM|     |
 *  +------------------+---------------+--+---+---+---+---+-+----+---+-----+
 *                                        +---------------+    |   |
 *                                                    |        |   |
 *  (user,supervisor,executive,kernel)read enable ----+        |   |
 *                                       granularity hint -----+   |
 *                                        address space match -----+
 *
 * \endcode
 *
 * \param virt    Virtual address.
 * \param pte     Translation in ITB_PTE format.
 **/
void CAlphaCPU::add_tb_i(u64 virt, u64 pte) {
  add_tb(virt, pte, pte & 0xf70, ACCESS_EXEC, state.asn);
}

/**
 * \brief Invalidate all translation-buffer entries
 *
 * Invalidate all translation-buffer entries in one of the translation buffers.
 *
 * \param flags   ACCESS_EXEC determines which translation buffer to use.
 **/
void CAlphaCPU::tbia(int flags) {
  // The shadow of data translations is a translation cache too: it obeys
  // every invalidate the TB does. Selected from flags exactly as t is below.
  if (!(flags & ACCESS_EXEC))
    for (int k = 0; k < kTbShadowEntries; k++)
      m_tb_shadow[k].valid = false;


  int t = (flags & ACCESS_EXEC) ? TB_INDEX_ITB : TB_INDEX_DATA;
  int i;
  for (i = 0; i < TB_ENTRIES; i++)
    state.tb[t][i].valid = false;
  memset(m_tb_idx[t], 0, sizeof(m_tb_idx[t]));
  memset(m_tb_idx_lost[t], 0, sizeof(m_tb_idx_lost[t]));
  m_tb_gh_live[t] = 0;
  state.last_found_tb[t][0] = 0;
  state.last_found_tb[t][1] = 0;
  state.next_tb[t] = 0;
  if (t == TB_INDEX_DATA)
    flush_data_page_cache();
#ifdef ES40_JIT
  else if (m_jit)
    m_jit->note_itb_invalidate(
        CJitEngine::EPOCH_TBIA); // whole ITB cleared -> indirect chains
                                 // re-validate phys
#endif
}

/**
 * \brief Invalidate all process-specific translation-buffer entries
 *
 * Invalidate all translation-buffer entries that do not have the ASM bit
 * set in one of the translation buffers.
 *
 * \param flags   ACCESS_EXEC determines which translation buffer to use.
 **/
void CAlphaCPU::tbiap(int flags) {
  if (!(flags & ACCESS_EXEC))
    for (int k = 0; k < kTbShadowEntries; k++)
      if (!m_tb_shadow[k].asm_bit)
        m_tb_shadow[k].valid = false;


  int t = (flags & ACCESS_EXEC) ? TB_INDEX_ITB : TB_INDEX_DATA;
  int i;
  for (i = 0; i < TB_ENTRIES; i++)
    if (!state.tb[t][i].asm_bit && state.tb[t][i].valid) {
      state.tb[t][i].valid = false;
      tb_idx_drop(t, i);
    }

  if (t == TB_INDEX_DATA)
    flush_data_page_cache();
#ifdef ES40_JIT
  else if (m_jit)
    m_jit->note_itb_invalidate(
        CJitEngine::EPOCH_TBIAP); // process ITB entries cleared -> chains
                                  // re-validate phys
#endif
}

/**
 * \brief Invalidate single translation-buffer entry
 *
 * \param virt    Virtual address for which the entry should be invalidated.
 * \param flags   ACCESS_EXEC determines which translation buffer to use.
 **/
void CAlphaCPU::tbis(u64 virt, int flags) {
  // Over-invalidating the shadow is always safe (it costs one PALcode
  // refill), so match on the page alone here; tbis_d matches by ASN too.
  if (!(flags & ACCESS_EXEC)) {
    STBEntry &sh = m_tb_shadow[tb_shadow_index(virt)];
    if (sh.valid && sh.virt == (virt & sh.match_mask))
      sh.valid = false;
  }


  int t = (flags & ACCESS_EXEC) ? TB_INDEX_ITB : TB_INDEX_DATA;

  if (t == TB_INDEX_DATA) {
    tbis_d(virt, state.asn0);
    if (state.asn1 != state.asn0)
      tbis_d(virt, state.asn1);
    return;
  }

  int i = FindTBEntry(virt, flags);
  if (i >= 0) {
    state.tb[t][i].valid = false;
    tb_idx_drop(t, i);
  }
#ifdef ES40_JIT
  // A TBIS signals the OS is changing this code page's mapping. Bump the JIT
  // generation even when the entry wasn't currently cached (i<0, already
  // evicted from the TB), a JIT block compiled from this page is still stale
  // and MUST re-validate before being chained.
  if (m_jit)
    m_jit->note_itb_invalidate(CJitEngine::EPOCH_TBIS);
#endif
}

void CAlphaCPU::tbis_d(u64 virt, int asn) {
  {
    STBEntry &sh = m_tb_shadow[tb_shadow_index(virt)];
    if (sh.valid && sh.virt == (virt & sh.match_mask) &&
        (sh.asm_bit || sh.asn == asn))
      sh.valid = false;
  }

  int i;

  for (i = 0; i < TB_ENTRIES; i++) {
    if (state.tb[TB_INDEX_DATA][i].valid &&
        !((state.tb[TB_INDEX_DATA][i].virt ^ virt) &
          state.tb[TB_INDEX_DATA][i].match_mask) &&
        (state.tb[TB_INDEX_DATA][i].asm_bit ||
         state.tb[TB_INDEX_DATA][i].asn == asn)) {
      state.tb[TB_INDEX_DATA][i].valid = false;
      tb_idx_drop(TB_INDEX_DATA, i);
      flush_data_page_cache_range(state.tb[TB_INDEX_DATA][i].virt,
                                  state.tb[TB_INDEX_DATA][i].match_mask);
    }
  }

  // The page itself, whether or not an entry still held it.
  flush_data_page_cache_range(virt, U64(0xfffffffffffff000) | U64(0x2000));
}

//\}

/**
 * \brief Enable i-cache regardles of config file.
 *
 * Required for SRM-ROM decompression.
 **/
void CAlphaCPU::enable_icache() { icache_enabled = true; }

/**
 * \brief Restore i-cache after temporary ROM decompression setup.
 **/
void CAlphaCPU::restore_icache() { icache_enabled = true; }

bool CAlphaCPU::run_until_below(u64 floor, int budget) {
#ifdef ES40_JIT
  // The dispatcher stops at the floor (jit_run); compiled code runs between.
  m_stop_below = floor;
  jit_run(budget);
  m_stop_below = 0;
#else
  // The interpreter checks nothing between the instructions of a batch, so
  // take the batch down to one instruction for the while.
  m_exec_batch = 2;
  while (budget-- > 0 && get_clean_pc() >= floor)
    execute();
  m_exec_batch = 512;
#endif
  return get_clean_pc() < floor;
}
