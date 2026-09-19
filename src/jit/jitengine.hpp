/* Alphabox Alpha Emulator -- JIT engine.
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
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
 * Per-CPU, direct-mapped cache of translation blocks. The dispatcher runs a
 * block's compiled safe-ALU prefix natively, then interprets the remainder.
 *
 * Blocks are keyed by VIRTUAL PC + ASN (like the icache), so the dispatch hot
 * path needs no address translation. A TB invalidation flushes the cache (see
 * flush()); a global (ASM) block matches any ASN.
 */
#if !defined(INCLUDED_JITENGINE_H)
#define INCLUDED_JITENGINE_H

#ifdef ES40_JIT

#include <chrono> // jit_tsc_ns calibrates the counter against steady_clock
#include "config_debug.hpp" // JIT_VERIFY
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#ifdef JIT_STATS
#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h> // __rdtsc -- host TSC for the JIT_STATS wall-time split
#else
#include <x86intrin.h>
#endif
static inline uint64_t jit_rdtsc() { return __rdtsc(); }
#else
// AArch64: the virtual counter register (cntvct_el0), 24 MHz on Apple
// Silicon and ~0.3 ns to read. steady_clock was ~15 ns a read, which is about
// a cheap helper's whole body, so timing helpers with it measured the clock.
// Units are counter ticks, not cycles or ns; every share here is a ratio of
// the same clock, and jit_tsc_ns() converts a per-call figure for printing.
static inline uint64_t jit_rdtsc() { return __builtin_readcyclecounter(); }
#endif
// Per-call figures: ticks -> ns. x86's TSC is not converted (it prints as
// cycles). The AArch64 counter's rate is measured once against steady_clock,
// because assuming it was wrong: hw.tbfrequency says 24 MHz on this host and
// the counter the builtin reads runs at 1 GHz, which put every per-call
// figure out by 42x until a two-line test caught it.
static inline double jit_tsc_ns(double ticks) {
#if defined(_M_X64) || defined(__x86_64__)
  return ticks;
#else
  static const double ns_per_tick = [] {
    const auto c0 = std::chrono::steady_clock::now();
    const uint64_t t0 = jit_rdtsc();
    while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(20)) {
    }
    const uint64_t t1 = jit_rdtsc();
    const double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - c0)
                          .count();
    return (t1 > t0) ? ns / (double)(t1 - t0) : 1.0;
  }();
  return ticks * ns_per_tick;
#endif
}
#endif
#if defined(JIT_REGPROF) && !defined(JIT_STATS)
#error                                                                         \
    "JIT_REGPROF needs JIT_STATS (its report rides note_exec's 100M-instruction window)"
#endif
#ifdef JIT_DISASM
#include <cstdio> // FILE* for the per-CPU disassembly trace
#endif

class CAlphaCPU; // compiled blocks call back into the CPU for memory accesses

class CJitEngine {
public:
  // 256K slots: the OS-active CPU's block working set (50K+) thrashed the old
  // 16K direct-mapped cache
  // (~190K recompiles/100M); 64K cut that to ~14K/100M, but a 50K set in 64K
  // slots still conflict-evicts (load ~0.8). 256K drops the load to ~0.2.
  // JitBlock ~110 B -> ~28 MB/CPU of metadata.
  static constexpr int kCacheBits = 18;
  static constexpr int kCacheEntries = 1 << kCacheBits;
  static constexpr uint64_t kIndexMask = (uint64_t)kCacheEntries - 1;

  // Trace tier (M0+): a small SECOND cache, beside m_blocks, for hot superblock
  // heads. Only the hottest loop heads are promoted, so it stays small. Same
  // direct-mapped, virtual+ASN-keyed shape.
  static constexpr int kTraceBits = 12; // 4K trace heads
  static constexpr uint64_t kTraceEntries = 1 << kTraceBits;
  static constexpr uint64_t kTraceIndexMask = kTraceEntries - 1;
  static constexpr uint32_t kMaxTraceSegs =
      16; // fused blocks per trace (multi-block coherence)
  static constexpr uint32_t kMaxTraceExits =
      16; // guards / side-exits per trace

  // Reclaim executable memory once compiled code passes this many bytes, rather
  // than tearing down the asmjit runtime on every flush (see flush()). A
  // reclaim frees ALL compiled code, so the budget must hold the guest's hot
  // working set: AArch64 output is ~2x the x86-64 size per Alpha instruction
  // (~135 bytes), and at 32 MB Windows 2000 setup reclaimed every few seconds
  // and spent ~30% of its time recompiling the same blocks; its boot alone
  // reaches ~200 MB of compiled code.
#if defined(__aarch64__) || defined(_M_ARM64)
  static constexpr uint64_t kReclaimBytes = 768 * 1024 * 1024;
#else
  static constexpr uint64_t kReclaimBytes = 32 * 1024 * 1024;
#endif

  // Compiled block entry point. Runs the prefix on regs[0..31], calling back
  // into cpu for memory accesses; returns the number of instructions fully
  // completed
  typedef uint32_t (*JitFn)(CAlphaCPU *cpu, uint64_t *regs);

  static constexpr int kLinkSlots =
      2; // cached direct successors per block (poly-link). Instrumentation
         // showed the thrashing fanout is EXACTLY 2; bump only if f3/f4 appear.

