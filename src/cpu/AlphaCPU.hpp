/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/alphabox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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
 * Although this is not required, the author would appreciate being notified
 * of, and receiving any modifications you may make to the source code that
 * might serve the general public.
 */

/**
 * \file
 * Contains the definitions for the emulated DecChip 21264CB EV68 Alpha
 *processor.
 **/
#if !defined(INCLUDED_ALPHACPU_H)
#define INCLUDED_ALPHACPU_H

#include <type_traits> // std::remove_reference_t for the TB entry alias
#include <atomic>
#include <condition_variable>
#include <mutex>

#include "CpuModel.hpp"
#include "System.hpp"
#include "SystemComponent.hpp"
#include "cpu_defs.hpp"
#ifdef ALPHABOX_HVF
#include "HvRuntime.hpp"
#endif
class CJitEngine; // JIT block-cache engine (ES40_JIT builds)

/// The processor executing on this thread, or nullptr on a device thread.
/// Diagnostics only (the unknown-access trace): never a control path.
extern thread_local class CAlphaCPU *t_running_cpu;

/// Is this a well-formed virtual address for the current VA size? Defined
/// with address translation in AlphaCPU.cpp; the JIT's memory helpers in
/// AlphaCPU_jit.cpp have to ask the same question before translating.
bool alpha_valid_va_form(u64 virt, bool va48);

// Bumped by every CPU's instruction-cache flush (IC_FLUSH / IMB). Each CPU's
// JIT compares it at the start of a dispatch batch and flushes its own block
// cache when another CPU flushed: compiled blocks are per-CPU, but guest code
// pages are shared, so new code loaded by one CPU must invalidate the others.
inline std::atomic<u64> g_jit_code_flush{0};

/// Number of entries in the Instruction Cache
#define ICACHE_ENTRIES 1024
// Size of Instruction Cache entries in DWORDS (instructions)
#define ICACHE_LINE_SIZE 512
/** These bits should match to have an Instruction Cache hit.
    This includes bit 0, because it indicates PALmode . */
#define ICACHE_MATCH_MASK (u64)(U64(0x1) - (ICACHE_LINE_SIZE * 4))
                  /// DWORD (instruction) number of an address in an ICache
                  /// entry.
#define ICACHE_INDEX_MASK (u64)(ICACHE_LINE_SIZE - U64(0x1))
/// Byte numer of an address in an ICache entry.
#define ICACHE_BYTE_MASK (u64)(ICACHE_INDEX_MASK << 2)
/// Number of entries in each Translation Buffer
#define TB_ENTRIES 128 // as the real EV68 (16 thrashed data-TB misses under NT)

/**
 * \brief Emulated CPU.
 *
 * The CPU emulated is the DECchip 21264CB Alpha Processor (EV68).
 *
 * Documentation consulted:
 *  - Alpha 21264/EV68CB and 21264/EV68DC Microprocessor Hardware Reference
 *Manual [HRM] (http://download.majix.org/dec/21264ev68cb_ev68dc_hrm.pdf)
 *  - DS-0026A-TE: Alpha 21264B Microprocessor Hardware Reference Manual [HRM]
 *(http://ftp.digital.com/pub/Digital/info/semiconductor/literature/21264hrm.pdf)
 *  - Alpha Architecture Reference Manual, fourth edition [ARM]
 *(http://download.majix.org/dec/alpha_arch_ref.pdf)
 *	.
 **/
class CAlphaCPU : public CSystemComponent {
public:
  /// The part this processor is (identity and extensions).
  const cpu_model &model() const { return *m_model; }

  void flush_icache_asm();
  virtual int SaveState(FILE *f);
  virtual int RestoreState(FILE *f);
  void irq_h(int number, bool assert, int delay);
  inline bool int_deliverable() const;
  void irq_trace_entry(); // ALPHABOX_IRQTRACE: interrupt-storm diagnosis
  void irq_trace_ipr(const char *what, u32 fn, u64 val);
  int get_cpuid();
  void flush_icache();
  void note_ic_flush_pc();  // ALPHABOX_TRACE_ICFLUSH: histogram of the PC that
                            // issued each icache flush
  void dump_ic_flush_pcs(); // ...printed when the CPU goes away

  virtual void run(); // Poco Thread entry point
  void run_loop();    // the dispatch loop run() ends in
#ifdef ALPHABOX_HVF
  static uint64_t hv_run_loop(void *self); // run_loop() as a VM entry
#endif
  void execute();
  void release_threads();

  void set_PAL_BASE(u64 pb);
  virtual void check_state();
  CAlphaCPU(CConfigurator *cfg, CSystem *system);
  virtual ~CAlphaCPU();
  u64 get_r(int i, bool translate);

  /// Whether ALPHABOX_TRACE_CALLS asked for the subroutine-call trace.
  /// Read once at startup: this sits in the interpreter's hot path.
  static bool trace_calls_on() { return s_trace_calls; }
  /// Report a call the first time this site reaches this routine.
  void trace_call(u64 from, u64 to);
  static bool s_trace_calls;
  u64 get_f(int i);
  void set_r(int reg, u64 val);
  void set_f(int reg, u64 val);
  u64 get_prbr(void);
  u64 get_hwpcb(void);
  u64 get_pc();
  u64 get_pal_base();

  void enable_icache();
#ifdef ES40_JIT
  // One jit_run dispatch batch for main-thread callers (LoadROM's SRM
  // decompression runs before the CPU threads exist).
  void jit_step(int budget) { jit_run(budget); }
#endif
  void restore_icache();

  bool get_waiting() { return state.wait_for_start; };
  void stop_waiting() { state.wait_for_start = false; };
#ifdef IDB
  u64 get_current_pc_physical();
  u64 get_instruction_count();
  u32 get_last_instruction();
  u64 get_last_read_loc() { return last_read_loc; }
  u64 get_last_write_loc() { return last_write_loc; }
#endif
  u64 get_clean_pc();
  void next_pc();
  void set_pc(u64 p_pc);
  void add_pc(u64 a_pc);

  u64 get_speed() { return cpu_hz; };

  u64 va_form(u64 address, bool bIBOX);

#if defined(IDB)
  void listing(u64 from, u64 to);
  void listing(u64 from, u64 to, u64 mark);
#endif
  int virt2phys(u64 virt, u64 *phys, int flags, bool *asm_bit, u32 instruction);

  virtual void init();
  virtual void start_threads();
  virtual void stop_threads();
  void ResetForSystemReset();

private:
  std::unique_ptr<std::thread> myThread;
  std::atomic_bool myThreadDead{false};
  CSemaphore mySemaphore;
  bool StopThread;

  int get_icache(u64 address, u32 *data);
  u8 icache_exec_modes(u64 address, u64 v_a);
  int FindTBEntry(u64 virt, int flags);
  int initiate_acv_fault(u64 virt, int flags, u32 instruction);
  void add_tb(u64 virt, u64 pte_phys, u64 pte_flags, int flags, int asn);
  void add_tb_i(u64 virt, u64 pte);
  void add_tb_d(u64 virt, u64 pte, int dtb);
  void tbia(int flags);
  void tbiap(int flags);
  void tbis(u64 virt, int flags);
  void tbis_d(u64 virt, int asn);

  /* Floating Point routines */
  u64 ieee_lds(u32 op);
  u32 ieee_sts(u64 op);
  u64 ieee_cvtst(u64 op, u32 ins);
  u64 ieee_cvtts(u64 op, u32 ins);
  s32 ieee_fcmp(u64 s1, u64 s2, u32 ins, u32 trap_nan);
  u64 ieee_cvtif(u64 val, u32 ins, u32 dp);
  u64 ieee_cvtfi(u64 op, u32 ins);
  u64 ieee_fadd(u64 s1, u64 s2, u32 ins, u32 dp, bool sub);
  u64 ieee_fmul(u64 s1, u64 s2, u32 ins, u32 dp);
  u64 ieee_fdiv(u64 s1, u64 s2, u32 ins, u32 dp);
  u64 ieee_sqrt(u64 op, u32 ins, u32 dp);
  int ieee_unpack(u64 op, UFP *r, u32 ins);
  void ieee_norm(UFP *r);
  u64 ieee_rpack(UFP *r, u32 ins, u32 dp);
  void ieee_trap(u64 trap, u32 instenb, u64 fpcrdsb, u32 ins);
  u64 vax_ldf(u32 op);
  u64 vax_ldg(u64 op);
  u32 vax_stf(u64 op);
  u64 vax_stg(u64 op);
  void vax_trap(u64 mask, u32 ins);
  void vax_unpack(u64 op, UFP *r, u32 ins);
  void vax_unpack_d(u64 op, UFP *r, u32 ins);
  void vax_norm(UFP *r);
  u64 vax_rpack(UFP *r, u32 ins, u32 dp);
  u64 vax_rpack_d(UFP *r, u32 ins);
  int vax_fcmp(u64 s1, u64 s2, u32 ins);
  u64 vax_cvtif(u64 val, u32 ins, u32 dp);
  u64 vax_cvtfi(u64 op, u32 ins);
  u64 vax_fadd(u64 s1, u64 s2, u32 ins, u32 dp, bool sub);
  u64 vax_fmul(u64 s1, u64 s2, u32 ins, u32 dp);
  u64 vax_fdiv(u64 s1, u64 s2, u32 ins, u32 dp);
  u64 vax_sqrt(u64 op, u32 ins, u32 dp);

