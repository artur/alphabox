/* ES40 emulator.
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2026 Artur Goulão
 *
 * WWW    : http://www.es40.org
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

//#define CONSTANT_TIME_FACTOR 100

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
#include "TraceEngine.hpp"
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
#include "lockstep.hpp"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <set>
#include <utility>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h> // _mm_setcsr: pin host MXCSR for the JIT SSE FP path
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <cfenv> // fesetenv: pin the host FPCR for the JIT FP path
#endif

void CAlphaCPU::release_threads() {
  try {
    mySemaphore.set();
  } catch (const std::overflow_error &) {
    // Already signaled, nothing to do
  }
}

thread_local CAlphaCPU *t_running_cpu = nullptr;

bool CAlphaCPU::s_trace_calls = false;

/**
 * Report a subroutine call, the first time each call site reaches each
 * routine (ALPHABOX_TRACE_CALLS).
 *
 * Bringing up a console is largely the question "which of its routines ran,
 * and which did not": a firmware image carries a name for every routine
 * (docs/platforms/ds10.md shows how to recover them), so this reads as the
 * console's own call graph. One line per pair keeps a whole boot readable.
 * Interpreter only -- compiled blocks do not pass through here.
 **/
void CAlphaCPU::trace_call(u64 from, u64 to) {
  static std::mutex lock;
  static std::set<std::pair<u64, u64>> seen;

  std::lock_guard<std::mutex> guard(lock);
  if (!seen.insert(std::make_pair(from, to)).second)
    return;
  printf("%%CPU-T-CALL: cpu%d %011" PRIx64 " -> %011" PRIx64 "\n", get_cpuid(),
         from, to);
}