  struct JitBlock {
    uint64_t tag;     // start VIRTUAL PC (validity tag / key)
    uint64_t phys;    // start physical PC (source bytes for compilation)
    uint32_t asn;     // address space number (key; ignored when asm_global)
    uint8_t cm;       // the processor mode this block was translated for: part
                      // of the key, because whether the mode may execute the
                      // page was settled once, when it was translated. A
                      // global (ASM) page is global across address spaces, not
                      // across modes.
    bool asm_global;  // global (ASM) page: matches any ASN, like the icache
    uint32_t n_instr; // instructions in the straight-line block
    bool valid;
    JitFn code; // compiled safe-prefix, or null (prologue entry, for C calls)
    void *jit_body; // chained re-entry point (after the prologue); null when
                    // not compiled
    JitBlock *link[kLinkSlots]; // cached direct successors (poly-link,
                                // round-robin back-patched); null = empty
    uintptr_t inbound; // head of the list of static exits linked INTO this
                       // block, as ExitRec* | slot (QEMU's jmp_list_head).
                       // Those links carry no epoch, so invalidating this
                       // block means walking this list and clearing them --
                       // see unlink_inbound().
#ifdef JIT_STATS
    uint32_t link_misses; // instrumentation: per-source link-miss count,
                          // cumulative (poly-link sizing)
    uint8_t link_fanout; // distinct re-link targets seen (saturates at 4; >4 =>
                         // a small successor cache won't help)
    uint64_t link_seen[4];
#endif
    uint32_t prefix_len; // # safe ALU ops in code
    bool compiled;       // compile has been attempted
    uint32_t body_off; // jit_body's offset within code -- restores the chained
                       // entry on revalidate
    uint64_t src_sum; // hash of the source words at compile time (revalidate vs
                      // self-mod)
    uint32_t hash_len; // word count src_sum covers -- frozen at compile time;
                       // n_instr drifts (interrupt-truncated cold passes shrink
                       // it), so it must NOT key the hash
    uint64_t vgen; // m_itb_gen + m_flush_gen at last full validation (phys +
                   // code bytes). Both counters are monotonic, so one sum
                   // compare detects either changing
                   // -- the single chain guard (see emit_chain / jit_indirect).
    uint64_t flush_gen; // icache-flush generation at which the code bytes were
                        // last hash-validated; stale => lookup misses and
                        // revalidate_flushed() re-hashes (lazy IC_FLUSH)
    uint32_t hot; // dispatches since record; at the promote threshold -> form a
                  // trace
    uint32_t cold_runs; // interpreted passes since record; compiled once this
                        // reaches compile_after() (hotness threshold)
#ifdef JIT_REGPROF
    uint64_t rp_hits; // REGPROF: block executions since record (body-entry inc
                      // -- counts chained runs)
    uint32_t rp_mask; // REGPROF: Alpha GPRs touched by this block (bit r);
                      // compile-time, exec-weighted at report
    uint32_t rp_csz;  // REGPROF: emitted x86 bytes for this block -- rp_hits x
                      // rp_csz = exec-weighted expansion
    // REGPROF: inline memory ops in the block, and those following a memory op
    // on the same base register and page-cache row with the probe state
    // intact (DPC-reuse candidates); near = displacements within 8 KB.
    uint32_t rp_memops;
    uint32_t rp_dpc_pairs;
    uint32_t rp_dpc_near;
#endif
  };

  // Per fused block: the source-coherence descriptor (review: multi-block
  // traces need this). A trace spans multiple blocks/pages, so a single head
  // tag + epoch is not enough -- trace_ok() re-hashes these on an epoch change,
  // mirroring revalidate_flushed, and also stores what to re-form.
  struct SourceSeg {
    uint64_t guest_pc; // segment start virtual PC
    uint64_t phys_pc;  // segment start physical PC (source bytes)
    uint32_t n_instr;  // instructions in the segment (hash length)
    bool asm_global;   // global (ASM) segment
    uint32_t asn;      // ASN (ignored when asm_global)
    uint64_t src_sum;  // hash of the segment's source words at build
  };

  // M4+: which guest regs are held in host registers (not yet committed to
  // state.r[]) at a side-exit. Empty through M3 (no register cache across
  // guards), so the side-exit needs no spill until M4.
  struct Snapshot {
    uint64_t dirty_gpr;
    uint64_t dirty_fpr;
  };

  struct TraceExit {
    uint64_t guest_pc; // resume PC handed to the block dispatcher (compile-time
                       // constant)
    Snapshot snap;     // M4+ (zero until then)
  };

  struct TraceFragment {
    uint64_t head_tag; // entry virtual PC (key)
    uint32_t asn;      // key (ignored when asm_global)
    bool asm_global;
    bool valid;
    JitFn code; // single entry; null = empty slot
    uint64_t
        vgen; // build epoch = m_itb_gen + m_flush_gen (coherence; see trace_ok)
    uint64_t flush_gen; // IC-flush epoch at build
    uint32_t n_blocks, n_instr;
    SourceSeg segs[kMaxTraceSegs];
    uint32_t n_segs;
    TraceExit exits[kMaxTraceExits];
    uint32_t n_exits;
  };