  /* VMS PALcode call: */
  void vmspal_call_cflush();
  void vmspal_call_draina();
  void vmspal_call_ldqp();
  void vmspal_call_stqp();
  void vmspal_call_swpctx();
  void vmspal_call_mfpr_asn();
  void vmspal_call_mtpr_asten();
  void vmspal_call_mtpr_astsr();
  void vmspal_call_cserve();
  void vmspal_call_mfpr_fen();
  void vmspal_call_mtpr_fen();
  void vmspal_call_mfpr_ipl();
  void vmspal_call_mtpr_ipl();
  void vmspal_call_mfpr_mces();
  void vmspal_call_mtpr_mces();
  void vmspal_call_mfpr_pcbb();
  void vmspal_call_mfpr_prbr();
  void vmspal_call_mtpr_prbr();
  void vmspal_call_mfpr_ptbr();
  void vmspal_call_mfpr_scbb();
  void vmspal_call_mtpr_scbb();
  void vmspal_call_mtpr_sirr();
  void vmspal_call_mfpr_sisr();
  void vmspal_call_mfpr_tbchk();
  void vmspal_call_mtpr_tbia();
  void vmspal_call_mtpr_tbiap();
  void vmspal_call_mtpr_tbis();
  void vmspal_call_mfpr_esp();
  void vmspal_call_mtpr_esp();
  void vmspal_call_mfpr_ssp();
  void vmspal_call_mtpr_ssp();
  void vmspal_call_mfpr_usp();
  void vmspal_call_mtpr_usp();
  void vmspal_call_mtpr_tbisd();
  void vmspal_call_mtpr_tbisi();
  void vmspal_call_mfpr_asten();
  void vmspal_call_mfpr_astsr();
  void vmspal_call_mfpr_vptb();
  void vmspal_call_mtpr_datfx();
  void vmspal_call_mfpr_whami();
  void vmspal_call_imb();
  void vmspal_call_prober();
  void vmspal_call_probew();
  void vmspal_call_rd_ps();
  int vmspal_call_rei();
  void vmspal_call_swasten();
  void vmspal_call_wr_ps_sw();
  void vmspal_call_rscc();
  void vmspal_call_read_unq();
  void vmspal_call_write_unq();

  /* VMS PALcode entry: */
  int vmspal_ent_dtbm_double_3(int flags);
  int vmspal_ent_dtbm_single(int flags);
  int vmspal_ent_itbm(int flags);
  int vmspal_ent_iacv(int flags);
  int vmspal_ent_dfault(int flags);
  int vmspal_ent_ext_int(int ei);
  int vmspal_ent_sw_int(int si);
  int vmspal_ent_ast_int(int ast);

  /* VMS PALcode internal: */
  int vmspal_int_initiate_exception();
  int vmspal_int_initiate_interrupt();

  // New FP helpers.....
  void write_fpcr_arch(u64 arch_val);
  u64 read_fpcr_arch() const;

  bool icache_enabled;
  bool skip_memtest_hack = false;
  // Host-recursion depth of native VMS-PAL exception initiation. Nested
  // initiations (exception while delivering an exception) fall back to the
  // guest's real PALcode instead of recursing on the host stack.
  int vmspal_exc_depth = 0;
  bool vmspal_lle_enabled;

  // ... ... ...
  u64 cc_large;
  u64 start_icount;
  u64 start_cc;
  std::chrono::steady_clock::time_point start_time;
  u64 prev_icount;
  u64 prev_cc;
  u64 prev_time;
  u64 cc_per_instruction;
  u64 cpu_hz;

  // Wall-clock-paced Cchip interval timer (b_irq<2>).  CPU 0 fires once
  // per period as it passes batch-flush boundaries.  Avoids cross-thread
  // edge coalescing seen with AliM1543C-thread firing.  next_timer_fire is
  // the count-preserving schedule (+= period per fire); tick_last_fire paces
  // catch-up so backlog repays at no more than 2x the nominal rate.
  std::chrono::steady_clock::time_point next_timer_fire;
  std::chrono::steady_clock::time_point tick_last_fire;
  u32 tick_pace_lcg = 0x9e3779b9; // noise term of the catch-up gap modulation
  u32 tick_fire_idx = 0;          // triangle-wave phase of that modulation
  u64 tick_gap_ns = 0; // min spacing after tick_last_fire for the next fire
                       // (0: the first fire is immediate)
  u64 tick_last_icount = 0;     // instruction_count at the last observed tick
  u32 tick_seen_seq = 0;        // last CSystem tick sequence this CPU observed
  u64 m_max_instr_per_tick = 0; // timer.max_instr_per_tick (0 = pacing off)
  u64 tick_next_gap_ns(u64 period_ns);
  enum class TickHold { Ticked, Expired, Doorbell };
  TickHold tick_hold(u64 period_ns);