void CAlphaCPU::run() {
  try {
    t_running_cpu = this;
#ifdef __APPLE__
    // ALPHABOX_CPU_QOS=1: ask for the interactive QoS class on this thread.
    // A std::thread starts at the default class, and on Apple Silicon that
    // lets the scheduler place a long-running compute thread on an efficiency
    // core -- an experiment hook to find out whether the guest CPU lands on a
    // performance core at all before anything else about its speed is judged.
    if (getenv("ALPHABOX_CPU_QOS"))
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    mySemaphore.wait();
    while (state.wait_for_start) {
      if (StopThread)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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

    // Re-base the timing-calibration epoch to when execution actually begins:
    // a secondary parks (wait_for_start) before the primary releases it, so
    // leaving start_time at init time makes check_state() derive a wildly
    // wrong cc_per_instruction (huge elapsed wall-time vs ~0 instructions).
    start_time = std::chrono::steady_clock::now();
    next_timer_fire = start_time;
    tick_last_fire = start_time;
    cc_last_sync = host_ticks();
    cc_large = 0;
    state.instruction_count = 0;
    prev_icount = 0;
    prev_cc = 0;
    prev_time = 0;

    for (;;) {
      if (StopThread)
        return;
#ifdef ES40_JIT
      jit_run(2000);
#else
      // execute() runs a 512-instruction batch itself; calling it 2000 times
      // here made one scheduler turn ~1M instructions, starving the other CPUs
      // and device threads.
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

/**
 * Constructor.
 **/
CAlphaCPU::CAlphaCPU(CConfigurator *cfg, CSystem *system)
    : CSystemComponent(cfg, system), mySemaphore(0, 1) {
  s_trace_calls = getenv("ALPHABOX_TRACE_CALLS") != nullptr;
  // The configuration class names the part ("ev68cb").
  m_model = find_cpu_model(cfg->get_myValue());
  if (!m_model)
    FAILURE_1(Configuration, "Unknown Alpha processor %s", cfg->get_myValue());
  // Native PALcode vs the vmspal replacement routines is a system-wide choice
  // (mixing them across CPUs in SMP is unsafe), decided while the CPUs are
  // constructed and read back in init(), which runs after all of them.
#ifdef ES40_JIT
  // The JIT compiles PALcode like any other guest code: no vmspal shortcuts.
  system->request_native_pal("JIT build");
#else
  if (cfg->get_bool_value("palcode.vms.nohle", false)) {
    char why[96];
    snprintf(why, sizeof(why), "palcode.vms.nohle set on %s",
             cfg->get_myName());
    system->request_native_pal(why);
  }
#endif
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

void CAlphaCPU::init() {
  memset(&state, 0, sizeof(state));
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
  // Decided for all CPUs at construction (see the constructor).
  vmspal_lle_enabled = cSystem->native_pal_requested();

  state.iProcNum = cSystem->RegisterCPU(this);

#ifdef ES40_JIT
  if (!m_jit) {
    m_jit = new CJitEngine((int)state.iProcNum);
    m_jit->set_cpu_identity(m_model->amask, m_model->implver);
    m_jit->set_dpc_flush_counter(&m_stat_dpc_flushes);
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
    o.dpc_write_row = (uint32_t)((char *)&data_page_cache[1][0] -
                                 (char *)&data_page_cache[0][0]);
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
  state.i_ctl_other = U64(0x502086);
  state.smc = 1;

  // SROM imitation...
  add_tb(0, 0, U64(0xff61), ACCESS_READ, state.asn0);

#if defined(IDB)
  bListing = false;
#endif
  myThread = nullptr;

  cc_large = 0;
  prev_cc = 0;
  start_cc = 0;
  prev_time = 0;
  prev_icount = 0;
  start_icount = 0;

  start_time = std::chrono::steady_clock::now();
  next_timer_fire = start_time;

#if defined(CONSTANT_TIME_FACTOR)
  cc_per_instruction = CONSTANT_TIME_FACTOR;
#else
  cc_per_instruction = 70;
#endif

  state.r[22] = state.r[22 + 32] = state.iProcNum;

  dram_ptr = cSystem->PtrToMem(0);
  dram_size = U64(1) << cSystem->get_memory_bits();

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
}

void CAlphaCPU::ResetForSystemReset() {
  const int savedProcNum = state.iProcNum;

  memset(&state, 0, sizeof(state));
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
  state.i_ctl_other = U64(0x502086);
  state.smc = 1;

  // SROM imitation...
  add_tb(0, 0, U64(0xff61), ACCESS_READ, state.asn0);

  myThread = nullptr;

  cc_large = 0;
  prev_cc = 0;
  start_cc = 0;
  prev_time = 0;
  prev_icount = 0;
  start_icount = 0;

  start_time = std::chrono::steady_clock::now();
  next_timer_fire = start_time;

#if defined(CONSTANT_TIME_FACTOR)
  cc_per_instruction = CONSTANT_TIME_FACTOR;
#else
  cc_per_instruction = 70;
#endif

  state.r[22] = state.r[22 + 32] = state.iProcNum;

  dram_ptr = cSystem->PtrToMem(0);
  dram_size = U64(1) << cSystem->get_memory_bits();

  flush_data_page_cache();

  seq_line_ptr = nullptr;
  seq_offset = 0;
  seq_remaining = 0;
  seq_next_pc = 0;
}

void CAlphaCPU::start_threads() {
  char buffer[5];
  mySemaphore.tryWait(1);
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

  mySemaphore.tryWait(1);
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
static std::map<uint64_t, uint64_t> s_icflush_from; // EXC_ADDR at the flush
void CAlphaCPU::note_ic_flush_pc() {
  if (!s_trace_icflush)
    return;
  std::lock_guard<std::mutex> g(s_icflush_lock);
  const bool first = s_icflush_pc[state.current_pc]++ == 0;
  const bool first_from = s_icflush_from[state.exc_addr]++ == 0;
  // First sighting of a caller: the words before its return address, so the
  // CALL_PAL (and what surrounds it) can be read. Firmware runs identity
  // mapped in low memory; anything else is skipped rather than translated.
  if (first_from && dram_ptr && state.exc_addr < dram_size &&
      state.exc_addr >= 48) {
    printf("%%CPU-I-ICFLUSH: first flush entered from exc_addr %016" PRIx64
           "\n",
           state.exc_addr);
    for (uint64_t a = state.exc_addr - 48; a < state.exc_addr + 16; a += 4) {
      uint32_t w;
      memcpy(&w, (const char *)dram_ptr + a, 4);
      printf("%%CPU-I-ICFLUSH:   %s %08llx: %08x  op=%02x ra=%02x rb=%02x "
             "fn=%04x\n",
             a == state.exc_addr - 4 ? "->" : "  ", (unsigned long long)a, w,
             w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xffff);
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
  // Who entered PALcode for it: EXC_ADDR at the flush is the return address
  // of the CALL_PAL (or the trapped PC) that led there.
  {
    std::vector<std::pair<uint64_t, uint64_t>> f(s_icflush_from.begin(),
                                                 s_icflush_from.end());
    std::sort(f.begin(), f.end(),
              [](const std::pair<uint64_t, uint64_t> &a,
                 const std::pair<uint64_t, uint64_t> &b) {
                return a.second > b.second;
              });
    printf("%%CPU-I-ICFLUSH: entered from %zu distinct EXC_ADDRs\n", f.size());
    for (size_t i = 0; i < f.size() && i < 12; ++i)
      printf("%%CPU-I-ICFLUSH:   exc_addr %016llx  %llu (%.1f%%)\n",
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

CAlphaCPU::~CAlphaCPU() {
  stop_threads();
  dump_ic_flush_pcs();
}

#if defined(IDB)
char dbg_string[1000];
#if !defined(LS_MASTER) && !defined(LS_SLAVE)
char *dbg_strptr;
#endif

/**
 * \brief Do whatever needs to be done to a debug-string.
 *
 * Used in IDB-mode to handle the disassembly- string. In es40_idb, it is
 * written to the standard output.
 *
 * \param s       Pointer to the debug string.
 **/
void handle_debug_string(char *s) {
#if defined(LS_SLAVE) || defined(LS_MASTER)

  //    lockstep_compare(s);
  *dbg_strptr++ = '\n';
  *dbg_strptr = '\0';
#else
  if (*s)
    printf("%s\n", s);
#endif
}
#endif
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

#if !defined(CONSTANT_TIME_FACTOR)
  if (state.instruction_count > 0) {
    // correct CPU timing loop...
    u64 icount = state.instruction_count;
    u64 cc = cc_large;
    u64 time = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - start_time)
                   .count();
    s64 ce = cc_per_instruction;

    u64 cc_aim = time * cpu_hz / 1000000; // microsecond resolution
    u64 ce_aim = cc_aim / icount;

    s64 icount_lapse = icount - prev_icount;
    s64 cc_diff = cc_aim - cc;
    s64 ce_diff = (s64)((float)cc_diff / (float)icount_lapse);

    s64 ce_new = ce_aim + ce_diff;
    if (ce_new < 1)
      ce_new = 1;
    if (ce_new > 200)
      ce_new = 200;

    if (ce_new != ce) {

      //    printf("                                     time %12" PRId64 " |
      //    prev %12" PRId64 "  \n",time,prev_time); printf("          count
      //    lapse %12" PRId64 " | curr %12" PRId64 " | prev %12" PRId64 "
      //    \n",icount_lapse,icount,prev_icount); printf("cc %12" PRId64 " | aim
      //    %12" PRId64 " | diff %12" PRId64 " | prev %12" PRId64 "
      //    \n",cc,cc_aim,cc_diff,prev_cc); printf("ce %12" PRId64 " | aim %12"
      //    PRId64 " | diff %12" PRId64 " | new  %12" PRId64 "
      //    \n",ce,ce_aim,ce_diff,ce_new);
      //    printf("==========================================================================
      //    \n");
      cc_per_instruction = ce_new;
      //    printf("cpu %d speed factor: %d\n",get_cpuid(),ce_new);
    }

    prev_cc = cc;
    prev_icount = icount;
    prev_time = time;
  }
#endif
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
  int _batch_budget = 512;
  u64 _cc_accum = 0;     // accumulated cycle counts (flushed every 32 insns)
  int _icount_accum = 0; // accumulated instruction count
  const u64 _cc_per_ins = cc_per_instruction; // cache in register
#else
  const u64 _cc_per_ins = cc_per_instruction;
#endif

#ifndef ES40_JIT
  {
    const auto now = std::chrono::steady_clock::now();

    // Wall-clock RPCC at batch granularity - same semantics as the JIT build.
    // The old per-instruction advance ran at cc_per_instruction rate, which
    // lags real time while the check_state feedback converges; SRM's
    // cycles-per-tick speed calibration measured that lag consistently and
    // locked in a low CPU speed.
    sync_cc_wallclock();

    // Poll the wall-clock Cchip interval timer once per execute() batch
    // (~512 instructions) rather than every 32;
    if (state.iProcNum == 0) {
      if (now >= next_timer_fire) {
        const u64 period_ns = theAli ? theAli->get_interval_period_ns() : 0;
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
  }

_next_instruction:
  if (--_batch_budget <= 0) {
    // Flush remaining accumulated counters before returning. state.cc is
    // wall-clock (advanced at batch top); _cc_accum only feeds cc_large for the
    // legacy check_state speed-factor feedback.
    state.instruction_count += _icount_accum;
    cc_large += _cc_accum;
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
#if defined(IDB)
  char *funcname = 0;
  dbg_string[0] = '\0';
#if !defined(LS_MASTER) && !defined(LS_SLAVE)
  dbg_strptr = dbg_string;
#endif
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
  if (DO_ACTION) {
#ifndef ES40_JIT
    // We're actually executing code. Cycle counter should be updated, interrupt
    // and interrupt timer status needs to be checked, and the next instruction
    // should be fetched from the instruction cache. Increase the cycle counter
    // if it is currently enabled.
    _icount_accum++;
    _cc_accum += _cc_per_ins;

    if ((_batch_budget & 31) == 0) {
      // Flush accumulated counters to state. state.cc is wall-clock (batch
      // top); _cc_accum only feeds cc_large for the check_state speed-factor
      // feedback.
      state.instruction_count += _icount_accum;
      _icount_accum = 0;
      cc_large += _cc_accum;
      _cc_accum = 0;

      // There are one or more active delayed irq_h interrupts. Go through the 6
      // irq_h timers, decrease them as needed, and set the interrupt if the
      // timer reaches 0. Batch to reduce memory ops.
      if (state.check_timers) {
        state.check_timers = false;
        for (int j = 0; j < 6; j++) {
          if (state.irq_h_timer[j]) {
            if (state.irq_h_timer[j] <= 32) {
              state.irq_h_timer[j] = 0;
              state.eir |= (U64(0x1) << j);
              // The timer hasn't reached 0 yet; check on the timers again next
              // clock tick.
              state.check_int = true;
            } else {
              // The timer has reached 0. Set the interrupt status, and set the
              // flag that we need to check the interrupt status
              state.irq_h_timer[j] -= 32;
              state.check_timers = true;
            }
          }
        }
      }
    }
#else
    // New unwound interpreter / future JIT path

    state.instruction_count++;
    cc_large += _cc_per_ins;
    // state.cc (RPCC) is pinned to wall-clock * cpu_hz at the jit_run boundary,
    // not advanced per-instruction here - interpreter can't run RPCC ahead of
    // wall time.

    // Process delayed irq_h timers one instruction at a time.
    if (state.check_timers) {
      state.check_timers = false;
      for (int ti = 0; ti < 6; ti++) {
        if (state.irq_h_timer[ti]) {
          if (state.irq_h_timer[ti] <= 1) {
            state.irq_h_timer[ti] = 0;
            state.eir |= (U64(0x1) << ti);
            state.check_int = true;
          } else {
            state.irq_h_timer[ti]--;
            state.check_timers = true;
          }
        }
      }
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
      // be checked.

      if (state.pal_vms) {
        // PALcode base is set to 0x8000; meaning OpenVMS PALcode is currently
        // active. In this case, our VMS PALcode replacement routines are valid,
        // and should be used as it is faster than using the original PALcode.

        // irq<4> = halt / MP work request (TIG halt lines): the replacement
        // routines don't handle it, so enter the real PALcode interrupt
        // vector, as without them.
        if (state.eir & state.eien & 0x10) {
          GO_PAL(INTERRUPT);
          seq_remaining = 0;
#ifndef ES40_JIT
          goto _next_instruction;
#else
          return;
#endif
        }

        // irq<1>=device, irq<2>=timer, irq<3>=IPI. (Was 0x6 = device+timer
        // only, which stranded incoming IPIs under VMS PALcode -> CPUSPINWAIT.)
        if (state.eir & state.eien & 0xe)
          if (vmspal_ent_ext_int(state.eir & state.eien & 0xe))
            return;

        if (state.sir & state.sien & 0xfffc)
          if (vmspal_ent_sw_int(state.sir & state.sien))
            return;

        if (state.asten &&
            (state.aster & state.astrr & ((1 << (state.cm + 1)) - 1)))
          if (vmspal_ent_ast_int(state.aster & state.astrr &
                                 ((1 << (state.cm + 1)) - 1)))
            return;

        if (state.sir & state.sien)
          if (vmspal_ent_sw_int(state.sir & state.sien))
            return;
      } else

      {

        // PALcode base is set to an unsupported value. We have no choice but to
        // transfer control to PALmode at the PALcode interrupt entry point.
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
          seq_remaining = 0;
#ifndef ES40_JIT
          goto _next_instruction;
#else
          return;
#endif
        }
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
#if defined(IDB)
      current_pc_physical = state.pc_phys;
#endif
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

#if defined(IDB)
      current_pc_physical = state.pc_phys;
#endif
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

#if defined(IDB)
    current_pc_physical = state.pc_phys;
#endif

#endif
  } // if (DO_ACTION)
  else {

    // We're not really executing any code (DO_ACTION is false); that means that
    // we're in a debugging session, and just listing instructions at a
    // particular address. In this case, we treat the program counter as a
    // physical address.
    ins = (u32)(cSystem->ReadMem(state.pc, 32, this));
#if defined(DEBUG_ARC)
    if (state.current_pc < 0x10000) {
      printf("PC=%016" PRIx64 " ins=%08x\n", state.current_pc, ins);
    }
#endif
  }

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

  // Decode and dispatch opcode. This is kept very compact using the OP-macro
  // defined in cpu_debug.h. For the normal emulator, this simply calls the
  // DO_<mnemonic> macro defined in one of the other cpu_*.h files; but for the
  // interactive debugger, it will also do disassembly, where the second
  // parameter to the macro (e.g. R12_R3) determines the formatting applied to
  // the operands. The macro ends with "return 0;".
#if defined(IDB)
  last_instruction = ins;
#endif
  opcode = ins >> 26;
  switch (opcode) {
  case 0x00: // CALL_PAL
    function = ins & 0x1fffffff;
    OP(CALL_PAL, PAL);

    //    switch (function)
    //    {
    //      case 0x123401: OP_FNC(vmspal_int_read_ide, NOP);
    //      default: OP(CALL_PAL,PAL);
    //    }
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

  case 0x15: // FLTV* instructions
    function = (ins >> 5) & 0x7ff;
    switch (function) {
    case 0x0a5:
    case 0x4a5:
      OP(CMPGEQ, F12_F3);

    case 0x0a6:
    case 0x4a6:
      OP(CMPGLT, F12_F3);

    case 0x0a7:
    case 0x4a7:
      OP(CMPGLE, F12_F3);

    case 0x03c:
    case 0x0bc:
      OP(CVTQF, F2_F3);

    case 0x03e:
    case 0x0be:
      OP(CVTQG, F2_F3);

    default:
      if (function & 0x200) {
        UNKNOWN2;
      }

      switch (function & 0x7f) {
      case 0x000:
        OP(ADDF, F12_F3);
      case 0x001:
        OP(SUBF, F12_F3);
      case 0x002:
        OP(MULF, F12_F3);
      case 0x003:
        OP(DIVF, F12_F3);
      case 0x01e:
        OP(CVTDG, F2_F3);
      case 0x020:
        OP(ADDG, F12_F3);
      case 0x021:
        OP(SUBG, F12_F3);
      case 0x022:
        OP(MULG, F12_F3);
      case 0x023:
        OP(DIVG, F12_F3);
      case 0x02c:
        OP(CVTGF, F12_F3);
      case 0x02d:
        OP(CVTGD, F2_F3);
      case 0x02f:
        OP(CVTGQ, F2_F3);
      default:
        UNKNOWN2;
      }
      break;
    }
    break;

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

  case 0x17: // FLTL* instructions
    function = (ins >> 5) & 0x7ff;
    switch (function) {
    case 0x010:
      OP(CVTLQ, F2_F3);

    case 0x020:
      OP(CPYS, F12_F3);

    case 0x021:
      OP(CPYSN, F12_F3);

    case 0x022:
      OP(CPYSE, F12_F3);

    case 0x024:
      OP(MT_FPCR, X_F1);

    case 0x025:
      OP(MF_FPCR, X_F1);

    case 0x02a:
      OP(FCMOVEQ, F12_F3);

    case 0x02b:
      OP(FCMOVNE, F12_F3);

    case 0x02c:
      OP(FCMOVLT, F12_F3);

    case 0x02d:
      OP(FCMOVGE, F12_F3);

    case 0x02e:
      OP(FCMOVLE, F12_F3);

    case 0x02f:
      OP(FCMOVGT, F12_F3);

    case 0x030:
    case 0x130:
    case 0x530:
      OP(CVTQL, F12_F3);

    default:
      UNKNOWN2;
    }
    break;

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
    case 0x4800:
      OP(IMB, NOP);
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

#if defined(IDB)

/**
 * \brief Produce disassembly-listing without marker
 *
 * \param from    Address of first instruction to be disassembled.
 * \param to      Address of instruction following the last instruction to
 *                be disassembled.
 **/
void CAlphaCPU::listing(u64 from, u64 to) { listing(from, to, 0); }

/**
 * \brief Produce disassembly-listing with marker
 *
 * \param from    Address of first instruction to be disassembled.
 * \param to      Address of instruction following the last instruction to
 *                be disassembled.
 * \param mark    Address of instruction to be underlined with a marker line.
 **/
void CAlphaCPU::listing(u64 from, u64 to, u64 mark) {
  printf("%%CPU-I-LISTNG: Listing from %016" PRIx64 " to %016" PRIx64 "\n",
         from, to);

  u64 iSavedPC;
  bool bSavedDebug;
  iSavedPC = state.pc;
  bSavedDebug = bDisassemble;
  bDisassemble = true;
  bListing = true;
  for (state.pc = from; state.pc <= to;) {
    execute();
    if (state.pc == mark)
      printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");
  }

  bListing = false;
  state.pc = iSavedPC;
  bDisassemble = bSavedDebug;
}

u64 CAlphaCPU::get_instruction_count() { return state.instruction_count; }
#endif
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

  // Then where this page was found last time (m_tb_hint): one indexed probe
  // instead of the scan below, validated by the same test the scan applies.
  u8 &hint = m_tb_hint[t][(virt >> 13) & (kTbHintEntries - 1)];
  i = hint;
  if (i < TB_ENTRIES && state.tb[t][i].valid &&
      !((state.tb[t][i].virt ^ virt) & state.tb[t][i].match_mask) &&
      TB_ASN_MATCH(state.tb[t][i])) {
#ifdef JIT_VERIFY
    // Oracle: the index is derived state that no differential test can see
    // (interpreter and JIT share this lookup, so a wrong answer is
    // common-mode). Validation guarantees the hinted entry MATCHES; what it
    // cannot guarantee is that it is the entry the scan would have chosen
    // first, which differs only if two entries match one address. Check.
    for (int j = 0; j < TB_ENTRIES; j++)
      if (state.tb[t][j].valid &&
          !((state.tb[t][j].virt ^ virt) & state.tb[t][j].match_mask) &&
          TB_ASN_MATCH(state.tb[t][j])) {
        if (j != i)
          printf("%%CPU-W-TBHINT: MISMATCH va %016" PRIx64 " hint entry %d, "
                 "scan entry %d (tb %d)\n",
                 virt, i, j, t);
        break;
      }
#endif
    state.last_found_tb[t][rw] = i;
    return i;
  }

  // Otherwise, loop through the TB entries to find a match.
  for (i = 0; i < TB_ENTRIES; i++) {
    if (state.tb[t][i].valid &&
        !((state.tb[t][i].virt ^ virt) & state.tb[t][i].match_mask) &&
        TB_ASN_MATCH(state.tb[t][i])) {
      state.last_found_tb[t][rw] = i;
      hint = (u8)i;
      return i;
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
// round-robin victim), the same eviction bookkeeping (drop the evicted page
// from the data page cache), then the entry copied back whole.
int CAlphaCPU::tb_refill_from_shadow(u64 virt, int asn) {
  const STBEntry &sh = m_tb_shadow[tb_shadow_index(virt)];
  if (!sh.valid || sh.virt != (virt & sh.match_mask) ||
      !(sh.asm_bit || sh.asn == asn))
    return -1;
  const int t = TB_INDEX_DATA;
  const int i = state.next_tb[t];
  state.next_tb[t] = (i + 1 == TB_ENTRIES) ? 0 : i + 1;
  if (state.tb[t][i].valid)
    flush_data_page_cache_range(state.tb[t][i].virt, state.tb[t][i].match_mask);
  state.tb[t][i] = sh;
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
  int res;

  state.exc_addr = state.current_pc;

  if (flags & ACCESS_EXEC) {
    state.exc_sum = 0;
    if (state.pal_vms) {
      res = vmspal_ent_iacv(flags);
      return res ? res : -1;
    }

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
  if (state.pal_vms) {
    res = vmspal_ent_dfault(flags);
    return res ? res : -1;
  }

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
 *                  - RECUR         Recursive try. We tried to find this address
 *                                  before, added a TB entry, and now it should
 *sail through.
 *                  - PROBE         Access is for a PROBER or PROBEW access;
 *Don't swap in the page if it is outswapped.
 *                  - PROBEW        Access is for a PROBEW access.
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
  int res;

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

#if defined IDB
  if (bListing) {
    *phys = virt;
    return 0;
  }
#endif
#if defined(DEBUG_TB)
  if (forreal)
#if defined(IDB)
    if (bTB_Debug)
#endif
      printf("TB %" PRIx64 ",%x: ", virt, flags);
#endif

  if (!alpha_valid_va_form(virt, va48)) {
#if defined(DEBUG_TB)
    if (forreal)
#if defined(IDB)
      if (bTB_Debug)
#endif
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
#if defined(IDB)
      if (bTB_Debug)
#endif
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
#if defined(IDB)
          if (bTB_Debug)
#endif
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
#if defined(IDB)
        if (bTB_Debug)
#endif
          printf("SPE\n");
#endif
      return 0;
    }
  }

  // try to find it in the translation buffer
  i = FindTBEntry(virt, flags);

  if (i < 0) // not found, either trap to PALcode, or try to load the TB entry
             // and try again.
  {
    if (!forreal) // debugger-lookup of the address
      return -1;  // report failure, and don't look any further
    if (!state.pal_vms ||
        vmspal_exc_depth >= 3) // unknown PALcode, or the
                               // native VMS fast-path is already nested in
                               // exception delivery: vector through the real
                               // PALcode instead of recursing on the host stack
    {

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
    } else // VMS PALcode
    {
      if (flags & RECUR) // we already tried this
      {
        printf("Translationbuffer RECUR lookup failed!\n");
        return -1;
      }

      state.exc_addr = state.current_pc;
      if (flags & VPTE) {

        // try to handle the double miss. If this needs to transfer control
        // to the OS, it will return non-zero value.
        if ((res = vmspal_ent_dtbm_double_3(flags)))
          return res;

        // Double miss succesfully handled. Try to get the physical address
        // again.
        return virt2phys(virt, phys, flags | RECUR, asm_bit, ins);
      } else if (flags & ACCESS_EXEC) {

        // try to handle the ITB miss. If this needs to transfer control
        // to the OS, it will return non-zero value.
        if ((res = vmspal_ent_itbm(flags)))
          return res;

        // ITB miss succesfully handled. Try to get the physical address again.
        return virt2phys(virt, phys, flags | RECUR, asm_bit, ins);
      } else {
        state.fault_va = virt;
        state.exc_sum = ((u64)REG_1 & 0x1f)
                        << 8; /* HRM 5.2.13: EXC_SUM REG[12:8] is 5 bits */

        u32 opcode = I_GETOP(ins);
        state.mm_stat =
            ((opcode == 0x1b || opcode == 0x1f) ? opcode - 0x18 : opcode) << 4 |
            (flags & ACCESS_WRITE);

        // try to handle the single miss. If this needs to transfer control
        // to the OS, it will return non-zero value.
        if ((res = vmspal_ent_dtbm_single(flags)))
          return res;

        // Single miss succesfully handled. Try to get the physical address
        // again.
        return virt2phys(virt, phys, flags | RECUR, asm_bit, ins);
      }
    }
  }

  // If we get here, the number of the matching TB entry is in i.
#if defined(DEBUG_TB)
  else {
    if (forreal)
#if defined(IDB)
      if (bTB_Debug)
#endif
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
#if defined(IDB)
        if (bTB_Debug)
#endif
          printf("acv\n");
#endif
      if (!forreal) // FAKE probe: report failure, don't vector the fault —
                    // caller re-runs it under the interpreter
        return -1;
      if (flags & ACCESS_EXEC) {

        // handle I-stream access violation
        state.exc_addr = state.current_pc;
        state.exc_sum = 0;
        if (state.pal_vms) {
          if ((res = vmspal_ent_iacv(flags)))
            return res;
        } else {
          set_pc(state.pal_base + IACV + 1);
          return -1;
        }
      } else {

        // Handle D-stream access violation
        state.exc_addr = state.current_pc;
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
        if (state.pal_vms) {
          if ((res = vmspal_ent_dfault(flags)))
            return res;
        } else {
          set_pc(state.pal_base + DFAULT + 1);
          return -1;
        }
      }
    }

    // check if requested access doesn't fault.
    // WRCHK additionally requires that the FOW bit be clear -- HRM 6.4.1
    // WrChk variants check both FOR and FOW.
    if (state.tb[t][i].fault[flags & ACCESS_MODE] ||
        ((flags & WRCHK) && state.tb[t][i].fault[1])) {
#if defined(DEBUG_TB)
      if (forreal)
#if defined(IDB)
        if (bTB_Debug)
#endif
          printf("fault\n");
#endif
      if (!forreal) // FAKE probe: report failure, don't vector the fault —
                    // caller re-runs it under the interpreter
        return -1;
      if (flags & ACCESS_EXEC) {

        // handle I-stream access fault
        state.exc_addr = state.current_pc;
        state.exc_sum = 0;
        if (state.pal_vms) {
          if ((res = vmspal_ent_iacv(flags)))
            return res;
        } else {
          set_pc(state.pal_base + IACV + 1);
          return -1;
        }
      } else {

        // handle D-stream access fault
        state.exc_addr = state.current_pc;
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
        if (state.pal_vms) {
          if ((res = vmspal_ent_dfault(flags)))
            return res;
        } else {
          set_pc(state.pal_base + DFAULT + 1);
          return -1;
        }
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
#if defined(IDB)
    if (bTB_Debug)
#endif
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
  // Keep an 8 KB data translation in the shadow (see m_tb_shadow): what the
  // TB evicts later can then be refilled without a trap to PALcode.
  if (t == TB_INDEX_DATA && match_mask == GH_0_MATCH)
    m_tb_shadow[tb_shadow_index(virt)] = state.tb[t][i];
  state.last_found_tb[t][rw] = i;

#ifdef ES40_JIT
  if (itb_remap && m_jit)
    m_jit->note_itb_invalidate(
        CJitEngine::EPOCH_REMAP); // code page remapped in place -> chains
                                  // re-validate
#endif

  if (t == TB_INDEX_DATA) {
    // Only the replaced entry's and the new entry's pages can change
    // translation (a wholesale flush here emptied the cache on every miss).
    if (old_valid)
      flush_data_page_cache_range(old_virt, old_mask);
    flush_data_page_cache_range(virt, match_mask);
  }

#if defined(DEBUG_TB_)
#if defined(IDB)
  if (bTB_Debug)
#endif
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
    if (!state.tb[t][i].asm_bit)
      state.tb[t][i].valid = false;

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
  if (i >= 0)
    state.tb[t][i].valid = false;
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

#if defined(IDB)
const char *PAL_NAME[] = {
    "HALT",       "CFLUSH",     "DRAINA",     "LDQP",
    "STQP",       "SWPCTX",     "MFPR_ASN",   "MTPR_ASTEN",
    "MTPR_ASTSR", "CSERVE",     "SWPPAL",     "MFPR_FEN",
    "MTPR_FEN",   "MTPR_IPIR",  "MFPR_IPL",   "MTPR_IPL",
    "MFPR_MCES",  "MTPR_MCES",  "MFPR_PCBB",  "MFPR_PRBR",
    "MTPR_PRBR",  "MFPR_PTBR",  "MFPR_SCBB",  "MTPR_SCBB",
    "MTPR_SIRR",  "MFPR_SISR",  "MFPR_TBCHK", "MTPR_TBIA",
    "MTPR_TBIAP", "MTPR_TBIS",  "MFPR_ESP",   "MTPR_ESP",
    "MFPR_SSP",   "MTPR_SSP",   "MFPR_USP",   "MTPR_USP",
    "MTPR_TBISD", "MTPR_TBISI", "MFPR_ASTEN", "MFPR_ASTSR",
    "28",         "MFPR_VPTB",  "MTPR_VPTB",  "MTPR_PERFMON",
    "2C",         "2D",         "MTPR_DATFX", "2F",
    "30",         "31",         "32",         "33",
    "34",         "35",         "36",         "37",
    "38",         "39",         "3A",         "3B",
    "3C",         "3D",         "WTINT",      "MFPR_WHAMI",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "-",          "-",          "-",          "-",
    "BPT",        "BUGCHK",     "CHME",       "CHMK",
    "CHMS",       "CHMU",       "IMB",        "INSQHIL",
    "INSQTIL",    "INSQHIQ",    "INSQTIQ",    "INSQUEL",
    "INSQUEQ",    "INSQUEL/D",  "INSQUEQ/D",  "PROBER",
    "PROBEW",     "RD_PS",      "REI",        "REMQHIL",
    "REMQTIL",    "REMQHIQ",    "REMQTIQ",    "REMQUEL",
    "REMQUEQ",    "REMQUEL/D",  "REMQUEQ/D",  "SWASTEN",
    "WR_PS_SW",   "RSCC",       "READ_UNQ",   "WRITE_UNQ",
    "AMOVRR",     "AMOVRM",     "INSQHILR",   "INSQTILR",
    "INSQHIQR",   "INSQTIQR",   "REMQHILR",   "REMQTILR",
    "REMQHIQR",   "REMQTIQR",   "GENTRAP",    "AB",
    "AC",         "AD",         "CLRFEN",     "AF",
    "B0",         "B1",         "B2",         "B3",
    "B4",         "B5",         "B6",         "B7",
    "B8",         "B9",         "BA",         "BB",
    "BC",         "BD",         "BE",         "BF"};

const char *IPR_NAME[] = {
    "ITB_TAG",     "ITB_PTE",     "ITB_IAP",     "ITB_IA",       "ITB_IS",
    "PMPC",        "EXC_ADDR",    "IVA_FORM",    "IER_CM",       "CM",
    "IER",         "IER_CM",      "SIRR",        "ISUM",         "HW_INT_CLR",
    "EXC_SUM",     "PAL_BASE",    "I_CTL",       "IC_FLUSH_ASM", "IC_FLUSH",
    "PCTR_CTL",    "CLR_MAP",     "I_STAT",      "SLEEP",        "?0001.1000?",
    "?0001.1001?", "?0001.1010?", "?0001.1011?", "?0001.1100?",  "?0001.1101?",
    "?0001.1110?", "?0001.1111?", "DTB_TAG0",    "DTB_PTE0",     "?0010.0010?",
    "?0010.0011?", "DTB_IS0",     "DTB_ASN0",    "DTB_ALTMODE",  "MM_STAT",
    "M_CTL",       "DC_CTL",      "DC_STAT",     "C_DATA",       "C_SHFT",
    "M_FIX",       "?0010.1110?", "?0010.1111?", "?0011.0000?",  "?0011.0001?",
    "?0011.0010?", "?0011.0011?", "?0011.0100?", "?0010.0101?",  "?0010.0110?",
    "?0010.0111?", "?0011.1000?", "?0011.1001?", "?0011.1010?",  "?0011.1011?",
    "?0011.1100?", "?0010.1101?", "?0010.1110?", "?0010.1111?",  "PCTX.00000",
    "PCTX.00001",  "PCTX.00010",  "PCTX.00011",  "PCTX.00100",   "PCTX.00101",
    "PCTX.00110",  "PCTX.00111",  "PCTX.01000",  "PCTX.01001",   "PCTX.01010",
    "PCTX.01011",  "PCTX.01100",  "PCTX.01101",  "PCTX.01110",   "PCTX.01111",
    "PCTX.10000",  "PCTX.10001",  "PCTX.10010",  "PCTX.10011",   "PCTX.10100",
    "PCTX.10101",  "PCTX.10110",  "PCTX.10111",  "PCTX.11000",   "PCTX.11001",
    "PCTX.11010",  "PCTX.11011",  "PCTX.11100",  "PCTX.11101",   "PCTX.11110",
    "PCTX.11111",  "PCTX.00000",  "PCTX.00001",  "PCTX.00010",   "PCTX.00011",
    "PCTX.00100",  "PCTX.00101",  "PCTX.00110",  "PCTX.00111",   "PCTX.01000",
    "PCTX.01001",  "PCTX.01010",  "PCTX.01011",  "PCTX.01100",   "PCTX.01101",
    "PCTX.01110",  "PCTX.01111",  "PCTX.10000",  "PCTX.10001",   "PCTX.10010",
    "PCTX.10011",  "PCTX.10100",  "PCTX.10101",  "PCTX.10110",   "PCTX.10111",
    "PCTX.11000",  "PCTX.11001",  "PCTX.11010",  "PCTX.11011",   "PCTX.11100",
    "PCTX.11101",  "PCTX.11110",  "PCTX.11111",  "?1000.0000?",  "?1000.0001?",
    "?1000.0010?", "?1000.0011?", "?1000.0100?", "?1000.0101?",  "?1000.0110?",
    "?1000.0111?", "?1000.1000?", "?1000.1001?", "?1000.1010?",  "?1000.1011?",
    "?1000.1100?", "?1000.1101?", "?1000.1110?", "?1000.1111?",  "?1001.0000?",
    "?1001.0001?", "?1001.0010?", "?1001.0011?", "?1001.0100?",  "?1001.0101?",
    "?1001.0110?", "?1001.0111?", "?1001.1000?", "?1001.1001?",  "?1001.1010?",
    "?1001.1011?", "?1001.1100?", "?1001.1101?", "?1001.1110?",  "?1001.1111?",
    "DTB_TAG1",    "DTB_PTE1",    "DTB_IAP",     "DTB_IA",       "DTB_IS1",
    "DTB_ASN1",    "?1010.0110?", "?1010.0111?", "?1010.1000?",  "?1010.1001?",
    "?1010.1010?", "?1010.1011?", "?1010.1100?", "?1010.1101?",  "?1010.1110?",
    "?1010.1111?", "?1011.0000?", "?1011.0001?", "?1011.0010?",  "?1011.0011?",
    "?1011.0100?", "?1011.0101?", "?1011.0110?", "?1011.0111?",  "?1011.1000?",
    "?1011.1001?", "?1011.1010?", "?1011.1011?", "?1011.1100?",  "?1011.1101?",
    "?1011.1110?", "?1011.1111?", "CC",          "CC_CTL",       "VA",
    "VA_FORM",     "VA_CTL",      "?1100.0101?", "?1100.0110?",  "?1100.0111?",
    "?1100.1000?", "?1100.1001?", "?1100.1010?", "?1100.1011?",  "?1100.1100?",
    "?1100.1101?", "?1100.1110?", "?1100.1111?", "?1101.0000?",  "?1101.0001?",
    "?1101.0010?", "?1101.0011?", "?1101.0100?", "?1101.0101?",  "?1101.0110?",
    "?1101.0111?", "?1101.1000?", "?1101.1001?", "?1101.1010?",  "?1101.1011?",
    "?1101.1100?", "?1101.1101?", "?1101.1110?", "?1101.1111?",  "?1110.0000?",
    "?1110.0001?", "?1110.0010?", "?1110.0011?", "?1110.0100?",  "?1110.0101?",
    "?1110.0110?", "?1110.0111?", "?1110.1000?", "?1110.1001?",  "?1110.1010?",
    "?1110.1011?", "?1110.1100?", "?1110.1101?", "?1110.1110?",  "?1110.1111?",
    "?1111.0000?", "?1111.0001?", "?1111.0010?", "?1111.0011?",  "?1111.0100?",
    "?1111.0101?", "?1111.0110?", "?1111.0111?", "?1111.1000?",  "?1111.1001?",
    "?1111.1010?", "?1111.1011?", "?1111.1100?", "?1111.1101?",  "?1111.1110?",
    "?1111.1111?",
};
#endif