  // Byte offsets (from the CAlphaCPU*) of the fields the inline load fast path
  // reads, so compiled code can touch them via [rsi + offset]. Filled once by
  // set_offsets().
  struct JitOffsets {
    uint32_t dpc_tag,  // the packed {page, asn, cm} the fast path compares
        dpc_bias,      // host address of the page, less its virtual address
        dpc_key;       // the live {asn, cm} half of a tag (CAlphaCPU member)
    uint32_t dpc_valid, dpc_virt_page, dpc_phys_base, dpc_host_base, dpc_cm,
        dpc_asn; // offsets of READ slot [0][0]
    uint32_t dpc_stride,
        dpc_mask; // direct-mapped page cache: per-slot byte stride, index mask
    uint32_t dpc_write_row; // byte distance from read cache [0] to write cache
                            // [1] (store fast path)
    uint32_t state_cm, state_asn0, dram_ptr, dram_size, state_pc;
    uint32_t state_current_pc; // GO_PAL takes EXC_ADDR from it (FLTV traps)
    uint32_t fpen, exc_sum, fpcr,
        f_base; // FP inline path: FPSTART gate + FPCR (rounding/INE) + f[] base
                // (f[i] = f_base + i*8)
    // For chaining: the budget ceiling and the interrupt-poll flags the
    // compiled epilogue checks before jumping on; link_from is where the
    // epilogue records a link-patch request.
    uint32_t jit_budget, check_int, check_timers, link_from;
    uint32_t link_target; // static exit's target PC, recorded with link_from
    uint32_t exc_addr, pal_base,
        sde; // CALL_PAL: exc_addr save, PAL entry base, PALshadow enable
    // Inline HW_MTPR IER (a64): the enable fields it stores and the request /
    // AST fields int_deliverable() reads (all 32-bit ints; eir is
    // std::atomic<int>).
    uint32_t ier_asten, ier_sien, ier_pcen, ier_cren, ier_slen, ier_eien;
    uint32_t sir, eir, aster, astrr;
    uint32_t regs; // state.r[0] (compiled code's x20 in production)
  };
  void set_offsets(const JitOffsets &o) { m_off = o; }
  // Hotness threshold: a block is compiled only after it has been interpreted
  // this many times. Compiling costs far more than interpreting a short block
  // a few times, so one-shot code (driver init, setup loaders) stays cheap.
  uint32_t compile_after() const { return m_compile_after; }

  // Why the dispatcher interpreted instead of running compiled code
  // (JIT_STATS cold-path accounting, see note_cold()).
  enum ColdReason {
    CR_NO_PHYS,      // start PC didn't translate side-effect-free
    CR_NO_BLOCK,     // no block recorded at this PC yet
    CR_NOT_HOT,      // recorded, below the compile threshold
    CR_UNCOMPILABLE, // compiled, but its first instruction can't be
    CR_STALE,        // block's physical no longer matches the live mapping
    CR_INT,          // interrupt pending (check_int), block not in PALmode
    CR_INT_PAL,      // ...block in PALmode (can't take it before leaving PAL)
    CR_TIMER,        // delayed interrupt countdown pending (check_timers)
    CR_PAL_NOSDE,    // PALmode block while shadow registers are off
    CR_BUDGET,       // prefix longer than the remaining dispatch budget
    CR_DONE0,        // compiled block ran but completed no instruction
    CR_COUNT
  };
  // Why a memory helper refused (bailed to the interpreter).
  enum BailKind {
    BK_UNALIGNED,
    BK_TB_MISS,
    BK_ACV,
    BK_FAULT,
    BK_MMIO,
    BK_COUNT
  };
  // C++ helper entries from compiled code (JIT_STATS): which slow paths run.
  enum HelperKind {
    HK_READ,
    HK_WRITE,
    HK_LOCKED,
    HK_STC,
    HK_INDIRECT,
    HK_READ_PHYS,
    HK_WRITE_PHYS,
    HK_MTPR,
    HK_MFPR,
    HK_COUNT
  };
#ifdef JIT_STATS
  inline void note_cold(int reason, uint32_t n_instr, uint32_t first_op) {
    m_cold_n[reason]++;
    m_cold_instr[reason] += n_instr;
    if (reason == CR_UNCOMPILABLE)
      m_cold_op[first_op & 63] += n_instr;
    if (reason == CR_DONE0)
      m_done0_op[first_op & 63]++;
  }
  inline void note_bail(bool write, int kind) {
    m_bail_kind[write ? 1 : 0][kind]++;
  }
  // Hot guest code: compiled-chain entry PCs weighted by the instructions the
  // chain ran (direct-mapped, a colliding heavier entry keeps its slot). phys
  // + dram locate the guest words dumped for the top entries.
  inline void note_hot_pc(uint64_t pc, uint64_t phys, uint32_t n,
                          const uint8_t *dram) {
    HotPc &h = m_hot_pc[(uint64_t)((pc >> 2) * UINT64_C(0x9E3779B97F4A7C15)) >>
                        (64 - kHotPcBits)];
    if (h.pc != pc) {
      if (h.n > n) {
        h.n -= n;
        return;
      }
      h.pc = pc;
      h.phys = phys;
      h.n = 0;
    }
    h.n += n;
    m_hot_dram = dram;
  }
  inline void note_helper(int kind) { m_helper_n[kind]++; }
  inline void note_mtpr(uint32_t fn) { m_mtpr_rt[fn & 0xff]++; }
  inline void set_dpc_flush_counter(const uint64_t *p) { m_dpc_flush_src = p; }
#else
  inline void note_helper(int) {}
  inline void note_mtpr(uint32_t) {}
  inline void set_dpc_flush_counter(const uint64_t *) {}
  inline void note_bail(bool, int) {}
#endif

  // Per-op helper function pointers
  struct HelperSet {
    void *read_helper;
    void *write_helper;
    void *opcdec_helper;
    void *hw_mfpr_helper;
    void *hw_ld_helper;
    void *hw_mtpr_helper;
    void *hw_st_helper;
    void *indirect_helper;
    void *read_locked_helper;
    void *stc_helper;
    void *misc_helper;
    void *read_vpte_helper;
    void *read_wchk_helper;
    void *itof_helper;
    void *ftoi_helper;
    void *fltl_helper;
    void *fp_read_helper;
    void *fp_write_helper;
    void *fltv_helper;
  };

  explicit CJitEngine(
      int cpu_id = 0); // cpu_id tags the stats/diagnostic prints