  // The host's monotonic clock, read as cheaply as the host allows.
  //
  // What this costs is not a detail: a guest RPCC read syncs the cycle
  // counter, and Windows 2000 reads RPCC about twenty million times a second,
  // so the read alone was a third of a core. Measured on an Apple M-series
  // host: std::chrono::steady_clock::now() 14.8 ns, mach_absolute_time()
  // 5.0 ns, the generic timer register 0.28 ns -- and they are all the same
  // counter. steady_clock merely reaches it through a call and a unit
  // conversion. Where the register is not ours to read, the standard clock
  // is what we have; ticks are then nanoseconds and nothing else changes.
#if defined(__aarch64__)
  static inline u64 host_ticks() {
    u64 v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
  }
  static inline u64 host_tick_hz() {
    u64 f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : 1000000000ULL;
  }
#else
  static inline u64 host_ticks() {
    return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  static inline u64 host_tick_hz() { return 1000000000ULL; }
#endif
  /// Host nanoseconds as host ticks, for the few places that bill an
  /// instrumentation stall back out of the cycle counter.
  static inline u64 ns_to_host_ticks(u64 ns) {
    const u64 hz = host_tick_hz();
    return hz == 1000000000ULL ? ns
                               : (u64)((__uint128_t)ns * hz / 1000000000ULL);
  }

  // Wall-clock RPCC: state.cc advances by real elapsed time * cpu_hz so it
  // tracks the configured CPU frequency regardless of how fast/bursty the JIT
  // runs. This is the last sync timestamp, in host ticks; the delta since it
  // (when cc_ena) is added to state.cc each jit_run, then it's reset to now.
  u64 cc_last_sync = 0;
  /// Guest cycles per host tick in 32.32 fixed point, so the conversion is a
  /// multiply and a shift rather than a divide by a frequency the compiler
  /// cannot see. Truncation makes the counter slow by under one part in 2^32.
  u64 cc_cycles_per_tick_q32 = 0;
  u64 cc_tick_hz = 1000000000ULL; // host_tick_hz(), read once
  u64 cc_wall_remainder = 0; // sub-cycle numerator carried across syncs (Q32)
  u64 cc_last_read = 0; // last RPCC value returned (forward-progress floor)
  u64 cc_borrow = 0;    // cycles lent to that floor, repaid from wall progress

  // Advance the wall-clock cc to now. Called at dispatch-batch boundaries and
  // on every guest RPCC read: a stale batch-start value makes NetBSD's PCC
  // timecounter see time go backwards (negative ping times).
  void sync_cc_wallclock() {
    const u64 now = host_ticks();
    if (cc_last_sync > now)
      cc_last_sync = now; // a stall can't exceed real elapsed; never bill
                          // negative
    u64 cc_delta = now - cc_last_sync;
    cc_last_sync = now;
    if (state.cc_ena) {
      // Cap (not drop) odd deltas at 1s: dropping made the cc run slow through
      // early-boot device-init stalls, and SRM's cycles-per-tick calibration
      // locked that in as a too-low CPU speed (the 357MHz bug).
      if (cc_delta > cc_tick_hz)
        cc_delta = cc_tick_hz;
      const u64 scaled =
          cc_delta * cc_cycles_per_tick_q32 + cc_wall_remainder; // sub-cycles
      u64 add = scaled >> 32;
      cc_wall_remainder = scaled & U64(0xffffffff);
      if (cc_borrow) { // repay the floor by withholding progress, never going
                       // backwards
        const u64 repay = cc_borrow < add ? cc_borrow : add;
        add -= repay;
        cc_borrow -= repay;
      }
      state.cc += add;
    }
  }

  // RPCC read with forward progress: the real counter advances every cycle,
  // so two reads never return the same value, even when the host clock hasn't
  // moved between them.
  u64 rpcc_read() {
    sync_cc_wallclock();
    if (state.cc_ena && state.cc <= cc_last_read) {
      cc_borrow += cc_last_read + 1 - state.cc;
      state.cc = cc_last_read + 1;
    }
    cc_last_read = state.cc;
    return ((u64)state.cc_offset) << 32 | (state.cc & U64(0xffffffff));
  }

  // The system bus, reached the way this build must reach it. Guest DRAM is
  // shared memory and is read directly wherever the caller already checks
  // for it; everything else is a device, and a device model may take locks,
  // allocate, or talk to a thread of its own -- none of which works from
  // inside the VM. So when the dispatch loop runs at EL1 these two leave
  // the VM and run on the host thread. Outside the VM they are the plain
  // call they always were. The argument block is a local: inside the VM
  // that is the vCPU stack, which both sides see.
  struct SysCall {
    CSystem *sys;
    CSystemComponent *src;
    u64 addr;
    u64 data;
    int size;
  };
  static u64 sys_read_out(void *p);
  static u64 sys_write_out(void *p);
  // The same instant as steady_clock::now(), but read from the generic
  // timer. std::chrono reaches the clock through Apple's counter register,
  // which the hypervisor traps: one VM exit per call, and the dispatch loop
  // calls it once per batch -- which was most of the cost of running
  // compiled code inside. cntvct_el0 is not trapped. Anchored once per
  // processor, in the shared object, so both sides agree on the epoch.
  inline void clock_anchor() {
    m_clk_ticks0 = host_ticks();
    m_clk_tp0 = std::chrono::steady_clock::now();
  }
  inline std::chrono::steady_clock::time_point now_fast() const {
#if defined(__aarch64__)
    // ALPHABOX_FASTCLOCK=0 goes back to std::chrono for this read, which is
    // the A/B switch that measures what the generic timer is worth. Read
    // once; the branch is predicted and costs nothing next to the call it
    // replaces.
    static const bool fast =
        !(getenv("ALPHABOX_FASTCLOCK") && atoi(getenv("ALPHABOX_FASTCLOCK")) == 0);
    if (fast && m_clk_ticks0) {
      const u64 hz = host_tick_hz();
      const u64 d = host_ticks() - m_clk_ticks0;
      // split so the nanosecond scaling cannot overflow on a long run
      return m_clk_tp0 + std::chrono::nanoseconds((d / hz) * 1000000000ull +
                                                  (d % hz) * 1000000000ull / hz);
    }
#endif
    return std::chrono::steady_clock::now();
  }
  void rate_tick();
#ifdef ES40_JIT
  // The block is opaque here: jitengine.hpp is not included by this header.
  struct CompileArg {
    CAlphaCPU *cpu;
    void *b;
  };
  static u64 compile_thunk(void *p);
  void compile_outside(void *b);
#endif

public:
#ifdef ALPHABOX_HVF
  // An interrupt raised by a device thread, the stop flag, the instruction
  // count the main thread reads: all of them cross the boundary, so under
  // ALPHABOX_HV=1 the processor object lives in shared memory.
  static void *operator new(size_t n);
  static void operator delete(void *p) noexcept;
#endif
  inline u64 sys_read(u64 addr, int size) {
#ifdef ALPHABOX_HVF
    if (hv::enabled()) {
      // A bulk data register is a transfer, not a register: served from the
      // shared buffer without leaving the VM. The word that completes the
      // buffer is left to the device, which clears the request and wakes
      // its controller.
      if (const CSystem::BulkPort *bp = cSystem->bulk_for(addr)) {
        const int words = (size == 32) ? 2 : (size == 16 ? 1 : 0);
        if (words && *bp->d.drq[*bp->d.selected & 1] &&
            *bp->d.ptr + words < *bp->d.size) {
          u64 v = bp->d.data[(*bp->d.ptr)++];
          if (words == 2)
            v |= (u64)bp->d.data[(*bp->d.ptr)++] << 16;
          ++m_bulk_served;
          return v;
        }
      }
      SysCall c{cSystem, this, addr, 0, size};
      return hv::escape(&sys_read_out, &c);
    }
#endif
    return cSystem->ReadMem(addr, size, this);
  }
  inline void sys_write(u64 addr, int size, u64 data) {
#ifdef ALPHABOX_HVF
    if (hv::enabled()) {
      if (const CSystem::BulkPort *bp = cSystem->bulk_for(addr)) {
        const int words = (size == 32) ? 2 : (size == 16 ? 1 : 0);
        if (words && *bp->d.drq[*bp->d.selected & 1] &&
            *bp->d.ptr + words < *bp->d.size) {
          bp->d.data[(*bp->d.ptr)++] = (u16)(data & 0xffff);
          if (words == 2)
            bp->d.data[(*bp->d.ptr)++] = (u16)((data >> 16) & 0xffff);
          ++m_bulk_served;
          return;
        }
      }
      SysCall c{cSystem, this, addr, data, size};
      hv::escape(&sys_write_out, &c);
      return;
    }
#endif
    cSystem->WriteMem(addr, size, data, this);
  }
  u64 m_bulk_served = 0; ///< transfers served without leaving the VM

  // DRAM fast-path cache
  char *dram_ptr; // cSystem->PtrToMem(0) - host pointer to base es40 ram array
                  // thingy
  u64 dram_size; // 1ULL << cSystem->get_memory_bits() — size of DRAM in bytes

  // Sequential icache fast path
  // Tracks position within current icache line for back-to-back sequential
  // fetches.
  u32 *seq_line_ptr; // pointer to current icache line data[]
  int seq_offset;    // next word offset within the line
  int seq_remaining; // words left in this line
  u64 seq_next_pc;   // expected PC for sequential hit

  inline void break_seq_icache() {
    seq_remaining = 0;
    // Also drop the icache-disabled fetch cursor: compiled JIT blocks write
    // state.pc natively, so pc_phys/rem_ins_in_page are stale after a
    // native pass and the next interpreted fetch must retranslate.
    state.rem_ins_in_page = 0;
  }

  // Data page translation cache: direct-mapped by virtual page (kDpcEntries
  // slots/dir) so a multi-page access pattern doesn't thrash a single slot. The
  // inline load checks one slot.
  // 64 slots/dir (8KB pages -> 512KB). Raising this to 256 was measured and
  // does NOT pay: read-helper calls fell only 373634 -> 343914 per 100M
  // instructions (8%) on CPU-bound Windows code, and the workload did not move
  // (61.7 s against 59.4 s, one run each). The misses are not conflicts, so
  // the slot count is not the lever -- see docs/performance.md. The ceiling,
  // should anyone try again, is the inline probe's addressing: it reaches both
  // rows with one displacement while dpc_tag + kDpcEntries*64 + 8 <= 32760, so
  // 256 is the largest power of two that stays free; past that the emitter has
  // to compute the slot address, on every memory op.
  static constexpr int kDpcBits = 6;
  static constexpr int kDpcEntries = 1 << kDpcBits;
  static constexpr u64 kDpcMask = (u64)kDpcEntries - 1;
  // A slice of the address, not a hash -- and that is a measured decision, not
  // an oversight. Slicing means two pages differing only ABOVE bits 13..18
  // always collide whatever the table size, which is exactly what a kernel and
  // a user page do: 100% of this probe's misses are "another page in my slot".
  // Folding the high bits in (h ^= h>>13; h ^= h>>26) does fix that -- read
  // helper calls per 100M instructions fell 364984 -> 242727 and writes
  // 45560 -> 115 -- and it still lost, because it costs two instructions on
  // every HIT and hits vastly outnumber misses:
  //
  //   cmd /c for /l loop   59.4 / 58.4 s  ->  57.3 / 57.3 s   (2-3% faster)
  //   nt_bench.sh axp all  24429 ms       ->  26007 ms        (6.5% SLOWER,
  //                                          three runs each, no overlap)
  //
  // The win only appears where misses are CONFLICT misses among a few hot
  // pages. Code that fits the cache pays the two instructions for nothing, and
  // code that streams past it misses anyway. Anyone retrying this needs an
  // index that costs at most one extra instruction. See docs/performance.md.
  static inline u64 dpc_index(u64 va) { return (va >> 13) & kDpcMask; }
  /// One translated page.
  ///
  /// The first two fields are the only ones compiled code reads, and it
  /// reads them together: they are adjacent and the slot is 64 bytes, so
  /// the index is a shift and the pair is one load. Everything a hit needs
  /// to know is in them -- which page this is, who it belongs to, whether
  /// it can be touched inline, and where it lives on the host.
  struct SDataPageCache {
    /// virt_page | (asn << 2) | cm, and bit 12 for a page compiled code
    /// must not touch inline (MMIO); all ones when the slot is empty. The
    /// low thirteen bits of a virtual page are zero, which is what leaves
    /// room for the address space and the mode.
    u64 tag;
    /// Where the page is on the host, less its virtual address, so that an
    /// access is bias + va: one register-offset load, no masking.
    u64 bias;

    u64 virt_page; // va & ~0x1FFF
    u64 phys_base; // pa & ~0x1FFF
    u64 host_base; // dram_ptr + phys_base for DRAM pages, 0 for MMIO
    int cm;        // current mode (CM) at fill time
    int asn;       // data ASN (asn0) at fill time
    bool valid;
    char pad[15]; // a 64-byte slot: the JIT's index is a shift

    /// What compiled code compares against: the page, the address space and
    /// the mode in one word. `mmio` sets a bit no key ever has, so such a
    /// page fails the comparison and takes the slow path without the fast
    /// path having to ask a second question.
    static inline u64 make_tag(u64 vp, int cm, int asn, bool mmio) {
      return vp | ((u64)(asn & 0xff) << 2) | (u64)(cm & 3) |
             (mmio ? U64(0x1000) : 0);
    }

    inline void fill(u64 vp, u64 phys, u64 host, int cm_, int asn_) {
      virt_page = vp;
      phys_base = phys;
      host_base = host;
      cm = cm_;
      asn = asn_;
      valid = true;
      bias = host - vp;
      tag = make_tag(vp, cm_, asn_, host == 0);
    }

    inline void invalidate() {
      valid = false;
      host_base = 0;
      tag = ~U64(0); // matches no key
    }
  } data_page_cache[2][kDpcEntries]; // [rw][dpc_index(va)]; [0]=read, [1]=write
  static_assert(sizeof(SDataPageCache) == 64,
                "the JIT indexes the page cache with a shift");

  /// An exact index of the TB: which slot holds each 8 KB page, two ways
  /// per set. A real EV68 looks its DTB up fully associatively in a cycle;
  /// FindTBEntry used to search it with a linear loop when its last-match
  /// guess failed, which on code that walks memory is every page-cache miss
  /// (the 47 ns helper of docs/performance.md, most of it that scan). The
  /// index is kept in step by every insert, eviction and invalidation, so a
  /// miss in it means "not in the TB" and the scan is skipped; an entry
  /// with a granularity hint spans pages and cannot be indexed, so while any
  /// is live (m_tb_gh_live) the scan is used. A hit is still validated
  /// against the entry exactly as the scan would, so the index can never
  /// return a wrong mapping; a false negative (three live pages in one set)
  /// costs a refill from the shadow at worst. In a JIT_VERIFY build the
  /// scan runs as the oracle after every index miss and counts the false
  /// negatives. ALPHABOX_TB_INDEX=0 keeps the scan in the same binary.
  /// Derived state: rebuilt from state.tb on reset and restore, not saved.
  static constexpr int kTbIdxBits = 11;
  static constexpr int kTbIdxEntries = 1 << kTbIdxBits;
  // Eight ways: page 0 alone is mapped under many ASNs at once under SRM,
  // and the verify oracle counted 317 false negatives in 6.4M probes with
  // two ways and 270 with four, all in that one set. A probe stops at the
  // first empty way, so the width costs nothing elsewhere.
  static constexpr int kTbIdxWays = 8;
  u8 m_tb_idx[2][kTbIdxEntries][kTbIdxWays] = {}; // slot + 1; 0 = empty
  int m_tb_gh_live[2] = {0, 0}; // live entries with a granularity hint
  bool m_tb_idx_on = true;
#ifdef JIT_VERIFY
  u64 m_tb_idx_false_neg = 0; // index said "absent", the scan found it
  u64 m_tb_idx_probes = 0;
#endif
  static inline int tb_idx_set(u64 virt) {
    return (int)((virt >> 13) & (u64)(kTbIdxEntries - 1));
  }
  /// Insert at way 0, the others sliding down; a slot already present just
  /// moves to the front; the oldest falls out (a false negative for it,
  /// served by the oracle in verify builds and by a refill otherwise).
  inline void tb_idx_insert(int t, u64 virt, int slot) {
    u8 *w = m_tb_idx[t][tb_idx_set(virt)];
    const u8 v = (u8)(slot + 1);
    int k = 0;
    while (k < kTbIdxWays - 1 && w[k] != v && w[k] != 0)
      k++;
    for (; k > 0; k--)
      w[k] = w[k - 1];
    w[0] = v;
  }
  /// Remove a slot, keeping the ways packed from the front (a lookup stops
  /// at the first empty way).
  inline void tb_idx_remove(int t, u64 virt, int slot) {
    u8 *w = m_tb_idx[t][tb_idx_set(virt)];
    const u8 v = (u8)(slot + 1);
    for (int k = 0; k < kTbIdxWays; k++)
      if (w[k] == v) {
        for (; k < kTbIdxWays - 1; k++)
          w[k] = w[k + 1];
        w[kTbIdxWays - 1] = 0;
        return;
      }
  }
  /// One entry leaves or enters the TB: the index, or the count of the
  /// unindexable (granularity-hint) ones. Defined with the GH_ masks.
  void tb_idx_drop(int t, int slot);
  void tb_idx_add(int t, int slot);
  void tb_idx_rebuild(); // from state.tb, both TBs

  /// (asn0 << 2) | cm: the half of a page-cache tag that is not the page.
  /// Kept beside the state it is made of so compiled code can load it in one
  /// instruction; dpc_context_changed() is what keeps it true.
  u64 m_dpc_key = 0;
  inline void dpc_context_changed() {
    m_dpc_key = ((u64)(state.asn0 & 0xff) << 2) | (u64)(state.cm & 3);
  }

  // Drop only the cached translation(s) a single data-TB entry could have
  // produced: its page's slot in both rows for an 8K entry (match_mask bit 13
  // set), or everything for a granularity-hint (large page) entry.
  inline void flush_data_page_cache_range(u64 virt, u64 match_mask) {
    if (!(match_mask & U64(0x2000))) {
      flush_data_page_cache();
      return;
    }
    const u64 idx = dpc_index(virt);
    for (int rw = 0; rw < 2; rw++)
      data_page_cache[rw][idx].invalidate();
  }
  /// The host bytes behind a physical page for the page cache: DRAM, or
  /// device memory the system offers for direct access (a framebuffer), or
  /// 0 for everything else (MMIO: every access goes through the device).
  inline u64 dpc_host_base(u64 phys) const {
    const u64 page = phys & ~U64(0x1FFF);
    if ((phys | U64(0x1FFF)) < dram_size)
      return (u64)dram_ptr + page;
    return (u64)cSystem->direct_host_page(page);
  }
  /// The same, for the write half of the cache. A page some block was
  /// compiled from answers 0, which tags the slot as one compiled code must
  /// not write inline: the store takes the helper instead, and the helper
  /// tells the code-page map about it. Reads are untouched.
  inline u64 dpc_host_base_w(u64 phys) const {
    if (m_nopflush && m_code_map && m_code_map->holds_code(phys))
      return 0;
    return dpc_host_base(phys);
  }
  /// A store landed in DRAM at phys: tell the map, so that the next IMB
  /// knows whether it has work.
  inline void note_dram_write(u64 phys) {
    if (m_code_map && !m_nopflush_break)
      m_code_map->note_write(phys);
  }
  /// A page became a code page after this processor had already cached it
  /// as inline-writable. Drop those cached translations; the refill will
  /// exclude it.
  void honour_new_code_pages();
  /// Another thread changed what the page cache may map (the direct range
  /// moved or went away): flush on this CPU's own thread, at the next timer
  /// check, which the JIT's gate also honours.
  std::atomic<bool> m_dpc_flush_req{false};
  friend class CSystem; // set_direct_memory() asks every CPU to flush
  void request_dpc_flush() {
    m_dpc_flush_req.store(true, std::memory_order_release);
    state.check_timers = true;
  }
  u64 m_stat_dpc_flushes = 0; // flush_data_page_cache() calls (JIT_STATS)
  inline void flush_data_page_cache() {
    ++m_stat_dpc_flushes;
    for (int i = 0; i < kDpcEntries; i++) {
      data_page_cache[0][i].invalidate();
      data_page_cache[1][i].invalidate();
    }
  }

  // ASN switch: bump the chain epoch so compiled chain edges revalidate through
  // the asn-keyed lookup paths (the chain guard checks tag+epoch only). No-op
  // in non-JIT builds.
  void jit_note_asn_change();

  /// The part this processor is: identity and architecture extensions,
  /// chosen by the configuration class (CpuModels.cpp).
  const cpu_model *m_model = nullptr;

#ifdef ES40_JIT
  // JIT block-discovery engine (per-CPU), allocated in init().
  CJitEngine *m_jit = nullptr;
  s64 m_jit_budget = 0; // instruction ceiling for a compiled chain
  void *m_link_from =
      nullptr; // JitBlock* whose successor link the dispatcher should patch
  u64 m_link_target = 0; // a static exit's target PC, recorded with link_from
  u64 m_jit_code_seen = 0;     // g_jit_code_flush at this CPU's last JIT flush
  // Idle pacing (JIT dispatcher, see jit_idle_pause): a CPU spinning in the NT
  // idle loop sleeps until irq_h raises an interrupt for it or 1 ms passes.
  std::mutex m_idle_mx;
  std::condition_variable m_idle_cv;
  std::atomic<bool> m_idle_sleeping{false};
  u64 m_idle_pc = 0;          // recognized idle-loop head (0 = not seen yet)
  u64 m_park_pc = 0;          // recognized HAL "wait to be started" loop head
  u64 m_idle_last_icount = 0; // instruction_count at the previous visit
  u32 m_idle_streak = 0;      // consecutive tight visits to the head
  u64 m_idle_sleeps = 0;      // pauses taken (diagnostics)
  // ALPHABOX_IDLESTATS=1 diagnostics: head visits, visits within the tight
  // window, zero-delta visits, sleeps blocked by check_int / check_timers,
  // host time slept, last visit delta.
  u64 m_idle_visits = 0, m_idle_near = 0, m_idle_zero = 0;
  u64 m_idle_blk_int = 0, m_idle_blk_tmr = 0, m_idle_slept_ns = 0;
  u64 m_idle_last_delta = 0;
  void jit_idle_pause();
  void idle_wake() {
    if (m_idle_sleeping.load()) {
      std::lock_guard<std::mutex> g(m_idle_mx);
      m_idle_cv.notify_all();
    }
  }
  void jit_run(int budget);    // drives the ES40_JIT lane via the interpreter
  void jit_flush_blocks();     // invalidate all discovered JIT blocks
  void jit_flush_blocks_asm(); // invalidate only !asm_global blocks (preserve
                               // global PAL across ASN flush)
  // Compiled-block memory helpers: load size_bits from va into *out / store
  // value to va. Return 0 on success, 1 on fault/unaligned (caller bails to the
  // interpreter).
  static int jit_read(CAlphaCPU *cpu, u64 va, int size_bits, u64 *out);
  int jit_spe_data(u64 va, int cm, u64 *phys) const; // data superpage probe
  static int jit_read_phys(CAlphaCPU *cpu, u64 phys, int size_bits,
                           u64 *out); // HW_LD physical: no translation
  static int jit_read_locked(CAlphaCPU *cpu, u64 va, int size_bits,
                             u64 *out); // LDx_L: load + establish LL/SC lock
  static int jit_read_vpte(CAlphaCPU *cpu, u64 va, int size_bits,
                           u64 *out); // HW_LD VPTE: kernel-checked virtual read
  static int
  jit_read_wchk(CAlphaCPU *cpu, u64 va, int size_bits,
                u64 *out); // HW_LD func 0xa: longword virtual + WrChk
  static int jit_write(CAlphaCPU *cpu, u64 va, int size_bits, u64 value);
  static int jit_write_phys(CAlphaCPU *cpu, u64 phys, int size_bits,
                            u64 value); // HW_ST physical: no translation
  static int jit_fp_read(CAlphaCPU *cpu, u64 va, u32 fa,
                         u32 descr); // LDS/LDT: f[fa] = convert(MEM[va])
  static int jit_fp_write(CAlphaCPU *cpu, u64 va, u32 fa,
                          u32 descr); // STS/STT: MEM[va] = convert(f[fa])
  static u64 jit_stc(CAlphaCPU *cpu, u64 va, int size_bits,
                     u64 value); // STx_C: store-conditional
  // CALL_PAL OPCDEC trap (privileged func in user mode): GO_PAL(OPCDEC) incl.
  // cpu_clear_lock.
  static void jit_opcdec(CAlphaCPU *cpu, u64 cpc);
  // HW_MFPR (PALmode): return the IPR named in ins; the caller (compiled
  // codegen) writes Ra.
  static u64 jit_hw_mfpr(CAlphaCPU *cpu, u32 ins, u64 cur);
  // HW_MTPR (PALmode): store value (Rb) to the side-effect-free IPR named by
  // function.
  static void jit_hw_mtpr(CAlphaCPU *cpu, u32 function, u64 value);
  // Indirect jump (JMP/HW_RET): look up the target block; return its chained
  // re-entry or null.
  static void *jit_indirect(CAlphaCPU *cpu, u64 target);
#ifdef JIT_VERIFY
  // ALPHABOX_JIT_FPTEST=1: compiled inline IEEE FP ops vs the interpreter
  void jit_fp_selftest();
#endif
  // ALPHABOX_JIT_RPCCTEST=1: the inline RPCC stub against the helper it
  // replaces. Not under JIT_VERIFY: the stub only exists on the builds
  // that verification cannot cover, which is exactly why it needs this.
  void jit_rpcc_selftest();
  // MISC (0x18) state reads: sel 0=RPCC (cycle counter), 1=RC, 2=RS (read
  // interrupt flag + clear/set). Value the verify can't re-derive -> replayed
  // from the load log like a load.
  static u64 jit_misc(CAlphaCPU *cpu, u32 sel);
  // Int<->FP register moves (ITOFx / FTOIx). fmt: 0=T raw, 1=S, 2=F. Return 1 =
  // FEN-trap bail.
  static int jit_itof(CAlphaCPU *cpu, u32 fc, u64 value, u32 fmt);
  static int jit_ftoi(CAlphaCPU *cpu, u32 fa, u32 fmt, u64 *out);
  // FLTL (0x17) non-arithmetic: FPCR moves, CPYSx, FCMOVx, CVTLQ/QL. Return 1 =
  // FEN-trap bail.
  static int jit_fltl(CAlphaCPU *cpu, u32 ins);
  // FLTV (0x15) VAX arith/convert/compare. Return 0 ok / 1 FEN-trap bail / 2
  // arith trap (exc_sum set).
  static int jit_fltv(CAlphaCPU *cpu, u32 ins);
  // Verify support: the interpreter pass records each value it loads, and the
  // compiled pass replays them instead of re-reading memory - false mismatch
  // fix
  bool m_jit_vreplay =
      false;            // compiled pass: replay recorded loads, don't re-read
  u32 m_jit_vlog_i = 0; // replay cursor
  u64 m_jit_vlog[64];   // values the interpreter pass loaded (<= prefix_len)
  u64 m_jit_vaddr[64];  // diagnostic: load addresses the interpreter computed
  // Store verify: the interpreter pass records each store (addr,value); the
  // compiled pass compares against it (stores touch memory, not GPRs, so the
  // GPR check can't see them).
  u32 m_jit_slog_i = 0;       // store-compare cursor
  u64 m_jit_slog_addr[64];    // store addresses the interpreter pass wrote
  u64 m_jit_slog_val[64];     // store values the interpreter pass wrote
  u64 m_jit_slog_success[64]; // STx_C outcome (1/0) the interp pass got; 1 for
                              // ordinary stores
#endif

  /// The state structure contains all elements that need to be saved to the
  /// statefile
  struct SCPU_state {
    u64 pc;         /**< Program counter */
    u64 current_pc; /**< Virtual address of current instruction */
    u64 pc_phys;
    u64 cc;                /**< IPR CC: Cycle counter [HRM p 5-3] */
    u64 instruction_count; /**< Number of times doclock has been called */
    bool cc_ena;           /**< IPR CC_CTL: Cycle counter enabled [HRM p 5-3] */
    std::atomic<bool> check_int;    /**< Interrupt maybe pending; raised
                                       cross-thread by irq_h() */
    std::atomic<bool> check_timers; /**< Delayed-irq countdown pending; set
                                       cross-thread by irq_h() */
    bool fpen; /**< IPR PCTX: fpe (floating point enable) [HRM p 5-21..23] */
    int cm;    /**< IPR IER_CM: cm (current mode) [HRM p 5-9..10] */
    int asn0;  /**< IPR DTB_ASN0: data ASN [HRM p 5-28]. Kept right after cm so
                  the JIT dpc  check loads {cm,asn0} as one 8-byte compare vs the
                  slot's {cm,asn}. */
    int asn;   /**< IPR PCTX: asn (address space number) [HRM p 5-21..22] */
    u64 exc_addr; /**< IPR EXC_ADDR: address of last exception [HRM p 5-8] */

    u64 r[64]; /**< Integer registers (0-31 normal, 32-63 shadow) */

    u64 f[64]; /**< Floating point registers (0-31 normal, 32-63 shadow) */

    bool wait_for_start;
    u64 pal_base; /**< IPR PAL_BASE [HRM: p 5-15] */
    u64 dc_stat;  /**< IPR DC_STAT: Dcache status [HRM p 5-31..32] */
    bool ppcen; /**< IPR PCTX: ppce (proc perf counting enable) [HRM p 5-21..23]
                 */
    u64 i_stat; /**< IPR I_STAT: Ibox status [HRM p 5-18..20] */
    u64 pctr_ctl;  /**< IPR PCTR_CTL [HRM p 5-23..25] */
    u32 cc_offset; /**< IPR CC: Cycle counter offset [HRM p 5-3] */
    u64 dc_ctl;    /**< IPR DC_CTL: Dcache control [HRM p 5-30..31] */
    int alt_cm;    /**< IPR DTB_ALTMODE: alternative cm for HW_LD/HW_ST [HRM p
                      5-26..27] */
    int smc;  /**< IPR M_CTL: smc (speculative miss control) [HRM p 5-29..30] */
    bool sde; /**< IPR I_CTL: sde[1] (PALshadow enable) [HRM p 5-15..18] */
    u64 fault_va;   /**< IPR VA: virtual address of last Dstream miss or fault
                       [HRM p 5-4] */
    u64 va_form_va; /**< Address used for VA_FORM computation (may differ from
                       VA for VPTE) */
    u64 exc_sum;    /**< IPR EXC_SUM: exception summary [HRM p 5-13..15] */
    int i_ctl_va_mode;  /**< IPR I_CTL: (va_form_32 + va_48) [HRM p 5-15..17] */
    int va_ctl_va_mode; /**< IPR VA_CTL: (va_form_32 + va_48) [HRM p 5-4] */
    u64 i_ctl_vptb;     /**< IPR I_CTL: vptb (virtual page table base) [HRM p
                           5-15..16] */
    u64 va_ctl_vptb; /**< IPR VA_CTL: vptb (virtual page table base) [HRM p 5-4]
                      */
    int asn1;  /**< IPR DTB_ASN1: asn (address space number) [HRM p 5-28] */
    int eien;  /**< IPR IER_CM: eien (external interrupt enable) [HRM p 5-9..10]
                */
    int slen;  /**< IPR IER_CM: slen (serial line interrupt enable) [HRM p
                  5-9..10] */
    int cren;  /**< IPR IER_CM: cren (corrected read error int enable) [HRM p
                  5-9..10] */
    int pcen;  /**< IPR IER_CM: pcen (perf counter interrupt enable) [HRM p
                  5-9..10] */
    int sien;  /**< IPR IER_CM: sien (software interrupt enable) [HRM p 5-9..10]
                */
    int asten; /**< IPR IER_CM: asten (AST interrupt enable) [HRM p 5-9..10] */
    int sir; /**< IPR SIRR: sir (software interrupt request) [HRM p 5-10..11] */
    std::atomic<int>
        eir; /**< external interrupt request; raised cross-thread by irq_h() */
    int slr; /**< serial line interrupt request */
    int crr; /**< corrected read error interrupt */
    int pcr; /**< perf counter interrupt */
    int astrr;       /**< IPR PCTX: astrr (AST request) [HRM p 5-21..22] */
    int aster;       /**< IPR PCTX: aster (AST enable) [HRM p 5-21..22] */
    u64 i_ctl_other; /**< various bits in IPR I_CTL that have no meaning to the
                        emulator */
    u64 mm_stat; /**< IPR MM_STAT: memory management status [HRM p 5-28..29] */
    bool hwe;    /**< IPR I_CLT: hwe (allow palmode ins in kernel mode) [HRM p
                    5-15..17] */
    int m_ctl_spe; /**< IPR M_CTL: spe (Super Page mode enabled) [HRM p
                      5-29..30] */
    int i_ctl_spe; /**< IPR I_CTL: spe (Super Page mode enabled) [HRM p
                      5-15..18] */
    u64 pmpc;
    u64 fpcr; /**< Floating-Point Control Register [HRM p 2-36] */
    bool bIntrFlag;

    /**
     * \brief Instruction cache entry.
     *
     * An instruction cache entry contains the address and address space number
     * (ASN) + 16 32-bit instructions. [HRM 2-11]
     **/
    struct SICache {
      int asn;                    /**< Address Space Number */
      u32 data[ICACHE_LINE_SIZE]; /**< Actual cached instructions  */
      u64 address;                /**< Address of first instruction */
      u64 p_address;              /**< Physical address of first instruction */
      bool asm_bit;               /**< Address Space Match bit */
      bool valid;                 /**< Valid cache entry */
      /** Which processor modes may execute this line: bit per mode, taken
          from the translation buffer when the line was filled. A hit has to
          check it, or a line filled for the kernel would go on answering
          fetches made in user mode -- an access violation that never
          happens. */
      u8 exec_modes;
    } icache[ICACHE_ENTRIES];     /**< Instruction cache entries [HRM p 2-11] */
    int next_icache;              /**< Number of next cache entry to use */
    int last_found_icache;        /**< Number of last cache entry found */

    /**
     * \brief Translation Buffer Entry.
     *
     * A translation buffer entry provides the mapping from a page of virtual
     *memory to a page of physical memory.
     **/
    struct STBEntry {
      u64 virt;       /**< Virtual address of page*/
      u64 phys;       /**< Physical address of page*/
      u64 match_mask; /**< The virtual address has to match for these bits to be
                         a hit*/
      u64 keep_mask;  /**< This part of the virtual address is OR-ed with the
                         phys address*/
      int asn;        /**< Address Space Number*/
      int asm_bit;    /**< Address Space Match bit*/
      int access[2][4]; /**< Access permitted [read/write][current mode]*/
      int fault[3];     /**< Fault on access [read/write/execute]*/
      bool valid;       /**< Valid entry*/
    } tb[2]
        [TB_ENTRIES]; /**< Unified Dstream TB model plus Istream TB entries */

    int next_tb[2]; /**< Number of next translation buffer entry to use */
    int last_found_tb[2]
                     [2]; /**< Number of last translation buffer entry found */
    u32 rem_ins_in_page;  /**< Number of instructions remaining in current page
                           */
    int iProcNum;     /**< number of the current processor (0 in a 1-processor
                         system) */
    u64 last_tb_virt; /**< ITB_TAG staging register for ITB_PTE writes */
    bool pal_vms; /**< True if the PALcode base is 0x8000 (=VMS PALcode base) */
    int irq_h_timer[6]; /**< Timers for delayed IRQ_H[0:5] assertion */
  } state; /**< Determines CPU state that needs to be saved to the state file */

  /// A shadow of 8 KB data translations, kept after the 128-entry TB evicts
  /// them. A real EV6 has 128 DTB entries; a program walking more pages than
  /// that misses on every one, and each miss is a trap into PALcode's
  /// DTB-miss handler -- measured on a 48 MB stride at 4.5M HW_MTPR + 4.4M
  /// HW_MFPR per 100M instructions, 19% of the section in the IPR helpers
  /// alone. Everything the handler would re-insert is what it inserted last
  /// time, so keep it: FindTBEntry refills the TB from here on a miss instead
  /// of trapping. Invisible to the guest as long as every invalidate the
  /// architecture defines is honoured, which tbia/tbiap/tbis do below. Only
  /// GH=0 (8 KB) entries are kept -- a larger-page entry spans many index
  /// slots and the few PALcode inserts of those are not the cost.
  static constexpr int kTbShadowBits = 12;
  static constexpr int kTbShadowEntries = 1 << kTbShadowBits;
  using STBEntry = std::remove_reference_t<decltype(state.tb[0][0])>;
  STBEntry m_tb_shadow[kTbShadowEntries] = {};
  u64 m_tb_shadow_refills = 0; // FindTBEntry refills from the shadow
  static inline u64 tb_shadow_index(u64 va) {
    return (va >> 13) & (u64)(kTbShadowEntries - 1);
  }
  int tb_refill_from_shadow(u64 virt, int asn); // -1 if the shadow has nothing


  u64 last_dtb_virt[2]; /**< DTB_TAG0/1 staging registers for DTB_PTE0/1 writes
                         */

#ifdef IDB
  u64 current_pc_physical; /**< Physical address of current instruction */
  u32 last_instruction;
  u64 last_read_loc;
  u64 last_write_loc;
#endif

  // Kept last on purpose: the compiled code reaches the page cache and the
  // register file with one displacement from `this`, and that only works
  // while they stay near the front of the object (see kDpcEntries above).
  // New members go here, behind everything the emitter addresses.
  std::chrono::steady_clock::time_point m_rate_last{};
  u64 m_rate_icount = 0;
  // The clock anchor lives here for the same reason as everything else in
  // this block: compiled code reaches the register file and the page
  // caches with one displacement from `this`, and a field inserted ahead
  // of them pushes those past the reach of that addressing. Putting these
  // two in the middle of the class emitted code that read from a null
  // pointer.
  u64 m_clk_ticks0 = 0;
  std::chrono::steady_clock::time_point m_clk_tp0{};
  u64 m_rate_cc = 0; // the cycle counter as of the last rate report
  u64 m_rate_escapes = 0, m_rate_entries = 0, m_rate_bulk = 0;
  u64 m_rate_flush_skipped = 0, m_rate_flush_done = 0;

public:
  /// How often compiled code leaves for the miscellaneous helper, by kind:
  /// [0] RPCC (the cycle counter), [1] RC, [2] RS.
  u64 m_misc_calls[3] = {0, 0, 0};

private:
  u64 m_rate_misc[3] = {0, 0, 0};

public:
  /// Interpret, never compile: set for every run inside the VM (the code
  /// cache cannot be shared with it) and by ALPHABOX_INTERP=1, which is the
  /// control arm that measures what running inside costs.
  bool m_interp_only = false;

  /// An IMB that has nothing to flush.
  ///
  /// The firmware issues IMB from a polling loop, and Windows from its
  /// scheduler: at the SRM prompt that is a hundred thousand flushes per
  /// hundred million instructions, each one costing an icache walk, an
  /// epoch bump that breaks every compiled chain, and a source re-hash for
  /// every block that runs again afterwards. Across a Windows boot those
  /// re-hashes have never once found a changed byte (300 million of them,
  /// measured). So the flush asks the machine's code-page map first: if
  /// nothing has been written to a page any block was compiled from since
  /// the last flush, there is nothing to flush, and it returns.
  ///
  /// What makes that safe is that every way guest memory can change reports
  /// to the map -- the interpreter's stores, the JIT's write helpers, a
  /// conditional store, DMA from a device thread, a firmware reload -- and
  /// that compiled code cannot store into a code page inline: such a page
  /// is never installed in the write half of the data page cache, so those
  /// stores take the helper, which reports. ALPHABOX_JIT_NOPFLUSH=0 keeps
  /// the old unconditional flush in the same binary.
  CCodePageMap *m_code_map = nullptr;
  u64 m_code_gen_seen = ~U64(0); // its write count at our last real flush
  u64 m_code_pages_seen = 0;     // pages it had marked when we last looked
  u64 m_flush_skipped = 0;       // flushes that turned out to have no work
  u64 m_flush_done = 0;          // ... and flushes that did
  bool m_nopflush = true;
  /// ALPHABOX_JIT_NOPFLUSH=2: flush unconditionally, as if the map were not
  /// there, but keep deciding what the map WOULD have said -- and shout if a
  /// block's source words turn out to have changed while it said nothing had
  /// been written. That is the failure this optimisation can have, and the
  /// only way to be sure of a write path we have not thought of is to run a
  /// guest with this on and see the count stay at zero.
  bool m_nopflush_audit = false;
  bool m_flush_was_clean = false; // the map said "nothing written" last time
  /// ALPHABOX_JIT_NOPFLUSH_BREAK=1 silences the processor's own stores, as
  /// though a write path had been forgotten. It exists so the test that
  /// modifies guest code can be shown to fail when the tracking is wrong --
  /// a test that cannot fail proves nothing (test/tools/smc_test.sh).
  bool m_nopflush_break = false;
};

/** Translate raw register (0..31) number to a number that takes PALshadow
    registers into consideration (0..63). Considers the program counter
    (to determine if we're in PALmode), and the SDE (Shadow Enable) bit. */
#define RREG(a)                                                                \
  (((a)&0x1f) + (((state.pc & 1) && (((a)&0xc) == 0x4) && state.sde) ? 32 : 0))

// Tell the JIT which caller's icache flush this is, for the epoch census.
// A flush rejects every cached block link, so knowing which event does it is
// what tells us whether the link guard can be replaced.
#ifdef ES40_JIT
#define JIT_FLUSH_CAUSE(c)                                                     \
  do {                                                                         \
    if (m_jit)                                                                 \
      m_jit->set_flush_cause(CJitEngine::c);                                   \
  } while (0)
#else
#define JIT_FLUSH_CAUSE(c)                                                     \
  do {                                                                         \
  } while (0)
#endif

/**
 * Empty the instruction cache.
 **/
inline void CAlphaCPU::flush_icache() {
  note_ic_flush_pc();
  // Nothing written to a page that holds code since the last flush means
  // there is nothing this flush could invalidate (see m_code_map).
  if (m_nopflush && m_code_map) {
    if (m_code_map->code_pages() != m_code_pages_seen)
      honour_new_code_pages(); // a page became code: drop stale inline writes
    const u64 gen = m_code_map->write_gen();
    m_flush_was_clean = (gen == m_code_gen_seen);
    if (m_flush_was_clean) {
      ++m_flush_skipped;
      if (!m_nopflush_audit)
        return;
    } else {
      m_code_gen_seen = gen;
    }
  }
  ++m_flush_done;
  if (icache_enabled) {
    for (int i = 0; i < ICACHE_ENTRIES; i++) {
      state.icache[i].valid = false;
    }
    state.next_icache = 0; // old version, may be relied on elsewhere
    state.last_found_icache = 0;
  }
  break_seq_icache();
#ifdef ES40_JIT
  jit_flush_blocks();
  m_jit_code_seen = ++g_jit_code_flush; // our own flush is already done
#endif
}

/**
 * Empty the instruction cache of lines with the ASM bit clear.
 **/
inline void CAlphaCPU::flush_icache_asm() {
  if (icache_enabled) {
    int i;
    for (i = 0; i < ICACHE_ENTRIES; i++)
      if (!state.icache[i].asm_bit)
        state.icache[i].valid = false;
  }
  break_seq_icache();
#ifdef ES40_JIT
  jit_flush_blocks_asm(); // preserve global (ASM-bit) JIT blocks, matching the
                          // icache ASM-bit rule
#endif
}

/**
 * Set the PALcode BASE register, and determine whether we're running VMS
 *PALcode.
 **/
inline void CAlphaCPU::set_PAL_BASE(u64 pb) {
  state.pal_base = pb;
  bool was_vms = state.pal_vms;

  // VMS PALcode uses base 0x8000
  state.pal_vms = (pb == U64(0x8000)) && !vmspal_lle_enabled;
  // state.pal_vms = false;

#ifdef DEBUG_PAL
  printf("%%CPU-I-PALSWITCH: PAL=%016" PRIx64 " p21=%016" PRIx64
         " p22=%016" PRIx64 " r22=%016" PRIx64 "\n",
         pb, state.r[53], state.r[54], state.r[22]);
  // Dump PAL scratch area contents for non-VMS PAL
  if (!state.pal_vms && state.r[53] != 0) {

    u64 scratch = U64(0x7cf420);

    printf("%%CPU-I-PALSCR: Scratch area at %016" PRIx64 ":\n", scratch);
    printf("%%CPU-I-PALSCR:   +0x00 VPTB = %016" PRIx64 "\n",
           sys_read(scratch + 0x00, 64));
    printf("%%CPU-I-PALSCR:   +0x08 PTBR = %016" PRIx64 "\n",
           sys_read(scratch + 0x08, 64));
    printf("%%CPU-I-PALSCR:   +0x10 PCBB = %016" PRIx64 "\n",
           sys_read(scratch + 0x10, 64));
    printf("%%CPU-I-PALSCR:   +0x18 KSP  = %016" PRIx64 "\n",
           sys_read(scratch + 0x18, 64));
    printf("%%CPU-I-PALSCR:   +0x98 WHAMI= %016" PRIx64 "\n",
           sys_read(scratch + 0x98, 64));
    printf("%%CPU-I-PALSCR:   +0x170 SCBB= %016" PRIx64 "\n",
           sys_read(scratch + 0x170, 64));
  } else if (!state.pal_vms && state.r[53] == 0) {
    printf(
        "%%CPU-W-NOP21: PAL switched but p21=0! Scratch area not available.\n");
  }
#endif
}

/**
 * Get an instruction from the instruction cache.
 * If necessary, fill a new cache block from memory.
 *
 * get_icache checks all cache entries, to see if there is a
 * cache entry that matches the current address space number,
 * and that contains the address we're looking for. If it
 * exists, the instruction is fetched from this cache,
 * otherwise, the physical address for the instruction is
 * calculated, and the cache block is filled.
 *
 * The last cache entry that was a hit is remembered, so that
 * cache entry is checked first on the next instruction. (very
 * likely to be the same cache block)
 *
 * It would be easiest to do without the instruction cache
 * altogether, but unfortunately SRM uses self-modifying
 * code, that relies on the correct instruction stream to
 * remain in the cache.
 **/
/**
 * Which processor modes may execute the page this line was filled from.
 *
 * A PALmode fetch does not go through the translation buffer at all, so
 * such a line answers in every mode -- the PAL bit is part of the tag, so
 * a native fetch cannot reach it anyway. Otherwise the answer is the
 * I-stream translation's per-mode read permission, which is what
 * virt2phys consulted to let this fill happen in the first place.
 **/
inline u8 CAlphaCPU::icache_exec_modes(u64 address, u64 v_a) {
  if (address & 1)
    return 0xf;

  const int i = FindTBEntry(v_a, ACCESS_EXEC);
  if (i < 0)
    return (u8)(1 << state.cm); // no entry to ask: trust this fetch only

  u8 modes = 0;
  for (int m = 0; m < 4; m++)
    if (state.tb[TB_INDEX_ITB][i].access[0][m])
      modes |= (u8)(1 << m);
  return modes;
}

inline int CAlphaCPU::get_icache(u64 address, u32 *data) {
  // Direct-map the icache: 2 KiB lines (ICACHE_LINE_SIZE * 4 == 2048 bytes).
  // Use VA[...:11] as the set index. The PAL bit (VA<0>) remains part of the
  // tag.
  const u64 kLineShift = 11;
  const u64 v_aligned = address & ~U64(0x3);
  const int i = (int)((v_aligned >> kLineShift) & (ICACHE_ENTRIES - 1));
  u64 v_a;
  u64 p_a;
  int result;
  bool asm_bit;

  if (icache_enabled) {
    // ---- Fast hit probe
    if (state.icache[i].valid &&
        (state.icache[i].asn == state.asn || state.icache[i].asm_bit) &&
        ((state.icache[i].exec_modes >> state.cm) & 1) &&
        state.icache[i].address == (address & ICACHE_MATCH_MASK)) {

      *data =
          endian_32(state.icache[i].data[(address >> 2) & ICACHE_INDEX_MASK]);

      // keep debug/pc_phys coherent even when icache is enabled
      state.pc_phys = state.icache[i].p_address + (address & ICACHE_BYTE_MASK);

#ifdef IDB
      current_pc_physical = state.pc_phys;
#endif
      state.last_found_icache = i;
      return 0;
    }

    // ---- Miss: translate + fill
    v_a = address & ICACHE_MATCH_MASK;
    if (address & 1) {
      // PALmode: VA<0> is PAL marker; physical is VA with bit0 cleared
      p_a = v_a & ~U64(0x1);
      asm_bit = true;
    } else {
      result = virt2phys(v_a, &p_a, ACCESS_EXEC, &asm_bit, 0);
      if (result)
        return result;
    }

    // Attempt to get a pointer into DRAM. If this is PIO (e.g., TIG flash),
    // PtrToMem returns null and we must *not* try to memcpy from it.
    char *mem = cSystem->PtrToMem(p_a);
    if (mem && cSystem->PtrToMem(p_a + ((ICACHE_LINE_SIZE * 4) - 1))) {
      // DRAM-backed: fill the direct-mapped icache line.
      memcpy(state.icache[i].data, mem, ICACHE_LINE_SIZE * 4);
      state.icache[i].valid = true;
      state.icache[i].asn = state.asn;
      state.icache[i].asm_bit = asm_bit;
      state.icache[i].address = address & ICACHE_MATCH_MASK;
      state.icache[i].p_address = p_a;
      state.icache[i].exec_modes = icache_exec_modes(address, v_a);

      *data =
          endian_32(state.icache[i].data[(address >> 2) & ICACHE_INDEX_MASK]);

      // same pc_phys update on fill
      state.pc_phys = p_a + (address & ICACHE_BYTE_MASK);
#ifdef IDB
      current_pc_physical = state.pc_phys;
#endif
      state.last_found_icache = i;
      return 0;
    } else {
      // PIO/TIG-backed: cannot fill icache lines.
      // Read exactly the requested instruction as 4 byte reads via the system
      // bus.
      const u64 p_instr = p_a + (address & ICACHE_BYTE_MASK);
      u32 ins = 0;
      ins |= (u8)sys_read(p_instr + 0, 8);
      ins |= ((u8)sys_read(p_instr + 1, 8)) << 8;
      ins |= ((u8)sys_read(p_instr + 2, 8)) << 16;
      ins |= ((u8)sys_read(p_instr + 3, 8)) << 24;
      *data = ins; // already in target little-endian form

      state.pc_phys = p_instr;
#ifdef IDB
      current_pc_physical = state.pc_phys;
#endif
      return 0;
    }
  }

  // ---- Icache disabled (unchanged)
  if (address & 1) {
    state.pc_phys = address & ~U64(0x3);
    state.rem_ins_in_page = 1;
  } else {
    if (!state.rem_ins_in_page) {
      result = virt2phys(address, &state.pc_phys, ACCESS_EXEC, &asm_bit, 0);
      if (result)
        return result;
      state.rem_ins_in_page = 2048 - ((((u32)address) >> 2) & 2047);
    }
  }

  *data = (u32)sys_read(state.pc_phys, 32);
  return 0;
}

/**
 * Convert a virtual address to va_form format.
 * Used for IPR VA_FORM [HRM 5-5..6] and IPR IVA_FORM [HRM 5-9].
 **/
inline u64 CAlphaCPU::va_form(u64 address, bool bIBOX) {
  switch (bIBOX ? state.i_ctl_va_mode : state.va_ctl_va_mode) {
  case 0:
    return ((bIBOX ? state.i_ctl_vptb : state.va_ctl_vptb) &
            U64(0xfffffffe00000000)) |
           ((address >> 10) & U64(0x00000001fffffff8));

  case 1:
    return ((bIBOX ? state.i_ctl_vptb : state.va_ctl_vptb) &
            U64(0xfffff80000000000)) |
           ((address >> 10) & U64(0x0000003ffffffff8)) |
           (((address >> 10) & U64(0x0000002000000000)) * U64(0x3e));

  case 2:
    return ((bIBOX ? state.i_ctl_vptb : state.va_ctl_vptb) &
            U64(0xffffffffc0000000)) |
           ((address >> 10) & U64(0x00000000003ffff8));
  }

  return 0;
}

/**
 * Return processor number.
 **/
inline int CAlphaCPU::get_cpuid() { return state.iProcNum; }

/**
 * True when execute()'s interrupt poll would deliver something: an enabled
 * external or software interrupt request, or an enabled AST at or below the
 * current mode. IPR writes that change these inputs (CM, IER, SIRR, AST) use it
 * to raise check_int only when needed -- check_int keeps compiled code out of
 * the dispatcher, and Windows rewrites IER on every IRQL change, so an
 * unconditional kick sent ~1.8M chain exits a second through the interpreter.
 * Device lines raise check_int themselves (irq_h), so nothing is missed.
 **/
inline bool CAlphaCPU::int_deliverable() const {
  return (state.eien & state.eir) || (state.sien & state.sir) ||
         (state.asten &&
          (state.aster & state.astrr & ((1 << (state.cm + 1)) - 1)));
}

/**
 * Assert or release an external interrupt line to the cpu.
 **/
inline void CAlphaCPU::irq_h(int number, bool assert, int delay) {
  bool active = (state.eir & (U64(0x1) << number)) || state.irq_h_timer[number];
  if (assert && !active) {
    if (delay) {
      state.irq_h_timer[number] = delay;
      state.check_timers = true;
    } else {
      state.eir |= (U64(0x1) << number);
      state.check_int = true;
    }
#ifdef ES40_JIT
    idle_wake(); // after the flags: a sleeper that missed them is notified
#endif

    return;
  }

  if (assert && (state.eir & (U64(0x1) << number))) {
    // A new request on a line that is already high (a second device asserting
    // while the first is still being serviced): eir is already right, but
    // check_int was consumed when the interrupt was dispatched and nothing
    // re-polls after its REI. Re-kick it for the next dispatch boundary.
    state.check_int = true;
#ifdef ES40_JIT
    idle_wake();
#endif
    return;
  }

  if (!assert && active) {
    state.eir &= ~(U64(0x1) << number);
    state.irq_h_timer[number] = 0;
    state.check_timers = false;
    for (int i = 0; i < 6; i++) {
      if (state.irq_h_timer[i])
        state.check_timers = true;
    }
  }
}

/**
 * Return program counter value.
 **/
inline u64 CAlphaCPU::get_pc() { return state.pc; }

#ifdef IDB

/**
 * Return the physical address the program counter refers to.
 **/
inline u64 CAlphaCPU::get_current_pc_physical() { return state.pc_phys; }
#endif

/**
 * Return program counter value without PALmode bit.
 **/
inline u64 CAlphaCPU::get_clean_pc() { return state.pc & ~U64(0x3); }

/**
 * Jump to next instruction
 **/
inline void CAlphaCPU::next_pc() {
  state.pc += 4;
  state.pc_phys += 4;
  if (state.rem_ins_in_page)
    state.rem_ins_in_page--;
}

/**
 * Set program counter to a certain value.
 **/
inline void CAlphaCPU::set_pc(u64 p_pc) {
  state.pc = p_pc;
  state.rem_ins_in_page = 0;
  seq_remaining = 0;
}

/**
 * Add  value to the program counter.
 **/
inline void CAlphaCPU::add_pc(u64 a_pc) {
  state.pc += a_pc;
  state.rem_ins_in_page = 0;
  seq_remaining = 0;
}

/**
 * Get a register value.
 * If \a translate is true, use shadow registers if currently enabled.
 **/
inline u64 CAlphaCPU::get_r(int i, bool translate) {
  if (translate)
    return state.r[RREG(i)];
  else
    return state.r[i];
}

/**
 * Get a fp register value.
 **/
inline u64 CAlphaCPU::get_f(int i) { return state.f[i]; }

/**
 * Set a register value
 **/
inline void CAlphaCPU::set_r(int reg, u64 value) { state.r[reg] = value; }

/**
 * Set a fp register value
 **/
inline void CAlphaCPU::set_f(int reg, u64 value) { state.f[reg] = value; }

/**
 * Get the PALcode base register.
 **/
inline u64 CAlphaCPU::get_pal_base() { return state.pal_base; }

/**
 * Get the processor base register.
 * A bit fuzzy...
 **/
inline u64 CAlphaCPU::get_prbr(void) {
  u64 v_prbr; // virtual
  u64 p_prbr; // physical
  bool b;
  if (state.r[21 + 32] && ((u64)(state.r[21 + 32] + 0xaf) <
                           (u64)((U64(0x1) << cSystem->get_memory_bits()))))
    v_prbr = sys_read(state.r[21 + 32] + 0xa8, 64);
  else
    v_prbr = sys_read(0x70a8 + (0x200 * get_cpuid()), 64);
  if (virt2phys(v_prbr, &p_prbr, ACCESS_READ | FAKE | NO_CHECK, &b, 0))
    p_prbr = v_prbr;
  if ((u64)p_prbr > (u64)(U64(0x1) << cSystem->get_memory_bits()))
    p_prbr = 0;
  return p_prbr;
}

/**
 * Get the hardware process control block address.
 **/
inline u64 CAlphaCPU::get_hwpcb(void) {
  u64 v_pcb; // virtual
  u64 p_pcb; // physical
  bool b;
  if (state.r[21 + 32] && ((u64)(state.r[21 + 32] + 0x17) <
                           (u64)((U64(0x1) << cSystem->get_memory_bits()))))
    v_pcb = sys_read(state.r[21 + 32] + 0x10, 64);
  else
    v_pcb = sys_read(0x7010 + (0x200 * get_cpuid()), 64);
  if (virt2phys(v_pcb, &p_pcb, ACCESS_READ | NO_CHECK | FAKE, &b, 0))
    p_pcb = v_pcb;
  if (p_pcb > (u64)(U64(0x1) << cSystem->get_memory_bits()))
    p_pcb = 0;
  return p_pcb;
}

#if defined(IDB)
/**
 * Return the last instruction executed.
 **/
inline u32 CAlphaCPU::get_last_instruction(void) { return last_instruction; }
#endif
extern bool bTB_Debug;
#endif // !defined(INCLUDED_ALPHACPU_H)