  /// The identity AMASK and IMPLVER report, from the processor this engine
  /// compiles for (CpuModel.hpp). Compiled code holds them as immediates,
  /// and every engine belongs to one CPU, so a machine whose processors
  /// differ still gets the right values.
  void set_cpu_identity(uint64_t amask, uint64_t implver) {
    m_amask = amask;
    m_implver = implver;
  }
  ~CJitEngine();

  static inline uint64_t index_of(uint64_t virt_pc) {
    return (virt_pc >> 2) & kIndexMask;
  }
  static inline uint64_t trace_index_of(uint64_t virt_pc) {
    return (virt_pc >> 2) & kTraceIndexMask;
  }

  // Virtual+ASN keyed: no translation on the dispatch hot path. A global (ASM)
  // block matches any ASN, mirroring the icache's hit rule. flush_gen-stale
  // blocks miss here; revalidate_flushed() resurrects them after a source-hash
  // check.
  inline JitBlock *lookup(uint64_t virt_pc, uint32_t asn, uint8_t cm) {
    JitBlock &b = m_blocks[index_of(virt_pc)];
    return (b.valid && b.flush_gen == m_flush_gen && b.tag == virt_pc &&
            b.cm == cm && (b.asm_global || b.asn == asn))
               ? &b
               : nullptr;
  }

  // Trace tier (M0+): the global kill-switch + the trace-cache lookup.
  // traces_enabled() is false until M1 enables a region, so the dispatcher hook
  // is inert (one predictable-not-taken branch) and the engine is bit-identical
  // to the block-only build. Unlike the block lookup, this does NOT gate on
  // flush_gen -- trace_ok() owns all staleness (so an unrelated flush
  // re-validates instead of dropping).
  inline bool traces_enabled() const { return m_traces_enabled; }
  inline void set_traces_enabled(bool e) { m_traces_enabled = e; }

  // the slot a head PC maps to (formation fills it). Unlike trace_lookup,
  // returns the slot unconditionally so  the caller decides whether to (re)form
  // into it.
  inline TraceFragment *trace_slot(uint64_t head_pc) {
    return &m_traces[trace_index_of(head_pc)];
  }
  inline void note_trace_stale() { // always defined (callable from trace_ok);
                                   // counts only under JIT_STATS
#ifdef JIT_STATS
    m_trace_stale++;
#endif
  }
  inline void
  note_link_edge(JitBlock *src,
                 uint64_t tgt_tag) { // instrument a source block's successor
                                     // fanout (poly-link sizing)
#ifdef JIT_STATS
    src->link_misses++;
    for (int i = 0; i < src->link_fanout && i < 4; ++i)
      if (src->link_seen[i] == tgt_tag)
        return; // already counted
    if (src->link_fanout < 4)
      src->link_seen[src->link_fanout] = tgt_tag;
    if (src->link_fanout < 250)
      src->link_fanout++;
#else
    (void)src;
    (void)tgt_tag;
#endif
  }
#ifdef JIT_STATS
  inline void trace_entered() { m_trace_entered++; }
  inline void trace_exited() {
    m_trace_exits++;
  } // a trace side-exited / underran its first-pass span
#endif

  // When this tier is revived, its key needs the processor mode as a block's
  // does (JitBlock::cm): whether a mode may execute a page is settled when the
  // page is translated, so a fragment built for one mode must not answer a
  // lookup made in another. Nothing builds fragments today, which is the only
  // reason this is a comment rather than a field.
  inline TraceFragment *trace_lookup(uint64_t virt_pc, uint32_t asn) {
    TraceFragment &t = m_traces[trace_index_of(virt_pc)];
    return (t.valid && t.head_tag == virt_pc && (t.asm_global || t.asn == asn))
               ? &t
               : nullptr;
  }

  // Source-coherence check (review: per-segment, from M0). head_live_phys is
  // the head's freshly resolved physical; on a remap/flush since build, fall
  // back to blocks + re-form. See the .cpp.
  bool trace_ok(TraceFragment *t, uint64_t head_live_phys, const uint8_t *dram);

  // Lazy-flush survivor: hash-revalidate the slot in place (no interpreted
  // pass, no re-record).
  JitBlock *revalidate_flushed(uint64_t virt_pc, uint32_t asn, uint8_t cm,
                               uint64_t phys_pc, const uint8_t *dram);

  JitBlock *record(uint64_t virt_pc, uint64_t phys_pc, uint32_t asn, uint8_t cm,
                   bool asm_global, uint32_t n_instr, const uint8_t *dram);
  void compile_block(JitBlock *b, const uint8_t *dram, uint64_t dram_size,
                     void *read_helper, void *write_helper, void *opcdec_helper,
                     void *hw_mfpr_helper, void *hw_ld_helper,
                     void *hw_mtpr_helper, void *hw_st_helper,
                     void *indirect_helper, void *read_locked_helper,
                     void *stc_helper, void *misc_helper,
                     void *read_vpte_helper, void *read_wchk_helper,
                     void *itof_helper, void *ftoi_helper, void *fltl_helper,
                     void *fp_read_helper, void *fp_write_helper,
                     void *fltv_helper);
  void flush();

  // Per-op codegen, shared by compile_block and compile_trace
  // Block register allocator: maps each guest GPR to a host reg id, or -1 =
  // the state.r[] memory slot. The global pins (x86-64: R26/R16/R27/R30 ->
  // r12/r13/r15/r14; AArch64: see jitemit_a64.hpp) are the static binding, live
  // across the chain; dynamic block-local pool next. host_of(r) drives
  // emit_op's operand routing either way.
  struct RegAlloc {
    int host[32];  // host reg id for guest GPR r, or -1 (memory)
    int rax_holds; // guest GPR whose value currently lives in the result reg
                   // (rax / x0) (value-forward), or -1
    int host_of(int r) const { return host[r]; }
  };

  // Host-architecture backend (x86-64: jitengine.cpp; AArch64:
  // jitemit_a64.hpp). compile_block / compile_trace do the arch-neutral work
  // (prefix scan, stats, slot bookkeeping) and call these to emit + add the
  // native code. Return false when nothing was produced.
  bool assemble_block(JitBlock *b, const uint32_t *words, uint32_t plen,
                      bool terminator_branch, bool terminator_jmp,
                      const HelperSet &hs, JitFn *fn, uint32_t *body_off,
                      size_t *csz);
  bool assemble_trace(JitBlock **blocks, uint32_t n_blocks, const uint8_t *dram,
                      const HelperSet &hs, JitFn *fn, size_t *csz);

  void emit_op(void *a, const uint8_t *gpa, void *done, const HelperSet &hs,
               bool pal_block, JitBlock *b, uint32_t ins, uint32_t i,
               RegAlloc &regalloc);

  // compile an N-block trace into slot t (reuses emit_op per block; blocks
  // fused with a guard -> side-exit between them). n_blocks==1 is the
  // single-block case; the exit returns to the dispatcher.
  void compile_trace(TraceFragment *t, JitBlock **blocks, uint32_t n_blocks,
                     const uint8_t *dram, uint64_t dram_size,
                     const HelperSet &hs);

  void flush_non_global(); // flush only !asm_global blocks (the ASM-bit-clear /
                           // ASN icache flush)
  void unlink_all();       // drop every direct static link (flush(): the code
                           // bytes may have changed under all of them)
  void reclaim_code();     // free ALL compiled code once past kReclaimBytes
                           // (cold-path only)
  // flush() can be reached from a compiled IC_FLUSH, so it DEFERS the reclaim
  // (sets m_reclaim_pending); the dispatcher calls this at a safe point (no
  // compiled frame live) to actually free the code.
  inline void reclaim_if_pending() {
    if (m_reclaim_pending) {
      m_reclaim_pending = false;
      reclaim_code();
    }
  }

  // Why the validation epoch moved. Every bump rejects every cached link, so
  // before replacing the epoch with a reverse index we need to know which
  // event actually does the rejecting -- the fixes differ per cause (an ASN
  // switch invalidates nothing about the code; an IMB invalidates the bytes).
  enum EpochCause {
    EPOCH_ASN,     // address-space switch (swpctx / ITB_ASN write)
    EPOCH_TBIS,    // single-page I-stream invalidate
    EPOCH_TBIA,    // whole ITB cleared
    EPOCH_TBIAP,   // process-specific ITB entries cleared
    EPOCH_REMAP,   // a code page re-translated in place (ITB fill over a
                   // different physical)
    EPOCH_FLUSH,   // icache flush, caller not separated out
    EPOCH_IMB,     // guest IMB (instruction memory barrier)
    EPOCH_ICFLUSH, // guest HW_MTPR IC_FLUSH
    EPOCH_PALRST,  // execution reached the PAL reset vector
    EPOCH_FNG,     // ASM-bit-clear icache flush (non-global bodies dropped)
    EPOCH_RECLAIM, // the code arena was freed
    EPOCH_IDLE,    // idle/park head learned (links into it are refused)
    EPOCH_OTHER,
    EPOCH_CAUSES
  };
  static const char *epoch_cause_name(int c) {
    static const char *const n[EPOCH_CAUSES] = {
        "asn",      "tbis",   "tbia", "tbiap",   "remap", "flush", "imb",
        "ic_flush", "palrst", "fng",  "reclaim", "idle",  "other"};
    return (c >= 0 && c < EPOCH_CAUSES) ? n[c] : "?";
  }

  // ITB-generation counter for the indirect-chain staleness check
  // (jit_indirect). Bumped on every I-stream TB invalidate (tbia/tbiap/tbis,
  // ACCESS_EXEC) ... those can remap a code page WITHOUT flushing the JIT, so a
  // chained block could run stale bytes.
  // Why the inline data-page-cache probe missed and had to call the helper.
  // The probe compares one packed tag, so every one of these looks the same
  // from compiled code -- and they want different fixes: OTHER_PAGE is a size
  // or indexing problem, MODE and ASN are key problems, MMIO is by design.
  enum DpcMiss {
    DM_EMPTY,      // slot never filled (cold, or invalidated since)
    DM_OTHER_PAGE, // another page occupies the slot
    DM_MODE,       // same page, cached for a different processor mode
    DM_ASN,        // same page, cached for a different address space
    DM_MMIO,       // a page compiled code may not touch inline
    DM_HIT,        // the C++ path accepts it: the two guards disagree
    DM_CAUSES
  };
  static const char *dpc_miss_name(int c) {
    static const char *const n[DM_CAUSES] = {
        "empty", "other-page", "mode", "asn", "mmio", "probe-only"};
    return (c >= 0 && c < DM_CAUSES) ? n[c] : "?";
  }
  void set_flush_cause(int c) { m_flush_cause = c; }
  inline void note_itb_invalidate(EpochCause cause = EPOCH_OTHER) {
    ++m_itb_gen;
    ++m_epoch;
    note_epoch(cause);
  }
  // Record a validated computed-jump target for the inline cache (see
  // m_ind_cache). Non-global blocks are safe too: the entry comes from a
  // lookup under the current ASN, and an ASN change bumps the epoch
  // (jit_note_asn_change), so it can't be hit from another address space.
  // Superpage kernel code is never ASM, so excluding it would gut the cache.
  inline void ind_cache_fill(uint64_t target, JitBlock *b) {
    if (!b->jit_body || (target & 1))
      return;
    IndCacheEntry &e = m_ind_cache[(target >> 2) & ((1u << kIndBits) - 1)];
    e.tag = target;
    e.vgen = b->vgen;
    e.body = b->jit_body;
  }
  inline uint64_t vgen() const {
    return m_epoch; // == m_itb_gen + m_flush_gen
  } // combined validation epoch

  // Bail-cause counters (JIT_STATS): why a compiled chain returned to the
  // dispatcher -- a branch/ fall-through cached-link miss vs a computed-jump
  // (jit_indirect) miss. Empty when stats are off, so the call sites need no
  // #ifdef.
#ifdef JIT_STATS
  // Why a cached link missed: the successor was already in a slot and merely
  // went stale (the epoch moved, e.g. an address-space switch), or it was a
  // target this exit had not cached. The first is an invalidation problem,
  // the second a prediction problem, and they want opposite fixes.
  void note_link_stale(bool stale) {
    if (stale)
      m_link_stale++, m_stale_by_cause[m_last_epoch_cause]++;
    else
      m_link_fresh++;
  }
  // The same question for a static exit's data link (a64 emit_static_exit):
  // the slot already held a body, so the epoch compare -- not the absence of a
  // prediction -- is what sent us back to the dispatcher.
  void note_dlink_stale(bool stale) {
    if (stale)
      m_dlink_stale++, m_stale_by_cause[m_last_epoch_cause]++;
    else
      m_dlink_fresh++;
  }
  void note_epoch(int cause) {
    m_last_epoch_cause = cause;
    m_epoch_bumps[cause]++;
  }
  void note_dpc_miss(int cause) { m_dpc_miss[cause]++; }
  void note_helper_tsc(int kind, uint64_t cycles) { m_helper_tsc[kind] += cycles; }
  void note_link_bail() { m_bail_link++; }
  void note_jmp_attempt() { m_jmp_attempt++; }
  void note_jmp_hit() { m_jmp_hit++; }
#else
  void note_link_stale(bool) {}
  void note_dlink_stale(bool) {}
  void note_epoch(int) {}
  void note_dpc_miss(int) {}
  void note_helper_tsc(int, uint64_t) {}
  void note_link_bail() {}
  void note_jmp_attempt() {}
  void note_jmp_hit() {}
#endif

#ifdef JIT_VERIFY
  // Differential check: compiled result (jit) vs interpreter result (interp),
  // r[0..30]. Returns the ns spent in its periodic progress printf (0
  // otherwise) so the dispatcher can exclude that stall from the
  // wall-clock-pinned RPCC (same Heisenberg fix as note_exec).
  uint64_t verify_compare(uint64_t blk_virt, const uint64_t *interp,
                          const uint64_t *jit, const uint32_t *words,
                          uint32_t nwords);
  void trace_selftest(); // M0: unit-test trace_ok's source-coherence
                         // (SMC/IMB/ITB-remap/head-remap)
#endif

#ifdef JIT_STATS
  // Accumulate native vs interpreted instruction counts; prints coverage
  // periodically. Returns the wall-clock ns spent in this call's stats-print
  // I/O (0 when it doesn't report), so the dispatcher can exclude that stall
  // from the wall-clock-pinned RPCC.
  uint64_t note_exec(uint32_t native_instr, uint32_t interp_instr,
                     uint64_t comp_tsc = 0, uint64_t interp_tsc = 0);
#endif

#ifdef JIT_REGPROF
  // Pin-selection profiler: per-block executions (rp_hits) x the block's
  // GPR-access mask (rp_mask), summed over the live cache -> an
  // execution-weighted histogram of which Alpha GPRs dominate the hot path.
  // Prints the top registers periodically so we can choose the global pin set.
  void regprof_report();
#endif

private:
  JitBlock m_blocks[kCacheEntries];
  TraceFragment
      m_traces[kTraceEntries]; // M0+: the trace tier's cache (inert until M1)
  bool m_traces_enabled =
      false; // global kill-switch; default OFF -> bit-identical
  int m_cpu_id;
  uint64_t m_amask = 0; ///< set by set_cpu_identity() before any compile
  uint64_t m_implver = 0;
  uint64_t m_recorded;
  uint64_t m_itb_gen =
      0; // current ITB generation (bumped on every I-stream TB invalidate)
  uint64_t m_flush_gen = 0; // current icache-flush generation (bumped by
                            // flush(); lazy IC_FLUSH/IMB)
  // Which caller's icache flush we are in, so the epoch census can separate
  // an IMB from an IC_FLUSH from a PAL restart. Set by flush_icache().
  int m_flush_cause = EPOCH_FLUSH;
  uint64_t m_direct_live = 0; // direct links currently established; lets
                              // unlink_all() cost nothing when there are none
  const bool m_direct_links = [] {
    const char *e = getenv("ALPHABOX_JIT_DLINK");
    return e && e[0] == '1';
  }();
  uint64_t m_epoch = 0; // m_itb_gen + m_flush_gen, kept in step with both so
                        // compiled chain guards load one word
  // Inline computed-jump cache (a64 emitter): target PC -> chained body of the
  // block validated for it, valid while its epoch (m_itb_gen + m_flush_gen at
  // validation, kept in vgen) is current; any ITB invalidate, ASN change or
  // flush bumps the epoch. Declared right after the epoch counters so compiled
  // code reaches all three through one base register. Filled by jit_indirect,
  // cleared by reclaim_code (it frees the bodies).
  struct IndCacheEntry {
    uint64_t tag, vgen;
    void *body;
    uint64_t pad; // 32-byte entries: index << 5
  };
  static constexpr int kIndBits = 10;
  IndCacheEntry m_ind_cache[1 << kIndBits] = {};
  uint64_t m_code_bytes;    // compiled bytes since last reclaim (see flush())
  bool m_reclaim_pending =
      false; // flush() hit kReclaimBytes; reclaim at the next dispatch boundary
  void *m_rt;            // asmjit::JitRuntime*
  JitOffsets m_off = {}; // field offsets for the inline load fast path
  // a64 hot/cold split: memory ops record the asmjit label ids of their
  // out-of-line slow path (indexed m_cold_base + instruction index), and
  // assemble_block / assemble_trace re-run emit_op with m_cold_pass set to
  // emit those paths after the epilogue, off the fall-through hot path.
  // a64 shared helper-call thunk (spill caller-saved pins, blr x16, reload),
  // built lazily in the current code runtime; reclaim_code drops it.
  void *m_call_thunk = nullptr;
  void *a64_call_thunk();
  // a64 static-exit data links. Each compiled block owns one ExitRec (its
  // address is baked into that block's code); the dispatcher caches the
  // successor's body and the epoch it was validated in. The record belongs to
  // the code, not to the reusable JitBlock slot: a slot re-recorded for another
  // PC leaves the old code (still reachable within the epoch) reading its own
  // links. Freed with the code on reclaim.
public: // the dispatcher fills these (AlphaCPU.cpp)
  struct ExitRec {
    void *body[kLinkSlots];
    uint64_t epoch[kLinkSlots]; // ~0 = empty (never a live epoch). Read only
                                // by a cross-page slot; a direct slot ignores
                                // it.
    // Reverse index, for direct (epoch-free) slots only: the next static exit
    // linked into the same target block, as ExitRec* | slot. 0 = end of list.
    uintptr_t in_next[kLinkSlots];
    uint8_t direct_mask; // bit k: slot k is a DIRECT link -- its target is in
                         // the source block's own guest page, so no MMU change
                         // can invalidate it without invalidating the source
                         // too, and the only guard left is body != null.
  };

  // Link a direct static exit to a block, and record it on that block's
  // inbound list so the block's invalidation can undo it. The caller has
  // already established that b is this exit's target and is compiled.
  inline void link_direct(ExitRec *xr, unsigned slot, JitBlock *b) {
    if (xr->body[slot])
      return; // already linked, hence already on b's list
    xr->body[slot] = b->jit_body;
    xr->in_next[slot] = b->inbound;
    b->inbound = (uintptr_t)xr | slot;
    m_direct_live++;
  }

  // Drop every direct link INTO b (QEMU's tb_jmp_unlink). Writes data only --
  // no code patching, so no icache maintenance and no W^X toggle -- and is
  // safe to call while b's own code is on the stack: the exits it clears will
  // simply miss to the dispatcher next time.
  inline void unlink_inbound(JitBlock *b) {
    uintptr_t p = b->inbound;
    b->inbound = 0;
    while (p) {
      ExitRec *xr = (ExitRec *)(p & ~(uintptr_t)7);
      const unsigned k = (unsigned)(p & 7);
      p = xr->in_next[k];
      xr->in_next[k] = 0;
      xr->body[k] = nullptr;
      m_direct_live--;
    }
  }

  // ALPHABOX_JIT_DLINK=1 compiles the epoch-free static exit for same-page
  // targets. OFF by default: measurement says a cached link misses ~1000 times
  // per 100M instructions on CPU-bound guest code, so there is nothing here to
  // win, and an icache flush -- which the guest's PALcode issues once per ~1000
  // instructions during firmware -- has to walk the cache to undo the links.
  // See docs/performance.md.
  bool direct_links_enabled() const { return m_direct_links; }

private:
  static constexpr size_t kExitChunk = 1u << 16;
  std::vector<std::unique_ptr<ExitRec[]>> m_exit_chunks;
  size_t m_exit_used = kExitChunk;
  ExitRec *alloc_exit_rec() {
    if (m_exit_used == kExitChunk) {
      m_exit_chunks.emplace_back(new ExitRec[kExitChunk]);
      m_exit_used = 0;
    }
    ExitRec *r = &m_exit_chunks.back()[m_exit_used++];
    for (int i = 0; i < kLinkSlots; ++i) {
      r->body[i] = nullptr;
      r->epoch[i] = ~(uint64_t)0;
      r->in_next[i] = 0;
    }
    r->direct_mask = 0;
    return r;
  }
  // AArch64 conditional-branch exit. A block ending in a PC-relative
  // conditional branch used to materialise BOTH successor PCs, csel between
  // them, store the winner to state.pc, and then compare that PC back against
  // the taken target to discover which way the branch went. Instead, emit_op
  // emits NOTHING for the terminator and records its opcode and register
  // here; the epilogue tests the register itself, right before it branches.
  // Six of the eight forms then need no flags at all (cbz/cbnz, tbz/tbnz on
  // bit 63 or bit 0), and the two that do (BLE/BGT) keep their cmp adjacent
  // to the b.cond -- so nothing has to survive across the count update, and a
  // flag-setting instruction there later (a subs down-counter, say) cannot
  // silently break it. Only assemble_block's main pass opts in -- the cold
  // pass and the trace builder still need the PC written where they stand.
  int m_pending_br_op = -1;
  int m_pending_br_ra = 31;
  bool m_defer_branch_pc = false;

  static constexpr uint32_t kColdMax = 1024;
  uint32_t m_cold_slow[kColdMax];
  uint32_t m_cold_back[kColdMax];
  bool m_cold_used[kColdMax] = {};
  uint32_t m_cold_base = 0;
  bool m_cold_pass = false;
  // Interpreted passes before a block compiles (ALPHABOX_JIT_COMPILE_AFTER
  // overrides). Measured on a Windows 2000 guest with interleaved boots (one
  // run per boot, arms alternating), 16 beat 2 on both workloads with no
  // overlap -- about 2% on a tight arithmetic loop and 27% on an I/O-heavy
  // one -- and reached the desktop 2s sooner.
  //
  // What JIT_STATS says is happening, which is not what one might assume:
  // compiling eagerly does not fill the code cache, it churns it. At 2 the
  // cache held 19.8 MB with 12 reclaims and 101810 recompiles (fresh-cause
  // asn 61703, cold 28467) -- blocks compiled, discarded and compiled again.
  // At 16 it held 313.8 MB with zero reclaims: blocks that survive to the
  // threshold are hot enough to stay resident. The extra interpretation this
  // costs (nothot: 2.08M instructions over 227658 entries) is smaller than
  // the churn it avoids.
  //
  // 16 is better than 2; it is not proven optimal. The curve must turn
  // somewhere above it, because never compiling is pure interpretation.
#ifdef JIT_VERIFY
  // Verify builds want differential coverage rather than speed: at 16 far
  // fewer blocks ever compile, so far less of the JIT is checked against the
  // interpreter.
  uint32_t m_compile_after = 1;
#else
  uint32_t m_compile_after = 16;
#endif
#ifdef JIT_DISASM
  FILE *m_disasm_fp =
      nullptr; // per-CPU disassembly trace file (jit_disasm_cpuN.txt)
#endif
#ifdef JIT_VERIFY
  uint64_t m_v_exec, m_v_fail;
#endif
#ifdef JIT_STATS
  uint64_t m_stat_native,
      m_stat_interp; // windowed: instrs run native vs interpreted
  uint64_t m_stat_hot,
      m_stat_miss; // windowed: compiled-chain dispatches, interp blocks
  uint64_t m_stat_compiled,
      m_stat_plen_sum; // cumulative: compiled blocks, sum of their lengths
  uint64_t m_stat_code_bytes; // cumulative: emitted x86 bytes (code expansion =
                              // /plen_sum)
  uint64_t m_stat_reclaims; // cumulative: code-cache reclaims (all code freed)
  uint64_t m_cold_n[CR_COUNT];     // windowed: cold-path entries by reason
  uint64_t m_cold_instr[CR_COUNT]; // windowed: instructions interpreted there
  uint64_t m_cold_op[64];  // windowed: interp instrs of uncompilable blocks by
                           // their first opcode
  uint64_t m_done0_op[64]; // windowed: zero-progress compiled runs by first op
  uint64_t m_bail_kind[2]
                      [BK_COUNT]; // windowed: helper bails [read/write][kind]
  static constexpr int kHotPcBits = 12;
  struct HotPc {
    uint64_t pc, phys, n;
  };
  HotPc m_hot_pc[1 << kHotPcBits]; // 5-window: hot chain-entry PCs
  const uint8_t *m_hot_dram = nullptr;
  uint64_t m_hot_win = 0;
  uint64_t m_helper_n[HK_COUNT];             // windowed: helper entries
  uint64_t m_helper_tsc[HK_COUNT];           // windowed: host cycles inside
                                             // each helper (entry to return,
                                             // callees included)
  uint64_t m_mtpr_rt[256]; // windowed: jit_hw_mtpr calls by IPR function
  const uint64_t *m_dpc_flush_src = nullptr; // CPU's data-page-cache flushes
  uint64_t m_dpc_flush_last = 0;             // ...at the previous window
  uint64_t m_stat_wall_last_ns; // steady_clock ns at the last window report
                                // (throughput delta)
  uint64_t m_tsc_compiled,
      m_tsc_interp; // windowed: host TSC cycles in b->code() vs interp fallback
  uint64_t m_tsc_window_start; // host TSC at window start (the time-split
                               // denominator)
  uint64_t m_link_stale = 0, m_link_fresh = 0; // link misses by cause
  uint64_t m_dlink_stale = 0, m_dlink_fresh = 0; // ...for static-exit data
                                                 // links (a64)
  uint64_t m_epoch_bumps[EPOCH_CAUSES] = {};     // cumulative: epoch bumps
  uint64_t m_stale_by_cause[EPOCH_CAUSES] = {};  // ...and the stale link misses
                                                 // charged to the last one
  int m_last_epoch_cause = EPOCH_OTHER;
  uint64_t m_dpc_miss[DM_CAUSES] = {}; // windowed: inline page-cache probe
                                       // misses by cause
  uint64_t m_bail_link, m_jmp_attempt,
      m_jmp_hit; // windowed: link-miss bails, jit_indirect attempts/hits
  uint64_t m_fresh_cold, m_fresh_tag, m_fresh_asn, m_fresh_phys,
      m_fresh_hash; // windowed: record() step-4 fresh-compile reason
  uint64_t m_trace_formed, m_trace_entered, m_trace_exits,
      m_trace_stale; // windowed: trace tier activity (M1+)
  uint64_t
      m_term_op[64]; // cumulative: opcode that ended a block's compiled prefix
  uint64_t
      m_pal_func[256]; // cumulative: CALL_PAL function code that ended a block
  uint64_t m_mtpr_func[256]; // cumulative: HW_MTPR (0x1d) IPR index that ended
                             // a block
  uint64_t m_hwld_func[16];  // cumulative: HW_LD (0x1b) form (ins>>12 & 0xf)
                             // that ended a block
  uint64_t m_misc_func[16];  // cumulative: MISC (0x18) Ra==31 form (ins>>12 &
                             // 0xf: RPCC/RC/RS) that ended a block
  bool m_first_breaker_logged; // one-shot guard for the punch-list print
#endif
};

#endif // ES40_JIT
#endif // INCLUDED_JITENGINE_H
