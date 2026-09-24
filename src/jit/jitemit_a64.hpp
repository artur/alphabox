/* Alphabox Alpha Emulator -- JIT engine, AArch64 backend.
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
 * Included from jitengine.cpp (JIT_HOST_A64) after the arch-neutral engine: it
 * shares classify()/SafeOp, g_zapnot_mask and the JitOffsets/HelperSet
 * contract, and provides emit_op / assemble_block / assemble_trace on asmjit's
 * a64 backend. It mirrors the x86-64 emitter op for op -- same bail protocol
 * (state.pc = faulting instruction, return the chain's completed count), same
 * chaining (shared frame, poly-link, jit_indirect), same verify behaviour --
 * so the dispatcher, the helpers and the JIT_VERIFY harness are unchanged.
 *
 * Host register roles (AAPCS64; x18 is the platform register, never touched):
 *   x19      cpu (CAlphaCPU*)          x20      regs (guest GPR file)
 *   x21-x26  global pins: R26 R16 R27 R30 R29 R0 (callee-saved, chain-live)
 *   x0       op1 / result (x86: rax)   x1       op2 (x86: rcx)
 *   x2       effective address (rdx)   x9       next PC (r10)
 *   x10-x12  scratch                   x16      helper call target
 *   x17      wide CAlphaCPU field offsets (see a64_cpu_field)
 * Frame, shared by every block of a chain (chained entry skips the prologue):
 *   [sp+0] x29/x30  [sp+16..79] x19-x26  [sp+80] helper out slot
 *   [sp+88] chain instruction count
 */
#if !defined(INCLUDED_JITEMIT_A64_H)
#define INCLUDED_JITEMIT_A64_H

#include <atomic>

namespace {

constexpr uint32_t kA64FrameSize = 112;
constexpr int32_t kA64SavedX27 = 80; // x27/x28 pair (see a64_prologue)
constexpr int32_t kA64OutSlot = 96;  // helper out-parameter

// Global pins: guest GPR -> callee-saved host register id. kGlobalPins (the
// x86 hot set RA/a0/PV) plus SP, GP and v0 -- AAPCS64 has callee-saved
// registers to spare, so this is the Win64 x86 pin set on every a64 host.
constexpr struct {
  int guest;
  uint32_t host;
} kA64Pins[] = {{26, 21}, {16, 22}, {27, 23}, {30, 24}, {29, 25}, {0, 26}};

// Caller-saved pins: more hot guest GPRs in host registers the emitter never
// uses as scratch (x4-x8, x13-x15; x18 is platform-reserved). Helper calls
// clobber them, so both call sites store them to their guest slots first and
// reload them after (a64_spill_pins / a64_reload_pins); the prologue and
// epilogue sync them like the callee-saved pins. No helper reads state.r
// directly, so the guest slots only need to be current across the call.
// Guests exclude R4-7 / R20-23 (PALshadow remap). Chosen from a JIT_REGPROF
// Windows 2000 setup profile: the hottest pin-eligible GPRs not already pinned
// (t0-t2 R1-R3, a1-a2 R17-R18, s0-s2 R9-R11).
constexpr struct {
  int guest;
  uint32_t host;
} kA64CallerPins[] = {{1, 4},  {9, 5},   {17, 6}, {2, 7},
                      {10, 8}, {18, 13}, {3, 14}, {11, 15}};

// Count the low contiguous set bits of a mask, to prove it is the
// ((1 << n) - 1) form UBFX can express.
static inline uint32_t a64_popcount_low(uint32_t m) {
  uint32_t n = 0;
  while (m & 1u) {
    ++n;
    m >>= 1;
  }
  return n;
}

// Emit failures (an operand combination a64 can't encode) must not ship a
// silently truncated block: record them and let assemble_* discard the code.
class A64EmitErrors : public asmjit::ErrorHandler {
public:
  bool failed = false;
  int cpu_id = -1;
  void handle_error(asmjit::Error err, const char *message,
                    asmjit::BaseEmitter *) override {
    (void)err;
    static std::atomic<int> reported{0};
    if (!failed && reported.fetch_add(1) < 20)
      fprintf(stderr, "[JIT][CPU%d][A64-EMIT-ERROR] %s\n", cpu_id, message);
    failed = true;
  }
};

inline asmjit::Imm a64_cc(asmjit::a64::CondCode c) {
  return asmjit::Imm((uint32_t)c);
}

// [cpu + off] for a (1 << lg)-byte load/store. CAlphaCPU is large, so many
// fields sit beyond the scaled imm12 range; those use x17 as the offset.
asmjit::a64::Mem a64_cpu_field(asmjit::a64::Assembler &a, uint32_t off,
                               unsigned lg) {
  using namespace asmjit;
  if ((off & ((1u << lg) - 1)) == 0 && (off >> lg) <= 4095)
    return a64::ptr(a64::x19, (int32_t)off);
  a.mov(a64::x17, imm(off));
  return a64::ptr(a64::x19, a64::x17);
}

// dst = src + d for any 64-bit d (imm12 when it fits, else through x17).
void a64_add_imm(asmjit::a64::Assembler &a, const asmjit::a64::Gp &dst,
                 const asmjit::a64::Gp &src, int64_t d) {
  using namespace asmjit;
  if (d >= 0 && d < 4096)
    a.add(dst, src, imm(d));
  else if (d < 0 && d > -4096)
    a.sub(dst, src, imm(-d));
  else {
    a.mov(a64::x17, imm(d));
    a.add(dst, src, a64::x17);
  }
}

// Chain-lifetime registers (callee-saved, live across the whole chain):
//   x27 = instructions completed so far (was a stack slot: one add per block
//         instead of load/add/store, and bails compute x0 = x27 + n),
//   x28 = &engine epoch (m_itb_gen, m_flush_gen beside it) for the link guard.
void a64_prologue(asmjit::a64::Assembler &a, uint64_t epoch_base) {
  using namespace asmjit;
  a.sub(a64::sp, a64::sp, imm(kA64FrameSize));
  a.stp(a64::x29, a64::x30, a64::ptr(a64::sp, 0));
  a.mov(a64::x29, a64::sp);
  a.stp(a64::x19, a64::x20, a64::ptr(a64::sp, 16));
  a.stp(a64::x21, a64::x22, a64::ptr(a64::sp, 32));
  a.stp(a64::x23, a64::x24, a64::ptr(a64::sp, 48));
  a.stp(a64::x25, a64::x26, a64::ptr(a64::sp, 64));
  a.stp(a64::x27, a64::x28, a64::ptr(a64::sp, kA64SavedX27));
  a.mov(a64::x19, a64::x0); // cpu  (arg 0)
  a.mov(a64::x20, a64::x1); // regs (arg 1)
  a.mov(a64::x27, imm(0));  // chain count := 0
  a.mov(a64::x28, imm(epoch_base));
  // Load the pins on cold entry; chained re-entry skips this and they stay
  // live across the chain, synced back in a64_epilogue.
  for (const auto &p : kA64Pins)
    a.ldr(a64::x(p.host), a64::ptr(a64::x20, p.guest * 8));
  for (const auto &p : kA64CallerPins)
    a.ldr(a64::x(p.host), a64::ptr(a64::x20, p.guest * 8));
}

// Shared exit: x0 = instructions completed across the chain (the JitFn
// result), state.pc already written by the exit path.
void a64_epilogue(asmjit::a64::Assembler &a) {
  using namespace asmjit;
  for (const auto &p : kA64Pins)
    a.str(a64::x(p.host), a64::ptr(a64::x20, p.guest * 8));
  for (const auto &p : kA64CallerPins)
    a.str(a64::x(p.host), a64::ptr(a64::x20, p.guest * 8));
  a.ldp(a64::x27, a64::x28, a64::ptr(a64::sp, kA64SavedX27));
  a.ldp(a64::x25, a64::x26, a64::ptr(a64::sp, 64));
  a.ldp(a64::x23, a64::x24, a64::ptr(a64::sp, 48));
  a.ldp(a64::x21, a64::x22, a64::ptr(a64::sp, 32));
  a.ldp(a64::x19, a64::x20, a64::ptr(a64::sp, 16));
  a.ldp(a64::x29, a64::x30, a64::ptr(a64::sp, 0));
  a.add(a64::sp, a64::sp, imm(kA64FrameSize));
  a.ret(a64::x30);
}

void a64_regalloc(CJitEngine::RegAlloc &ra) {
  for (int r = 0; r < 32; ++r)
    ra.host[r] = -1;
  ra.rax_holds = -1;
  for (const auto &p : kA64Pins)
    ra.host[p.guest] = (int)p.host;
  for (const auto &p : kA64CallerPins)
    ra.host[p.guest] = (int)p.host;
}

// Around a helper call: the caller-saved pins go to their guest slots before
// the blr and come back after it (the result stays in x0).
void a64_spill_pins(asmjit::a64::Assembler &a) {
  for (const auto &p : kA64CallerPins)
    a.str(asmjit::a64::x(p.host),
          asmjit::a64::ptr(asmjit::a64::x20, p.guest * 8));
}
void a64_reload_pins(asmjit::a64::Assembler &a) {
  for (const auto &p : kA64CallerPins)
    a.ldr(asmjit::a64::x(p.host),
          asmjit::a64::ptr(asmjit::a64::x20, p.guest * 8));
}

// Chain gate: branch to lbl when the chain hit the budget ceiling or an
// interrupt/timer is pending. Clobbers x1 (and x17).
void a64_emit_gate(asmjit::a64::Assembler &a, const CJitEngine::JitOffsets &off,
                   const asmjit::Label &lbl) {
  using namespace asmjit;
  a.ldr(a64::x1, a64_cpu_field(a, off.jit_budget, 3));
  a.cmp(a64::x27, a64::x1);
  a.b_ge(lbl);
  if (off.check_timers == off.check_int + 1) {
    // The two adjacent flag bytes in one load: nonzero if either is set.
    a.ldrh(a64::w1, a64_cpu_field(a, off.check_int, 1));
    a.cbnz(a64::w1, lbl);
  } else {
    a.ldrb(a64::w1, a64_cpu_field(a, off.check_int, 0));
    a.cbnz(a64::w1, lbl);
    a.ldrb(a64::w1, a64_cpu_field(a, off.check_timers, 0));
    a.cbnz(a64::w1, lbl);
  }
}

void a64_count_add(asmjit::a64::Assembler &a, uint32_t n) {
  a64_add_imm(a, asmjit::a64::x27, asmjit::a64::x27, n);
}

} // namespace

// Shared helper-call thunk, one per code runtime: store the caller-saved pins
// to their guest slots, call x16, reload them. Call sites load x16 and blr
// here instead of carrying 16 spill/reload instructions each. Arguments are
// already in x0-x3 and the result comes back in x0; sp stays 16-aligned.
void *CJitEngine::a64_call_thunk() {
  if (m_call_thunk)
    return m_call_thunk;
  using namespace asmjit;
  CodeHolder code;
  if (code.init(((JitRuntime *)m_rt)->environment()) != Error::kOk)
    return nullptr;
  A64EmitErrors eh;
  eh.cpu_id = m_cpu_id;
  code.set_error_handler(&eh);
  a64::Assembler a(&code);
  a.sub(a64::sp, a64::sp, imm(16));
  a.stp(a64::x29, a64::x30, a64::ptr(a64::sp, 0));
  a64_spill_pins(a);
  a.blr(a64::x16);
  a64_reload_pins(a);
  a.ldp(a64::x29, a64::x30, a64::ptr(a64::sp, 0));
  a.add(a64::sp, a64::sp, imm(16));
  a.ret(a64::x30);
  if (eh.failed)
    return nullptr;
  JitFn fn = nullptr;
  if (!publish_code(&code, (void **)&fn))
    return nullptr;
  m_call_thunk = (void *)fn;
  return m_call_thunk;
}

// The data page cache's second way (CAlphaCPU::data_page_cache2), shared
// by every memory op's cold stub, so the stubs stay a few instructions long
// -- carrying the probe and the swap in each one made the compiled code half
// as big again and cost the branch-heavy code more than the memory ops gained
// (docs/performance.md). Entered with blr: x2 = va, x9 = the offset of way
// 0's row (read or write). Returns Z set on a hit, with the two ways swapped
// -- the page just used moves to way 0, as the helpers' promotion does, or a
// page left in way 1 would take this detour on every access instead of once
// per switch -- and x10 = its bias; Z clear on a miss. Uses x0, x1, x3,
// x9-x12 and x17, which the helper call the stub otherwise makes clobbers too.
void *CJitEngine::a64_dpc2_thunk() {
  if (m_dpc2_thunk)
    return m_dpc2_thunk;
  if (!m_off.dpc_way1 || m_off.dpc_stride != 64 ||
      m_off.dpc_bias - m_off.dpc_tag != 8)
    return nullptr;
  const uint32_t idx_bits = a64_popcount_low(m_off.dpc_mask);
  if (!idx_bits || ((1u << idx_bits) - 1u) != m_off.dpc_mask)
    return nullptr;
  using namespace asmjit;
  CodeHolder code;
  if (code.init(((JitRuntime *)m_rt)->environment()) != Error::kOk)
    return nullptr;
  A64EmitErrors eh;
  eh.cpu_id = m_cpu_id;
  code.set_error_handler(&eh);
  a64::Assembler a(&code);
  Label out = a.new_label();
  a.ubfx(a64::x10, a64::x2, imm(13), imm(idx_bits));
  a.add(a64::x10, a64::x19, a64::x10, a64::lsl(6));
  a.add(a64::x3, a64::x10, a64::x9); // way 0's slot
  a.mov(a64::x9, imm((uint64_t)m_off.dpc_way1));
  a.add(a64::x9, a64::x3, a64::x9);      // way 1's slot
  a.ldr(a64::x12, a64::ptr(a64::x9, 0)); // way 1's tag
  a.and_(a64::x11, a64::x2, imm(~(uint64_t)0x1FFF));
  a.ldr(a64::x0, a64_cpu_field(a, m_off.dpc_key, 3));
  a.orr(a64::x11, a64::x11, a64::x0);
  a.cmp(a64::x12, a64::x11);
  a.b_ne(out);
  for (int32_t k = 0; k < 64; k += 16) { // loads and stores leave Z alone
    a.ldp(a64::x0, a64::x1, a64::ptr(a64::x3, k));
    a.ldp(a64::x11, a64::x12, a64::ptr(a64::x9, k));
    a.stp(a64::x11, a64::x12, a64::ptr(a64::x3, k));
    a.stp(a64::x0, a64::x1, a64::ptr(a64::x9, k));
  }
  a.ldr(a64::x10, a64::ptr(a64::x3, 8)); // the page's bias
  a.bind(out);
  a.ret(a64::x30);
  if (eh.failed)
    return nullptr;
  JitFn fn = nullptr;
  if (!publish_code(&code, (void **)&fn))
    return nullptr;
  m_dpc2_thunk = (void *)fn;
  return m_dpc2_thunk;
}

// RPCC, without leaving the compiled frame. Same arithmetic as
// CAlphaCPU::rpcc_read() -- wall-clock sync, then the forward-progress
// floor -- but written in scratch registers (x0-x3, x16, x17) only, so the
// eight pinned guest registers a C call would spill stay where they are.
// x19 holds the CPU. Result in x0.
void *CJitEngine::a64_rpcc_stub() {
  if (m_rpcc_stub)
    return m_rpcc_stub;
  using namespace asmjit;
  CodeHolder code;
  if (code.init(((JitRuntime *)m_rt)->environment()) != Error::kOk)
    return nullptr;
  A64EmitErrors eh;
  eh.cpu_id = m_cpu_id;
  code.set_error_handler(&eh);
  a64::Assembler a(&code);
  // x19 is the CPU. Every field of it is far beyond a load's immediate
  // range, so each access goes through a64_cpu_field, which parks the
  // offset in x17 -- nothing of ours may live there.
  const auto F = [&](uint32_t off, unsigned lg) {
    return a64_cpu_field(a, off, lg);
  };
  Label no_add = a.new_label(), no_borrow = a.new_label(),
        store = a.new_label();

  // --- sync_cc_wallclock: advance the counter by the real time elapsed ---
  uint32_t mrs_cntvct = 0xD53BE040; // mrs x0, cntvct_el0
  a.embed(&mrs_cntvct, 4);
  a.ldr(a64::x1, F(m_off.cc_last_sync, 3));
  a.cmp(a64::x1, a64::x0);
  a.csel(a64::x1, a64::x0, a64::x1, a64::CondCode::kHI); // never bill negative
  a.sub(a64::x2, a64::x0, a64::x1);                      // delta
  a.str(a64::x0, F(m_off.cc_last_sync, 3));
  a.ldrb(a64::w3, F(m_off.cc_ena, 0));
  a.cbz(a64::w3, no_add);
  a.ldr(a64::x3, F(m_off.cc_tick_hz, 3));
  a.cmp(a64::x2, a64::x3);
  a.csel(a64::x2, a64::x3, a64::x2, a64::CondCode::kHI); // cap at one second
  a.ldr(a64::x16, F(m_off.cc_q32, 3));
  a.mul(a64::x2, a64::x2, a64::x16);
  a.ldr(a64::x16, F(m_off.cc_remainder, 3));
  a.add(a64::x2, a64::x2, a64::x16);
  a.lsr(a64::x3, a64::x2, imm(32));             // whole cycles
  a.and_(a64::x2, a64::x2, imm(0xffffffffull)); // sub-cycle carry
  a.str(a64::x2, F(m_off.cc_remainder, 3));
  a.ldr(a64::x16, F(m_off.cc_borrow, 3));
  a.cbz(a64::x16, no_borrow);
  a.cmp(a64::x16, a64::x3);
  a.csel(a64::x1, a64::x16, a64::x3, a64::CondCode::kLO); // repay = min
  a.sub(a64::x3, a64::x3, a64::x1);
  a.sub(a64::x16, a64::x16, a64::x1);
  a.str(a64::x16, F(m_off.cc_borrow, 3));
  a.bind(no_borrow);
  a.ldr(a64::x16, F(m_off.state_cc, 3));
  a.add(a64::x16, a64::x16, a64::x3);
  a.str(a64::x16, F(m_off.state_cc, 3));
  a.bind(no_add);

  // --- forward progress: two reads never return the same value ---
  a.ldrb(a64::w3, F(m_off.cc_ena, 0));
  a.ldr(a64::x0, F(m_off.state_cc, 3));
  a.ldr(a64::x1, F(m_off.cc_last_read, 3));
  a.cbz(a64::w3, store);
  a.cmp(a64::x0, a64::x1);
  a.b(a64::CondCode::kHI, store);
  a.add(a64::x2, a64::x1, imm(1)); // the floor, plus one
  a.sub(a64::x3, a64::x2, a64::x0);
  a.ldr(a64::x16, F(m_off.cc_borrow, 3));
  a.add(a64::x16, a64::x16, a64::x3); // lend the difference
  a.str(a64::x16, F(m_off.cc_borrow, 3));
  a.mov(a64::x0, a64::x2);
  a.str(a64::x0, F(m_off.state_cc, 3));
  a.bind(store);
  a.str(a64::x0, F(m_off.cc_last_read, 3));
  a.ldr(a64::w1, F(m_off.cc_offset, 2));
  a.and_(a64::x0, a64::x0, imm(0xffffffffull));
  a.orr(a64::x0, a64::x0, a64::x1, a64::lsl(32));
  a.ret(a64::x30);

  if (eh.failed)
    return nullptr;
  JitFn fn = nullptr;
  if (!publish_code(&code, (void **)&fn))
    return nullptr;
  m_rpcc_stub = (void *)fn;
  return m_rpcc_stub;
}

void CJitEngine::emit_op(void *a_ptr, const uint8_t *gpa, void *done_ptr,
                         const HelperSet &hs, bool pal_block, JitBlock *b,
                         uint32_t ins, uint32_t i, RegAlloc &regalloc) {
  using namespace asmjit;
  using a64::CondCode;
  (void)gpa; // AAPCS64 argument order is fixed: x0..x7
  a64::Assembler &a = *(a64::Assembler *)a_ptr;
  Label &done = *(Label *)done_ptr;
  const a64::Gp x0 = a64::x0, x1 = a64::x1, x2 = a64::x2, x9 = a64::x9,
                x10 = a64::x10, x11 = a64::x11, x12 = a64::x12;
  const a64::Gp w0 = a64::w0, w1 = a64::w1, w11 = a64::w11, w12 = a64::w12;
  const a64::Gp kCpu = a64::x19, kRegs = a64::x20;
  const a64::Mem out_slot = a64::ptr(a64::sp, kA64OutSlot);

  auto fld = [&](uint32_t off, unsigned lg) {
    return a64_cpu_field(a, off, lg);
  };
  auto set_pc = [&](uint64_t pc_val) {
    a.mov(x9, imm(pc_val));
    a.str(x9, fld(m_off.state_pc, 3));
  };
  // Bail to the dispatcher: resume at this block's instruction n, having
  // completed n of its instructions (plus the chain's earlier blocks).
  auto bail = [&](uint32_t n) {
    set_pc(b->tag + 4 * (uint64_t)n);
    a64_add_imm(a, x0, a64::x27, n);
    a.b(done);
  };

  int ra = (ins >> 21) & 0x1F;
  int rb = (ins >> 16) & 0x1F;
  int rc = ins & 0x1F;
  bool islit = ((ins >> 12) & 1) != 0;
  uint32_t lit = (ins >> 13) & 0xFF;
  SafeOp op = classify(ins, pal_block);
  // Cold pass (hot/cold split): emit only the slow paths recorded in the hot
  // pass; every other instruction produces nothing.
  const uint32_t cold_idx = m_cold_base + i;
  if (m_cold_pass && (cold_idx >= kColdMax || !m_cold_used[cold_idx]))
    return;

  do {
    // MISC (0x18): prefetch and cache hints are nothing to us, but the
    // barriers are. A guest CPU is a host thread and compiled stores are
    // plain stores, so on a weakly ordered host the guest's own ordering is
    // only as good as what we emit for its MB and WMB. DMB ISH is the
    // instruction for it -- inner shareable covers every core a guest CPU
    // can be scheduled on -- and it matches what the interpreter does with
    // a seq_cst fence and what the x86 emitter does with mfence.
    //
    // (An earlier comment here claimed AArch64 had no equivalent of mfence
    // and emitted nothing at all. It does, and a guest releasing a lock or
    // publishing a descriptor ring entry depends on it.)
    if (op == OP_NOP)
      continue;
    if (op == OP_MFENCE) {
      a.dmb(asmjit::a64::Predicate::DB::kISH);
      continue;
    }

    // Value-forwarding: x0 may still hold the guest reg the previous op
    // computed (see the x86 emitter).
    const int prev_x0 = regalloc.rax_holds;
    regalloc.rax_holds = -1;

    auto reg = [&](int r) { // PALshadow remap (RREG), as in the x86 emitter
      int idx = (pal_block && ((r & 0xc) == 0x4)) ? r + 32 : r;
      return a64::ptr(kRegs, idx * 8);
    };
    auto mov_from_reg = [&](const a64::Gp &dst, int r) { // dst is 64-bit
      int p = regalloc.host_of(r);
      if (p >= 0)
        a.mov(dst, a64::x((uint32_t)p));
      else
        a.ldr(dst, reg(r));
    };
    auto mov_from_reg32 = [&](const a64::Gp &dst, int r) { // dst is 32-bit
      int p = regalloc.host_of(r);
      if (p >= 0)
        a.mov(dst, a64::w((uint32_t)p));
      else
        a.ldr(dst, reg(r)); // low dword (little-endian)
    };
    auto mov_to_reg = [&](int r, const a64::Gp &src) { // src is 64-bit
      int p = regalloc.host_of(r);
      if (p >= 0)
        a.mov(a64::x((uint32_t)p), src);
      else
        a.str(src, reg(r));
      if (src.id() == 0 && r != 31)
        regalloc.rax_holds = r; // x0 now mirrors r[r]; forward it
    };
    auto op1_x0 = [&]() {
      if (ra != 31 && prev_x0 == ra)
        return;
      if (ra == 31)
        a.mov(x0, imm(0));
      else
        mov_from_reg(x0, ra);
    };
    auto op2_x1 = [&]() { // operand2 (literal, or r[Rb] with r31=0)
      if (islit)
        a.mov(x1, imm(lit));
      else if (rb == 31)
        a.mov(x1, imm(0));
      else
        mov_from_reg(x1, rb);
    };
    // va = r[Rb] + disp -> x2
    auto ea_x2 = [&](int64_t disp) {
      if (rb == 31) {
        a.mov(x2, imm(disp));
        return;
      }
      // A pinned base folds the displacement straight into the ADD/SUB
      // instead of moving the pin to x2 and adding to it. One instruction
      // saved on every memory access whose base is pinned, which is most of
      // them: the pin set was chosen from a profile of real guest code.
      const int p = regalloc.host_of(rb);
      if (p >= 0 && disp != 0 && disp > -4096 && disp < 4096) {
        if (disp > 0)
          a.add(x2, a64::x((uint32_t)p), imm(disp));
        else
          a.sub(x2, a64::x((uint32_t)p), imm(-disp));
        return;
      }
      mov_from_reg(x2, rb);
      if (disp)
        a64_add_imm(a, x2, x2, disp);
    };

    // Three-address operands: read pinned guest registers in place and compute
    // straight into a pinned destination, instead of the x0/x1 shuttle (mov,
    // mov, op, mov -> one instruction). R31 operands still materialize zero in
    // the scratch register: in several a64 encodings register 31 means SP, not
    // XZR. Only a result computed in x0 is value-forwarded (mov_to_reg).
    auto src_reg = [&](int r, const a64::Gp &scratch, bool may_forward) {
      if (r == 31) {
        a.mov(scratch, imm(0));
        return scratch;
      }
      if (may_forward && prev_x0 == r)
        return x0;
      const int p = regalloc.host_of(r);
      if (p >= 0)
        return a64::x((uint32_t)p);
      a.ldr(scratch, reg(r));
      return scratch;
    };
    auto op1_src = [&]() { return src_reg(ra, x0, true); };
    auto op2_src = [&]() { return src_reg(rb, x1, false); };
    auto dst_reg = [&](int r) {
      const int p = (r == 31) ? -1 : regalloc.host_of(r);
      return p >= 0 ? a64::x((uint32_t)p) : x0;
    };
    auto rc_dst = [&]() { return dst_reg(rc); };
    auto dst_done = [&](int r, const a64::Gp &d) {
      if (d.id() == 0 && r != 31)
        mov_to_reg(r, x0); // spilled destination: store + forward
    };
    auto rc_done = [&](const a64::Gp &d) { dst_done(rc, d); };

    // ABI-native helper call; same argument kinds as the x86 emitter.
    // Register-sourced arguments are placed before immediates so a size
    // immediate for arg 2 can't clobber x2 while JA_VA still needs it.
    enum JitArgKind {
      JA_CPU,
      JA_GP,
      JA_GPZ,
      JA_VA,
      JA_OUT,
      JA_R10,
      JA_I32,
      JA_I64
    };
    struct JitArg {
      JitArgKind k;
      uint64_t v;
    };
    auto emit_call = [&](void *fn, std::initializer_list<JitArg> as) {
      auto place = [&](uint32_t k, const JitArg &s) {
        const a64::Gp xk = a64::x(k), wk = a64::w(k);
        switch (s.k) {
        case JA_CPU:
          a.mov(xk, kCpu);
          break;
        case JA_GP:
          mov_from_reg(xk, (int)s.v);
          break;
        case JA_GPZ:
          if (s.v == 31)
            a.mov(xk, imm(0));
          else
            mov_from_reg(xk, (int)s.v);
          break;
        case JA_VA:
          if (k != 2)
            a.mov(xk, x2);
          break;
        case JA_OUT:
          a.add(xk, a64::sp, imm(kA64OutSlot));
          break;
        case JA_R10:
          a.mov(xk, x9);
          break;
        case JA_I32:
          a.mov(wk, imm((uint32_t)s.v));
          break;
        case JA_I64:
          a.mov(xk, imm((uint64_t)s.v));
          break;
        }
      };
      uint32_t k = 0;
      for (const JitArg &s : as) {
        if (s.k != JA_I32 && s.k != JA_I64)
          place(k, s);
        ++k;
      }
      k = 0;
      for (const JitArg &s : as) {
        if (s.k == JA_I32 || s.k == JA_I64)
          place(k, s);
        ++k;
      }
      a.mov(a64::x16, imm((uint64_t)fn));
      if (void *thunk = a64_call_thunk()) {
        a.mov(a64::x17, imm((uint64_t)thunk)); // spill / call x16 / reload
        a.blr(a64::x17);
      } else {
        a64_spill_pins(a);
        a.blr(a64::x16);
        a64_reload_pins(a);
      }
    };
    // Hot pass: defer this instruction's slow path (entered at `slow`, resuming
    // at `back`) to the cold pass. False when the table is full -- the caller
    // then emits the slow path inline.
    auto cold_record = [&](const Label &slow, const Label &back) {
      if (cold_idx >= kColdMax)
        return false;
      m_cold_slow[cold_idx] = slow.id();
      m_cold_back[cold_idx] = back.id();
      m_cold_used[cold_idx] = true;
      return true;
    };
    // After a 0-ok / nonzero-bail int helper: bail at this instruction.
    auto bail_if_w0 = [&]() {
      Label ok = a.new_label();
      a.cbz(w0, ok);
      bail(i);
      a.bind(ok);
    };

#ifndef JIT_VERIFY
    // Inline data-page-cache probe (mirrors jit_read/jit_write's cache path):
    // x2 = va. On a hit: x10 = the page's bias, so the access itself is
    // ldr/str [x10, x2] -- no masking, no second base. A miss of any kind
    // branches to slow. Clobbers x9-x12.
    //
    // The slot holds what a hit needs in two adjacent words: a tag that is
    // the page, the address space and the mode together, and the bias. So
    // the question "is this page here, mine, and safe to touch inline?" is
    // one comparison, and a page the fast path must not touch (MMIO) simply
    // carries a tag no key can equal.
    const uint32_t dpc_bias_rel = m_off.dpc_bias - m_off.dpc_tag;
    auto dpc_probe = [&](bool write_row, const Label &slow) {
      const uint32_t row =
          m_off.dpc_tag + (write_row ? m_off.dpc_write_row : 0);
      // Index: one UBFX instead of LSR+AND. kDpcMask is a contiguous run of
      // low bits by construction (kDpcEntries is a power of two), so the two
      // forms are identical -- and this must keep matching
      // CAlphaCPU::dpc_index.
      const uint32_t idx_bits = a64_popcount_low(m_off.dpc_mask);
      if (idx_bits && ((1u << idx_bits) - 1u) == m_off.dpc_mask &&
          13 + idx_bits <= 64) {
        a.ubfx(x10, x2, imm(13), imm(idx_bits));
      } else {
        a.lsr(x10, x2, imm(13)); // must match CAlphaCPU::dpc_index
        a.and_(x10, x10, imm((uint64_t)m_off.dpc_mask));
      }
      if (m_off.dpc_stride == 64 && row + dpc_bias_rel <= 32760) {
        // A 64-byte slot makes the index a shift, and both fields sit within
        // one load's displacement of the slot.
        a.add(x10, kCpu, x10, a64::lsl(6));
      } else {
        a.mov(x11, imm(m_off.dpc_stride));
        a.mul(x10, x10, x11);
        a.mov(x11, imm((uint64_t)row));
        a.add(x10, x10, x11);
        a.add(x10, kCpu, x10);
      }
      const int32_t base =
          (m_off.dpc_stride == 64 && row + dpc_bias_rel <= 32760) ? (int32_t)row
                                                                  : 0;
      // Tag and bias are adjacent 8-byte fields, so one LDP fetches the pair
      // -- which is what the slot layout was designed for. LDP's immediate is
      // a signed 7-bit value scaled by 8, so it reaches +504: the read row
      // (offset 400) fits, the write row (4496) does not and keeps two loads.
      if (dpc_bias_rel == 8 && (base % 8) == 0 && base >= -512 && base <= 504) {
        a.ldp(x12, x10, a64::ptr(x10, base)); // slot tag, slot bias
      } else {
        a.ldr(x12, a64::ptr(x10, base));                         // slot tag
        a.ldr(x10, a64::ptr(x10, base + (int32_t)dpc_bias_rel)); // slot bias
      }
      a.and_(x11, x2, imm(~(uint64_t)0x1FFF));                  // this page
      a.ldr(x9, fld(m_off.dpc_key, 3)); // the live address space and mode
      a.orr(x11, x11, x9);              // ... which together are the key
      a.cmp(x12, x11);
      a.b_ne(slow);
    };
    // The second way (CAlphaCPU::data_page_cache2), probed from a cold stub,
    // so only after way 0 has missed: a hit in way 0 pays nothing for it.
    // The stub is also where a misaligned access lands, so alignment is
    // tested again. The probe and the swap live in one shared thunk
    // (a64_dpc2_thunk). On a hit x10 = the page's bias, as after dpc_probe.
    // False when there is no second way: go straight to the helper.
    auto dpc_probe_way1 = [&](bool write_row, int size_bits,
                              const Label &miss) -> bool {
      void *thunk = a64_dpc2_thunk();
      if (!thunk)
        return false;
      if (size_bits > 8) {
        a.tst(x2, imm((uint64_t)(size_bits / 8 - 1)));
        a.b_ne(miss);
      }
      a.mov(x9, imm((uint64_t)(m_off.dpc_tag +
                               (write_row ? m_off.dpc_write_row : 0))));
      a.mov(a64::x17, imm((uint64_t)thunk));
      a.blr(a64::x17);
      a.b_ne(miss);
      return true;
    };
#endif

    // Memory-format loads: Ra = MEM[Rb + disp16].
    if (op == OP_LDQ || op == OP_LDL || op == OP_LDBU || op == OP_LDWU ||
        op == OP_LDQ_U) {
      if (ra == 31)
        continue; // LDx R31 is a NOP (interpreter skips the read)
      const int size_bits = (op == OP_LDQ || op == OP_LDQ_U) ? 64
                            : (op == OP_LDL)                 ? 32
                            : (op == OP_LDWU)                ? 16
                                                             : 8;
      auto load_from = [&](const a64::Mem &m) {
        // Land the value in the destination's own pinned register where there
        // is one, instead of loading into x0 and moving it across. The cost is
        // that x0 no longer mirrors Ra, so this path forgoes value-forwarding
        // -- worth it, because the forward only pays when the very next
        // instruction reads Ra, while the extra MOV was paid by every load.
        const int p = regalloc.host_of(ra);
        const a64::Gp dst = (p >= 0) ? a64::x((uint32_t)p) : x0;
        const a64::Gp dst32 = (p >= 0) ? a64::w((uint32_t)p) : w0;
        if (size_bits == 64)
          a.ldr(dst, m);
        else if (size_bits == 32)
          a.ldrsw(dst, m);
        else if (size_bits == 16)
          a.ldrh(dst32, m);
        else
          a.ldrb(dst32, m);
        if (p < 0)
          mov_to_reg(ra, x0); // unpinned: store it to the guest slot
      };
      auto emit_helper = [&]() {
        emit_call(hs.read_helper, {{JA_CPU, 0},
                                   {JA_VA, 0},
                                   {JA_I32, (uint64_t)size_bits},
                                   {JA_OUT, 0}});
        bail_if_w0();
        load_from(out_slot);
      };
#ifdef JIT_VERIFY
      ea_x2((int16_t)(ins & 0xFFFF));
      if (op == OP_LDQ_U)
        a.and_(x2, x2, imm(~(uint64_t)7));
      emit_helper();
#else
      if (m_cold_pass) { // x2 = va, as the hot path left it
        a.bind(Label(m_cold_slow[cold_idx]));
        Label miss = a.new_label();
        if (dpc_probe_way1(false, size_bits, miss)) {
          load_from(a64::ptr(x10, x2));
          a.b(Label(m_cold_back[cold_idx]));
          a.bind(miss);
        }
        emit_helper();
        a.b(Label(m_cold_back[cold_idx]));
        continue;
      }
      ea_x2((int16_t)(ins & 0xFFFF));
      if (op == OP_LDQ_U)
        a.and_(x2, x2, imm(~(uint64_t)7));
      Label slow = a.new_label(), ldone = a.new_label();
      if (size_bits > 8) {
        a.tst(x2, imm((uint64_t)(size_bits / 8 - 1)));
        a.b_ne(slow);
      }
      dpc_probe(false, slow);
      load_from(a64::ptr(x10, x2));
      if (!cold_record(slow, ldone)) {
        a.b(ldone);
        a.bind(slow);
        emit_helper();
      }
      a.bind(ldone);
#endif
      continue;
    }

    // Memory-format stores: MEM[Rb + disp16] = Ra.
    if (op == OP_STL || op == OP_STQ || op == OP_STB || op == OP_STW ||
        op == OP_STQ_U) {
      const int size_bits = (op == OP_STQ || op == OP_STQ_U) ? 64
                            : (op == OP_STL)                 ? 32
                            : (op == OP_STW)                 ? 16
                                                             : 8;
      auto emit_helper = [&]() {
        emit_call(hs.write_helper, {{JA_CPU, 0},
                                    {JA_VA, 0},
                                    {JA_I32, (uint64_t)size_bits},
                                    {JA_GPZ, (uint64_t)ra}});
        bail_if_w0();
      };
#ifdef JIT_VERIFY
      ea_x2((int16_t)(ins & 0xFFFF));
      if (op == OP_STQ_U)
        a.and_(x2, x2, imm(~(uint64_t)7));
      emit_helper();
#else
      if (m_cold_pass) { // x2 = va, as the hot path left it
        a.bind(Label(m_cold_slow[cold_idx]));
        Label miss = a.new_label();
        if (dpc_probe_way1(true, size_bits, miss)) {
          if (ra == 31)
            a.mov(x12, imm(0));
          else
            mov_from_reg(x12, ra);
          const a64::Mem m = a64::ptr(x10, x2);
          if (size_bits == 64)
            a.str(x12, m);
          else if (size_bits == 32)
            a.str(w12, m);
          else if (size_bits == 16)
            a.strh(w12, m);
          else
            a.strb(w12, m);
          a.b(Label(m_cold_back[cold_idx]));
          a.bind(miss);
        }
        emit_helper();
        a.b(Label(m_cold_back[cold_idx]));
        continue;
      }
      ea_x2((int16_t)(ins & 0xFFFF));
      if (op == OP_STQ_U)
        a.and_(x2, x2, imm(~(uint64_t)7));
      Label slow = a.new_label(), sdone = a.new_label();
      if (size_bits > 8) {
        a.tst(x2, imm((uint64_t)(size_bits / 8 - 1)));
        a.b_ne(slow);
      }
      dpc_probe(true, slow);
      if (ra == 31)
        a.mov(x12, imm(0));
      else
        mov_from_reg(x12, ra);
      const a64::Mem m = a64::ptr(x10, x2);
      if (size_bits == 64)
        a.str(x12, m);
      else if (size_bits == 32)
        a.str(w12, m);
      else if (size_bits == 16)
        a.strh(w12, m);
      else
        a.strb(w12, m);
      if (!cold_record(slow, sdone)) {
        a.b(sdone);
        a.bind(slow);
        emit_helper();
      }
      a.bind(sdone);
#endif
      continue;
    }

    // FP memory (LDS/LDT/STS/STT + VAX LDF/LDG/STF/STG): f[Fa] <->
    // MEM[Rb+disp16]. LDT/STT (raw 8B) get the inline path; the converting
    // forms go through the helper.
    if (op == OP_LDT || op == OP_LDS || op == OP_STT || op == OP_STS ||
        op == OP_LDF || op == OP_LDG || op == OP_STF || op == OP_STG) {
      const bool isload =
          (op == OP_LDT || op == OP_LDS || op == OP_LDF || op == OP_LDG);
      const bool israw = (op == OP_LDT || op == OP_STT);
      const int fa = ra;
      const uint32_t fmt = (op == OP_LDS || op == OP_STS)   ? 1u
                           : (op == OP_LDF || op == OP_STF) ? 2u
                           : (op == OP_LDG || op == OP_STG) ? 3u
                                                            : 0u;
      const int size_bits = (fmt == 1 || fmt == 2) ? 32 : 64;
      const uint32_t descr = (fmt << 16) | (uint32_t)size_bits;
      if (isload && fa == 31)
        continue; // LDT/LDS f31: interp skips the read (NOP)
#ifndef JIT_VERIFY
      if (m_cold_pass) { // only the raw LDT/STT inline path records; x2 = va
        a.bind(Label(m_cold_slow[cold_idx]));
        emit_call(isload ? hs.fp_read_helper : hs.fp_write_helper,
                  {{JA_CPU, 0},
                   {JA_VA, 0},
                   {JA_I32, (uint64_t)fa},
                   {JA_I32, (uint64_t)descr}});
        bail_if_w0();
        a.b(Label(m_cold_back[cold_idx]));
        continue;
      }
#endif
      ea_x2((int16_t)(ins & 0xFFFF));
      auto emit_helper = [&]() {
        emit_call(isload ? hs.fp_read_helper : hs.fp_write_helper,
                  {{JA_CPU, 0},
                   {JA_VA, 0},
                   {JA_I32, (uint64_t)fa},
                   {JA_I32, (uint64_t)descr}});
        bail_if_w0();
      };
#ifdef JIT_VERIFY
      (void)israw;
      emit_helper();
#else
      if (!israw) {
        emit_helper();
        continue;
      }
      Label slow = a.new_label(), fdone = a.new_label();
      a.ldrb(w11, fld(m_off.fpen, 0));
      a.cbz(w11, slow); // fpen==0 -> FEN trap
      a.str(a64::xzr, fld(m_off.exc_sum, 3));
      a.tst(x2, imm(7));
      a.b_ne(slow);
      dpc_probe(!isload, slow);
      if (isload) {
        a.ldr(x0, a64::ptr(x10, x2));
        a.str(x0, fld(m_off.f_base + (uint32_t)fa * 8, 3));
      } else {
        a.ldr(x12, fld(m_off.f_base + (uint32_t)fa * 8, 3));
        a.str(x12, a64::ptr(x10, x2));
      }
      if (!cold_record(slow, fdone)) {
        a.b(fdone);
        a.bind(slow);
        emit_helper();
      }
      a.bind(fdone);
#endif
      continue;
    }

    // Store-conditional STL_C/STQ_C: jit_stc -> 0x100 bail, else Ra = 1/0.
    if (op == OP_STL_C || op == OP_STQ_C) {
      const int size_bits = (op == OP_STQ_C) ? 64 : 32;
      ea_x2((int16_t)(ins & 0xFFFF));
      emit_call(hs.stc_helper, {{JA_CPU, 0},
                                {JA_VA, 0},
                                {JA_I32, (uint64_t)size_bits},
                                {JA_GP, (uint64_t)ra}});
      Label nobail = a.new_label();
      a.tst(x0, imm(0x100));
      a.b_eq(nobail);
      bail(i);
      a.bind(nobail);
      mov_to_reg(ra, x0);
      continue;
    }

    // HW_LD physical / VPTE / WrChk (PALmode): Ra = MEM[Rb + disp12].
    if (op == OP_HW_LDL || op == OP_HW_LDQ || op == OP_HW_LDQ_VPTE ||
        op == OP_HW_LDL_WCHK || op == OP_HW_LDL_VPTE || op == OP_HW_LD_VIRT) {
      if (ra == 31)
        continue;
      const uint32_t ld_fn = (ins >> 12) & 0xf;
      const bool ld_virt = (op == OP_HW_LD_VIRT || op == OP_HW_LDL_VPTE);
      const int size_bits = ld_virt ? ((ld_fn & 1) ? 64 : 32)
                            : (op == OP_HW_LDL || op == OP_HW_LDL_WCHK) ? 32
                                                                        : 64;
      // bit 8 of the size argument selects DTB_ALTMODE (jit_read)
      const uint32_t size_arg =
          (uint32_t)size_bits |
          ((op == OP_HW_LD_VIRT && ld_fn >= 12) ? 0x100u : 0u);
      auto ld_helper = [&]() {
        emit_call((op == OP_HW_LDQ_VPTE || op == OP_HW_LDL_VPTE)
                      ? hs.read_vpte_helper
                  : op == OP_HW_LDL_WCHK ? hs.read_wchk_helper
                  : op == OP_HW_LD_VIRT  ? hs.read_helper
                                         : hs.hw_ld_helper,
                  {{JA_CPU, 0},
                   {JA_VA, 0},
                   {JA_I32, (uint64_t)size_arg},
                   {JA_OUT, 0}});
        bail_if_w0();
        if (size_bits == 32)
          a.ldrsw(x0, out_slot); // longword sign-extends (see the x86 emitter)
        else
          a.ldr(x0, out_slot);
        mov_to_reg(ra, x0);
      };
#ifndef JIT_VERIFY
      const bool ld_phys = (op == OP_HW_LDL || op == OP_HW_LDQ);
      if (m_cold_pass) { // only the physical forms record; x2 = pa
        a.bind(Label(m_cold_slow[cold_idx]));
        ld_helper();
        a.b(Label(m_cold_back[cold_idx]));
        continue;
      }
#endif
      ea_x2((int32_t)(ins << 20) >> 20);
#ifndef JIT_VERIFY
      // Physical forms into DRAM load inline (NT's PALcode walks page tables
      // and per-CPU data this way millions of times per 100M instructions);
      // the MMIO range and the virtual / VPTE / WrChk forms take the helper.
      // Aligned like READ_PHYS_NT; dram_size is page-aligned, so an aligned
      // address below it has the whole datum in DRAM.
      if (ld_phys) {
        Label ld_slow = a.new_label(), ld_done = a.new_label();
        a.and_(x11, x2, imm(~(uint64_t)(size_bits / 8 - 1)));
        a.ldr(x10, fld(m_off.dram_size, 3));
        a.cmp(x11, x10);
        a.b_hs(ld_slow);
        a.ldr(x10, fld(m_off.dram_ptr, 3));
        if (size_bits == 32)
          a.ldrsw(x0, a64::ptr(x10, x11));
        else
          a.ldr(x0, a64::ptr(x10, x11));
        mov_to_reg(ra, x0);
        if (!cold_record(ld_slow, ld_done)) {
          a.b(ld_done);
          a.bind(ld_slow);
          ld_helper();
        }
        a.bind(ld_done);
        continue;
      }
#endif
      ld_helper();
      continue;
    }

    // Load-locked LDL_L/LDQ_L: Ra = MEM[Rb + disp16] + LL/SC monitor.
    if (op == OP_LDL_L || op == OP_LDQ_L) {
      const int size_bits = (op == OP_LDQ_L) ? 64 : 32;
      ea_x2((int16_t)(ins & 0xFFFF));
      emit_call(hs.read_locked_helper, {{JA_CPU, 0},
                                        {JA_VA, 0},
                                        {JA_I32, (uint64_t)size_bits},
                                        {JA_OUT, 0}});
      bail_if_w0();
      a.ldr(x0, out_slot); // already sign-extended by the helper
      mov_to_reg(ra, x0);
      continue;
    }

    // HW_MTPR side-effect-free IPRs (PALmode): jit_hw_mtpr(cpu, fn, Rb).
    if (op == OP_HW_MTPR || op == OP_HW_MTPR_TERM) {
      const uint32_t function = (ins >> 8) & 0xff;
      // IER inline: NT's PALcode rewrites IER on every IRQL change (~1 in 20
      // instructions under Windows, 99.9% of HW_MTPR helper calls). Mirrors
      // jit_hw_mtpr case 0x0a: store the enable fields, then set check_int if
      // int_deliverable(). The verify lane snapshots these fields, so it
      // checks this path too. ALPHABOX_IRQTRACE keeps the helper (it logs).
      static const bool irqtrace = getenv("ALPHABOX_IRQTRACE") != nullptr;
      if (function == 0x0a && !irqtrace) {
        const a64::Gp v = src_reg(rb, x1, false);
        const a64::Gp x3 = a64::x3, w2 = a64::w2, w3 = a64::w3, w10 = a64::w10;
        a.lsr(x2, v, imm(13));
        a.and_(w3, w2, imm(1));
        a.str(w3, fld(m_off.ier_asten, 2));
        a.and_(w3, w2, imm(0xfffe));
        a.str(w3, fld(m_off.ier_sien, 2));
        a.lsr(x2, v, imm(29));
        a.and_(w3, w2, imm(3));
        a.str(w3, fld(m_off.ier_pcen, 2));
        a.lsr(x2, v, imm(31));
        a.and_(w3, w2, imm(1));
        a.str(w3, fld(m_off.ier_cren, 2));
        a.lsr(x2, v, imm(32));
        a.and_(w3, w2, imm(1));
        a.str(w3, fld(m_off.ier_slen, 2));
        a.lsr(x2, v, imm(33));
        a.and_(w3, w2, imm(0x3f)); // w3 = eien
        a.str(w3, fld(m_off.ier_eien, 2));
        (void)x3;
        Label set_int = a.new_label(), no_int = a.new_label();
        a.ldr(w10, fld(m_off.eir, 2)); // eien & eir
        a.tst(w10, w3);
        a.b_ne(set_int);
        a.ldr(w10, fld(m_off.sir, 2)); // sien & sir
        a.ldr(w11, fld(m_off.ier_sien, 2));
        a.tst(w10, w11);
        a.b_ne(set_int);
        a.ldr(w10, fld(m_off.ier_asten, 2)); // asten && (aster & astrr &
        a.cbz(w10, no_int);                  //   ((1 << (cm + 1)) - 1))
        a.ldr(w10, fld(m_off.aster, 2));
        a.ldr(w11, fld(m_off.astrr, 2));
        a.and_(w10, w10, w11);
        a.ldr(w11, fld(m_off.state_cm, 2));
        a.add(w11, w11, imm(1));
        a.mov(w12, imm(1));
        a.lsl(w12, w12, w11);
        a.sub(w12, w12, imm(1));
        a.tst(w10, w12);
        a.b_eq(no_int);
        a.bind(set_int);
        a.mov(w10, imm(1));
        a.strb(w10, fld(m_off.check_int, 0));
        a.bind(no_int);
        continue;
      }
      emit_call(
          hs.hw_mtpr_helper,
          {{JA_CPU, 0}, {JA_I32, (uint64_t)function}, {JA_GPZ, (uint64_t)rb}});
      if (op == OP_HW_MTPR_TERM) // I_CTL: end the block, re-dispatch past it
        set_pc(b->tag + 4 * (uint64_t)(i + 1));
      continue;
    }

    // HW_ST physical (PALmode): phys[Rb + disp12] = Ra.
    if (op == OP_HW_STL || op == OP_HW_STQ || op == OP_HW_ST_VIRT) {
      const uint32_t st_fn = (ins >> 12) & 0xf;
      const int size_bits = (op == OP_HW_ST_VIRT)
                                ? ((st_fn & 1) ? 64 : 32)
                                : ((op == OP_HW_STQ) ? 64 : 32);
      // bit 8 of the size argument selects DTB_ALTMODE (jit_write)
      const uint32_t size_arg =
          (uint32_t)size_bits |
          ((op == OP_HW_ST_VIRT && st_fn >= 12) ? 0x100u : 0u);
      auto st_helper = [&]() {
        emit_call(op == OP_HW_ST_VIRT ? hs.write_helper : hs.hw_st_helper,
                  {{JA_CPU, 0},
                   {JA_VA, 0},
                   {JA_I32, (uint64_t)size_arg},
                   {JA_GPZ, (uint64_t)ra}});
        bail_if_w0();
      };
#ifndef JIT_VERIFY
      const bool st_phys = (op == OP_HW_STL || op == OP_HW_STQ);
      if (m_cold_pass) { // only the physical forms record; x2 = pa
        a.bind(Label(m_cold_slow[cold_idx]));
        st_helper();
        a.b(Label(m_cold_back[cold_idx]));
        continue;
      }
#endif
      ea_x2((int32_t)(ins << 20) >> 20);
#ifndef JIT_VERIFY
      // Physical forms into DRAM store inline (see HW_LD above); the MMIO
      // range and the virtual forms take the helper.
      if (st_phys) {
        Label st_slow = a.new_label(), st_done = a.new_label();
        a.and_(x11, x2, imm(~(uint64_t)(size_bits / 8 - 1)));
        a.ldr(x10, fld(m_off.dram_size, 3));
        a.cmp(x11, x10);
        a.b_hs(st_slow);
        a.ldr(x10, fld(m_off.dram_ptr, 3));
        const a64::Gp v = src_reg(ra, x12, false);
        if (size_bits == 32)
          a.str(a64::w(v.id()), a64::ptr(x10, x11));
        else
          a.str(v, a64::ptr(x10, x11));
        if (!cold_record(st_slow, st_done)) {
          a.b(st_done);
          a.bind(st_slow);
          st_helper();
        }
        a.bind(st_done);
        continue;
      }
#endif
      st_helper();
      continue;
    }

    // LDA / LDAH: Ra = Rb + disp16 (<< 16 for LDAH).
    if (op == OP_LDA || op == OP_LDAH) {
      if (ra == 31)
        continue;
      int64_t d = (int64_t)(int16_t)(ins & 0xFFFF);
      if (op == OP_LDAH)
        d *= 65536;
      const a64::Gp dst = dst_reg(ra);
      if (rb == 31)
        a.mov(dst, imm(d));
      else {
        const a64::Gp src = src_reg(rb, x0, true);
        if (d)
          a64_add_imm(a, dst, src, d);
        else if (src.id() != dst.id())
          a.mov(dst, src);
      }
      dst_done(ra, dst);
      continue;
    }

    // HW_MFPR (PALmode): Ra = IPR value (helper returns it; cur = Ra).
    if (op == OP_HW_MFPR) {
      if (ra != 31) {
        emit_call(
            hs.hw_mfpr_helper,
            {{JA_CPU, 0}, {JA_I32, (uint64_t)ins}, {JA_GP, (uint64_t)ra}});
        mov_to_reg(ra, x0);
      }
      continue;
    }

    // MISC state reads RPCC/RC/RS: Ra = jit_misc(cpu, sel).
    if (op == OP_RPCC || op == OP_RC || op == OP_RS) {
      const int sel = (op == OP_RPCC) ? 0 : (op == OP_RC) ? 1 : 2;
      // RPCC goes to the stub, which keeps the pins in place.
      //
      // Not on a JIT_VERIFY build: the verifier runs each block twice and
      // relies on jit_misc logging what the counter returned so the
      // interpreter's pass replays the same value. The stub cannot take
      // part in that, and without it every RPCC block reports a mismatch.
      // Everything else stays verified; this one instruction is checked
      // instead against what the firmware measures the CPU's speed to be.
#ifdef JIT_VERIFY
      const bool inline_rpcc = false;
#else
      const bool inline_rpcc = true;
#endif
      void *stub = (op == OP_RPCC && inline_rpcc) ? a64_rpcc_stub() : nullptr;
      if (stub) {
        a.mov(a64::x16, imm((uint64_t)stub));
        a.blr(a64::x16);
      } else
        emit_call(hs.misc_helper, {{JA_CPU, 0}, {JA_I32, (uint64_t)sel}});
      if (ra != 31)
        mov_to_reg(ra, x0);
      continue;
    }

    // ITOFx: f[Fc] = fmt(Ra) via jit_itof (FEN trap -> bail).
    if (op == OP_ITOFS || op == OP_ITOFF || op == OP_ITOFT) {
      emit_call(hs.itof_helper, {{JA_CPU, 0},
                                 {JA_I32, (uint64_t)rc},
                                 {JA_GPZ, (uint64_t)ra},
                                 {JA_I32, (uint64_t)(op == OP_ITOFS   ? 1
                                                     : op == OP_ITOFF ? 2
                                                                      : 0)}});
      bail_if_w0();
      continue;
    }

    // FTOIx: Rc = fmt(f[Fa]) via jit_ftoi (FEN trap -> bail).
    if (op == OP_FTOIS || op == OP_FTOIT) {
      emit_call(hs.ftoi_helper, {{JA_CPU, 0},
                                 {JA_I32, (uint64_t)ra},
                                 {JA_I32, (uint64_t)(op == OP_FTOIS ? 1 : 0)},
                                 {JA_OUT, 0}});
      bail_if_w0();
      a.ldr(x0, out_slot);
      mov_to_reg(rc, x0);
      continue;
    }

    // FLTL non-arithmetic: jit_fltl(cpu, ins) (FEN trap -> bail).
    if (op == OP_FLTL) {
      emit_call(hs.fltl_helper, {{JA_CPU, 0}, {JA_I32, (uint64_t)ins}});
      bail_if_w0();
      continue;
    }

    // FLTV VAX: 0 ok / 1 FEN bail (op not run) / 2 arith trap (op ran, GO_PAL
    // already set state.pc -> count it and return as-is).
    if (op == OP_FLTV) {
      // A trap inside the helper (GO_PAL) takes EXC_ADDR from current_pc,
      // which compiled code doesn't otherwise maintain.
      a.mov(x9, imm(b->tag + 4 * (uint64_t)i));
      a.str(x9, fld(m_off.state_current_pc, 3));
      emit_call(hs.fltv_helper, {{JA_CPU, 0}, {JA_I32, (uint64_t)ins}});
      Label ok = a.new_label(), trapped = a.new_label();
      a.cbz(w0, ok);
      a.cmp(w0, imm(2));
      a.b_eq(trapped);
      bail(i);
      a.bind(trapped);
      a64_add_imm(a, x0, a64::x27, i + 1);
      a.b(done);
      a.bind(ok);
      continue;
    }

    // Inline IEEE FP (FLTI 0x16 and the ITFP SQRTs). Same contract as the x86
    // emitter: complete natively only when the result is bit-exact with the
    // interpreter AND the interpreter would neither trap nor set a new FPCR
    // sticky bit (compiled code never writes FPCR); every other case bails to
    // the interpreter at this instruction. The host FPCR is the AAPCS64
    // default (round-to-nearest, no traps, no flush-to-zero; see
    // CAlphaCPU::run). Beyond the x86 checks this also bails on:
    //  - an underflow to zero (MUL/DIV/CVTTS with nonzero operands): the
    //    interpreter sets UNF (and traps with /U) although no denormal appears;
    //  - an S operand that isn't exactly representable in single precision
    //    (the interpreter operates on the full register value);
    //  - CVTTQ Inf/NaN operands up front, as fcvtz*/fcvtn* saturate (NaN -> 0)
    //    instead of returning x86's integer-indefinite value.
    if (op == OP_CVTQT || op == OP_CVTQS || op == OP_ADDT || op == OP_SUBT ||
        op == OP_MULT || op == OP_DIVT || op == OP_CMPTUN || op == OP_CMPTEQ ||
        op == OP_CMPTLT || op == OP_CMPTLE || op == OP_ADDS || op == OP_SUBS ||
        op == OP_MULS || op == OP_DIVS || op == OP_CVTST || op == OP_CVTTS ||
        op == OP_CVTTQ || op == OP_SQRTS || op == OP_SQRTT) {
      const a64::Vec d0 = a64::d(0), d1 = a64::d(1), d2 = a64::d(2);
      const a64::Vec s0 = a64::s(0), s1 = a64::s(1);
      const bool dyn = ((ins >> 11) & 3) == 3; // /D: FPCR<59:58> rounding
      const uint64_t kMag64 = ~((uint64_t)1 << 63);
      Label fbail = a.new_label(), cont = a.new_label();
      auto freg = [&](int r) {
        return fld(m_off.f_base + 8u * (uint32_t)r, 3);
      };
      // FPCR gates. INE already sticky -> an inexact result changes nothing.
      auto need_ine = [&]() {
        a.ldr(x11, fld(m_off.fpcr, 3));
        a.tbz(x11, imm(56), fbail);
      };
      auto need_nearest = [&]() {
        a.ldr(x11, fld(m_off.fpcr, 3));
        a.lsr(x11, x11, imm(58));
        a.and_(x11, x11, imm(3));
        a.cmp(x11, imm(2));
        a.b_ne(fbail);
      };
      // Class checks on raw register bits (clobber x10/x11). Denormal always
      // bails; chk also bails Inf/NaN.
      auto dbl_bail = [&](const a64::Gp &v, bool chk) {
        Label ok = a.new_label();
        a.lsr(x11, v, imm(52));
        a.and_(x11, x11, imm(0x7ff));
        if (chk) {
          a.cmp(x11, imm(0x7ff));
          a.b_eq(fbail);
        }
        a.cbnz(x11, ok);
        a.lsl(x10, v, imm(12));
        a.cbnz(x10, fbail);
        a.bind(ok);
      };
      auto sgl_bail = [&](const a64::Gp &v32, bool chk) {
        Label ok = a.new_label();
        a.lsr(a64::w11, v32, imm(23));
        a.and_(a64::w11, a64::w11, imm(0xff));
        if (chk) {
          a.cmp(a64::w11, imm(0xff));
          a.b_eq(fbail);
        }
        a.cbnz(a64::w11, ok);
        a.lsl(a64::w10, v32, imm(9));
        a.cbnz(a64::w10, fbail);
        a.bind(ok);
      };
      // Compare operands: NaN or denormal bails; zero/Inf/normal compare.
      auto cmp_bail = [&](const a64::Gp &v) {
        Label ok = a.new_label(), special = a.new_label();
        a.lsr(x11, v, imm(52));
        a.and_(x11, x11, imm(0x7ff));
        a.cmp(x11, imm(0x7ff));
        a.b_eq(special);
        a.cbnz(x11, ok);
        a.bind(special);
        a.lsl(x10, v, imm(12));
        a.cbnz(x10, fbail);
        a.bind(ok);
      };
      // vd <- v (register bits) narrowed to single in vs; bail unless exact.
      auto narrow_exact = [&](const a64::Gp &v, const a64::Vec &vd,
                              const a64::Vec &vs) {
        a.fmov(vd, v);
        a.fcvt(vs, vd);
        a.fcvt(d2, vs);
        a.fmov(x10, d2);
        a.cmp(x10, v);
        a.b_ne(fbail);
      };

      a.ldrb(w11, fld(m_off.fpen, 0)); // FPSTART: FP disabled -> FEN trap
      a.cbz(w11, fbail);
      a.str(a64::xzr, fld(m_off.exc_sum, 3));

      switch (op) {
      case OP_CVTQT:
      case OP_CVTQS: { // f[Fc] = (T|S)(s64) f[Fb]
        Label exact = a.new_label(), inexact = a.new_label();
        if (dyn)
          need_nearest();
        if (rb == 31)
          a.mov(x0, imm(0));
        else
          a.ldr(x0, freg(rb));
        if (op == OP_CVTQT) {
          a.scvtf(d0, x0);
          a.fcvtzs(x1, d0);
        } else {
          a.scvtf(s0, x0);
          a.fcvtzs(x1, s0);
        }
        a.cmp(x1, x0); // round trip equal -> exact ...
        a.b_ne(inexact);
        a.mov(x12, imm(kMag64)); // ... except INT64_MAX, which saturation
        a.cmp(x0, x12);          // hides (2^63 is never exact)
        a.b_ne(exact);
        a.bind(inexact);
        need_ine();
        a.bind(exact);
        if (op == OP_CVTQS)
          a.fcvt(d0, s0);
        a.fmov(x12, d0);
        break;
      }
      case OP_ADDT:
      case OP_SUBT:
      case OP_MULT:
      case OP_DIVT:
      case OP_ADDS:
      case OP_SUBS:
      case OP_MULS:
      case OP_DIVS: {
        const bool sgl =
            op == OP_ADDS || op == OP_SUBS || op == OP_MULS || op == OP_DIVS;
        const bool mul = op == OP_MULT || op == OP_MULS;
        const bool div = op == OP_DIVT || op == OP_DIVS;
        need_ine();
        if (dyn)
          need_nearest();
        a.ldr(x0, freg(ra));
        a.ldr(x1, freg(rb));
        dbl_bail(x0, false);
        dbl_bail(x1, false);
        if (sgl) {
          narrow_exact(x0, d0, s0);
          narrow_exact(x1, d1, s1);
        } else {
          a.fmov(d0, x0);
          a.fmov(d1, x1);
        }
        const a64::Vec &l = sgl ? s0 : d0;
        const a64::Vec &r = sgl ? s1 : d1;
        if (op == OP_ADDT || op == OP_ADDS)
          a.fadd(l, l, r);
        else if (op == OP_SUBT || op == OP_SUBS)
          a.fsub(l, l, r);
        else if (mul)
          a.fmul(l, l, r);
        else
          a.fdiv(l, l, r);
        Label nz = a.new_label();
        if (sgl) {
          a.fmov(w12, s0);
          sgl_bail(w12, true); // Inf/NaN/denormal result
          a.tst(w12, imm(0x7fffffff));
        } else {
          a.fmov(x12, d0);
          dbl_bail(x12, true);
          a.tst(x12, imm(kMag64));
        }
        if (mul || div) { // zero result: underflow unless an operand is 0
          a.b_ne(nz);
          a.tst(x0, imm(kMag64));
          a.b_eq(nz);
          if (mul) {
            a.tst(x1, imm(kMag64));
            a.b_ne(fbail);
          } else {
            a.b(fbail);
          }
        }
        a.bind(nz);
        if (sgl) {
          a.fcvt(d0, s0);
          a.fmov(x12, d0);
        }
        break;
      }
      case OP_CMPTUN:
      case OP_CMPTEQ:
      case OP_CMPTLT:
      case OP_CMPTLE: { // f[Fc] = (Fa cmp Fb) ? 2.0 : 0.0
        a.ldr(x0, freg(ra));
        a.ldr(x1, freg(rb));
        cmp_bail(x0);
        cmp_bail(x1);
        if (op == OP_CMPTUN) {
          a.mov(x12, imm(0)); // both ordered -> unordered is false
        } else {
          a.fmov(d0, x0);
          a.fmov(d1, x1);
          a.fcmp(d0, d1);
          a.cset(x12, a64_cc(op == OP_CMPTEQ   ? CondCode::kEQ
                             : op == OP_CMPTLT ? CondCode::kMI
                                               : CondCode::kLS));
          a.lsl(x12, x12, imm(62));
        }
        break;
      }
      case OP_CVTST: // zero/normal S bits are already valid T
        a.ldr(x12, freg(rb));
        dbl_bail(x12, true);
        break;
      case OP_CVTTS: { // T -> S narrow
        Label nz = a.new_label();
        need_ine();
        if (dyn)
          need_nearest();
        a.ldr(x0, freg(rb));
        dbl_bail(x0, false);
        a.fmov(d0, x0);
        a.fcvt(s0, d0);
        a.fmov(w12, s0);
        sgl_bail(w12, true);
        a.tst(w12, imm(0x7fffffff)); // zero from a nonzero operand: underflow
        a.b_ne(nz);
        a.tst(x0, imm(kMag64));
        a.b_ne(fbail);
        a.bind(nz);
        a.fcvt(d0, s0);
        a.fmov(x12, d0);
        break;
      }
      case OP_CVTTQ: { // T -> s64 bits
        const bool chop = ((ins >> 11) & 3) == 0;
        Label exact = a.new_label();
        if (dyn)
          need_nearest();
        a.ldr(x0, freg(rb));
        dbl_bail(x0, true);
        a.fmov(d0, x0);
        if (chop)
          a.fcvtzs(x12, d0);
        else
          a.fcvtns(x12, d0); // nearest, ties to even (the interpreter's rule)
        a.mov(x10, imm((uint64_t)1 << 63)); // saturated -> overflow (IOV)
        a.cmp(x12, x10);
        a.b_eq(fbail);
        a.mvn(x10, x10);
        a.cmp(x12, x10);
        a.b_eq(fbail);
        a.scvtf(d1, x12); // round trip == source -> exact
        a.fcmp(d1, d0);
        a.b_eq(exact);
        need_ine();
        a.bind(exact);
        break;
      }
      default: { // OP_SQRTT / OP_SQRTS
        need_ine();
        if (dyn)
          need_nearest();
        a.ldr(x0, freg(rb));
        dbl_bail(x0, false);
        if (op == OP_SQRTT) {
          a.fmov(d0, x0);
          a.fsqrt(d0, d0);
          a.fmov(x12, d0);
          dbl_bail(x12, true); // Inf/NaN (negative operand)/denormal
        } else {
          narrow_exact(x0, d0, s0);
          a.fsqrt(s0, s0);
          a.fmov(w12, s0);
          sgl_bail(w12, true);
          a.fcvt(d0, s0);
          a.fmov(x12, d0);
        }
        break;
      }
      }
      a.str(x12, freg(rc));
      a.b(cont);
      a.bind(fbail);
      bail(i);
      a.bind(cont);
      continue;
    }

    // JMP/JSR/RET: Ra = PC+4; PC = Rb & ~3 (| current mode bits). x9 = target
    // for the epilogue's jit_indirect chain.
    if (op == OP_JMP) {
      const uint64_t ret = b->tag + 4 * (uint64_t)(i + 1);
      if (rb == 31)
        a.mov(x9, imm(0));
      else
        mov_from_reg(x9, rb);
      a.and_(x9, x9, imm(~(uint64_t)3));
      if (b->tag & 3)
        a.orr(x9, x9, imm(b->tag & 3));
      if (ra != 31) {
        a.mov(x0, imm(ret & ~(uint64_t)3));
        mov_to_reg(ra, x0);
      }
      a.str(x9, fld(m_off.state_pc, 3));
      continue;
    }

    // HW_RET (PALmode): PC = Rb & ~2.
    if (op == OP_HW_RET) {
      if (rb == 31)
        a.mov(x9, imm(0));
      else
        mov_from_reg(x9, rb);
      a.and_(x9, x9, imm(~(uint64_t)2));
      a.str(x9, fld(m_off.state_pc, 3));
      continue;
    }

    // CALL_PAL: R23 (shadow-aware) = return address, EXC_ADDR = this PC,
    // PC = pal_base | entry offset; privileged funcs OPCDEC in user mode.
    if (op == OP_CALL_PAL) {
      const uint32_t func = ins & 0x1FFFFFFF;
      const uint64_t cpc = b->tag + 4 * (uint64_t)i;
      const uint64_t ret = (b->tag + 4 * (uint64_t)(i + 1)) & ~(uint64_t)2;
      const uint64_t voff = (uint64_t)0x2000 | ((uint64_t)(func & 0x80) << 5) |
                            ((uint64_t)(func & 0x3f) << 6) | (uint64_t)1;
      Label do_vector = a.new_label();
      if (func < 0x40) {
        a.ldr(w11, fld(m_off.state_cm, 2));
        a.cbz(w11, do_vector);
        emit_call(hs.opcdec_helper, {{JA_CPU, 0}, {JA_I64, cpc}});
        a64_count_add(a, i + 1); // helper already wrote state.pc
        a.mov(x0, a64::x27);     // done expects x0 = count
        a.b(done);
      }
      a.bind(do_vector);
      a.mov(x11, imm(cpc));
      a.str(x11, fld(m_off.exc_addr, 3));
      a.ldrb(w11, fld(m_off.sde, 0));
      a.lsl(w11, w11, imm(5));
      a.add(w11, w11, imm(23)); // R23 index: 23, or 55 if SDE
      a.mov(x12, imm(ret));
      a.str(x12, a64::ptr(kRegs, x11, a64::lsl(3)));
      a.ldr(x9, fld(m_off.pal_base, 3));
      a.mov(x11, imm(voff));
      a.orr(x9, x9, x11);
      a.str(x9, fld(m_off.state_pc, 3));
      continue;
    }

    // FP branches: FPSTART, then branch on f[Fa] vs 0.0 through the same
    // sign-magnitude -> monotonic signed mapping as the x86 emitter.
    if (is_fp_branch(op)) {
      const int64_t bdisp = (int64_t)((uint64_t)(ins & 0x1FFFFF) << 43) >> 43;
      const uint64_t fall = b->tag + 4 * (uint64_t)(i + 1);
      const uint64_t tgt = fall + (uint64_t)(bdisp * 4);
      Label fbail = a.new_label(), cont = a.new_label();
      a.ldrb(w11, fld(m_off.fpen, 0));
      a.cbz(w11, fbail);
      a.str(a64::xzr, fld(m_off.exc_sum, 3));
      if (ra == 31)
        a.mov(x0, imm(0));
      else {
        a.ldr(x0, fld(m_off.f_base + 8u * (uint32_t)ra, 3));
        a.asr(x10, x0, imm(63));                   // sign mask
        a.and_(x0, x0, imm(~((uint64_t)1 << 63))); // magnitude
        a.eor(x0, x0, x10);
        a.sub(x0, x0, x10); // s = sign ? -magnitude : magnitude
      }
      a.mov(x9, imm(fall));
      a.mov(x10, imm(tgt));
      a.cmp(x0, imm(0));
      CondCode cc = CondCode::kEQ;
      switch (op) {
      case OP_FBEQ:
        cc = CondCode::kEQ;
        break;
      case OP_FBNE:
        cc = CondCode::kNE;
        break;
      case OP_FBLT:
        cc = CondCode::kLT;
        break;
      case OP_FBGE:
        cc = CondCode::kGE;
        break;
      case OP_FBLE:
        cc = CondCode::kLE;
        break;
      default:
        cc = CondCode::kGT;
        break; // OP_FBGT
      }
      a.csel(x9, x10, x9, a64_cc(cc));
      a.str(x9, fld(m_off.state_pc, 3));
      a.b(cont);
      a.bind(fbail);
      bail(i);
      a.bind(cont);
      continue;
    }

    // Integer branches: x9 = next PC (target or fall-through) -> state.pc.
    if (is_branch(op)) {
      const int64_t bdisp = (int64_t)((uint64_t)(ins & 0x1FFFFF) << 43) >> 43;
      const uint64_t fall = b->tag + 4 * (uint64_t)(i + 1);
      const uint64_t tgt = fall + (uint64_t)(bdisp * 4);
      if (op == OP_BR || op == OP_BSR) {
        if (ra != 31) {
          a.mov(x9, imm(fall & ~(uint64_t)3));
          mov_to_reg(ra, x9);
        }
        if (m_defer_branch_pc)
          continue; // the exit writes the PC where it is read: miss and gate
        a.mov(x9, imm(tgt));
      } else {
        if (m_defer_branch_pc || i < m_block_last) {
          // Emit nothing -- and load nothing: the exit code tests Ra where it
          // lives (its pin, the value-forward slot, or one load of its own)
          // and materialises only the PC of the side actually taken. Every
          // op clears the value-forward slot on entry; put the previous op's
          // back, since nothing was emitted here to invalidate it. (Loading
          // Ra into x0 first and then claiming x0 still held the previous
          // op's value was harmless while a branch always ended the block;
          // with instructions after it, JIT_VERIFY caught 41k mismatches.)
          m_pending_br_op = (int)op;
          m_pending_br_ra = ra;
          regalloc.rax_holds = prev_x0;
          continue;
        }
        if (ra == 31)
          a.mov(x0, imm(0));
        else
          mov_from_reg(x0, ra);
        if (op == OP_BLBC || op == OP_BLBS)
          a.tst(x0, imm(1));
        else
          a.tst(x0, x0);
        a.mov(x9, imm(fall));
        a.mov(x10, imm(tgt));
        CondCode cc = CondCode::kEQ;
        switch (op) {
        case OP_BEQ:
        case OP_BLBC:
          cc = CondCode::kEQ;
          break;
        case OP_BNE:
        case OP_BLBS:
          cc = CondCode::kNE;
          break;
        case OP_BLT:
          cc = CondCode::kLT;
          break;
        case OP_BGE:
          cc = CondCode::kGE;
          break;
        case OP_BLE:
          cc = CondCode::kLE;
          break;
        default:
          cc = CondCode::kGT;
          break; // OP_BGT
        }
        a.csel(x9, x10, x9, a64_cc(cc));
      }
      a.str(x9, fld(m_off.state_pc, 3));
      continue;
    }

    // CMOVxx: Rc = cond(Ra) ? op2 : Rc.
    if (op == OP_CMOV) {
      if (rc == 31)
        continue;
      const uint32_t f = (ins >> 5) & 0x7f;
      op1_x0();
      op2_x1();
      mov_from_reg(x9, rc);
      if (f == 0x14 || f == 0x16)
        a.tst(x0, imm(1));
      else
        a.tst(x0, x0);
      CondCode cc = CondCode::kEQ;
      switch (f) {
      case 0x24: // CMOVEQ
      case 0x16: // CMOVLBC
        cc = CondCode::kEQ;
        break;
      case 0x26: // CMOVNE
      case 0x14: // CMOVLBS
        cc = CondCode::kNE;
        break;
      case 0x44:
        cc = CondCode::kLT;
        break;
      case 0x46:
        cc = CondCode::kGE;
        break;
      case 0x64:
        cc = CondCode::kLE;
        break;
      default:
        cc = CondCode::kGT;
        break; // 0x66 CMOVGT
      }
      a.csel(x9, x1, x9, a64_cc(cc));
      mov_to_reg(rc, x9);
      continue;
    }

    // INTS byte-manipulation (EXT/INS/MSK/ZAP), keyed on pos = op2 & 7.
    if (op == OP_EXTL || op == OP_EXTH || op == OP_INSL || op == OP_INSH ||
        op == OP_MSKL || op == OP_MSKH || op == OP_ZAP) {
      if (rc == 31)
        continue;
      const uint32_t f = (ins >> 5) & 0x7f;
      const int size = (f >> 4) & 3;
      const uint64_t mask = (size == 0)   ? (uint64_t)0xff
                            : (size == 1) ? (uint64_t)0xffff
                            : (size == 2) ? (uint64_t)0xffffffff
                                          : ~(uint64_t)0;
      op1_x0(); // data
      op2_x1(); // selector
      switch (op) {
      case OP_EXTL: // (Ra >> pos*8) & mask
        a.and_(x1, x1, imm(7));
        a.lsl(x1, x1, imm(3));
        a.lsr(x0, x0, x1);
        if (size != 3)
          a.and_(x0, x0, imm(mask));
        break;
      case OP_EXTH: // (Ra << ((64-pos*8)&63)) & mask
        a.and_(x1, x1, imm(7));
        a.lsl(x1, x1, imm(3));
        a.neg(x1, x1);
        a.and_(x1, x1, imm(63));
        a.lsl(x0, x0, x1);
        if (size != 3)
          a.and_(x0, x0, imm(mask));
        break;
      case OP_INSL: // (Ra & mask) << pos*8
        if (size != 3)
          a.and_(x0, x0, imm(mask));
        a.and_(x1, x1, imm(7));
        a.lsl(x1, x1, imm(3));
        a.lsl(x0, x0, x1);
        break;
      case OP_INSH: // pos ? ((Ra&mask) >> ((64-pos*8)&63)) : 0
        if (size != 3)
          a.and_(x0, x0, imm(mask));
        a.and_(x1, x1, imm(7));
        a.mov(x10, x1);
        a.lsl(x1, x1, imm(3));
        a.neg(x1, x1);
        a.and_(x1, x1, imm(63));
        a.lsr(x0, x0, x1);
        a.mov(x11, imm(0));
        a.cmp(x10, imm(0));
        a.csel(x0, x11, x0, a64_cc(CondCode::kEQ));
        break;
      case OP_MSKL: // Ra & ~(mask << pos*8)
        a.and_(x1, x1, imm(7));
        a.lsl(x1, x1, imm(3));
        a.mov(x9, imm(mask));
        a.lsl(x9, x9, x1);
        a.bic(x0, x0, x9);
        break;
      case OP_MSKH: // pos ? (Ra & ~(mask >> ((64-pos*8)&63))) : Ra
        a.and_(x1, x1, imm(7));
        a.mov(x10, x1);
        a.lsl(x1, x1, imm(3));
        a.neg(x1, x1);
        a.and_(x1, x1, imm(63));
        a.mov(x9, imm(mask));
        a.lsr(x9, x9, x1);
        a.bic(x11, x0, x9);
        a.cmp(x10, imm(0));
        a.csel(x0, x11, x0, a64_cc(CondCode::kNE));
        break;
      case OP_ZAP: // Ra & byte_expand(selector); ZAP inverts
        a.and_(x1, x1, imm(0xff));
        a.mov(x11, imm((uint64_t)&g_zapnot_mask[0]));
        a.ldr(x9, a64::ptr(x11, x1, a64::lsl(3)));
        if (f == 0x30)
          a.mvn(x9, x9); // ZAP keeps bytes whose bit is CLEAR
        a.and_(x0, x0, x9);
        break;
      default:
        break;
      }
      mov_to_reg(rc, x0);
      continue;
    }

    // Rc = Ra <op> (lit | Rb) for the non-trapping register ops.
    enum {
      B_ADD,
      B_SUB,
      B_AND,
      B_ORR,
      B_EOR,
      B_BIC,
      B_ORN,
      B_EON,
      B_MUL,
      B_UMULH
    };
    auto binop = [&](int kind) {
      const a64::Gp s1 = op1_src();
      a64::Gp s2 = x1;
      if (islit && (kind == B_ADD || kind == B_SUB)) {
        const a64::Gp d = rc_dst();
        if (kind == B_ADD)
          a.add(d, s1, imm(lit));
        else
          a.sub(d, s1, imm(lit));
        rc_done(d);
        return;
      }
      if (islit)
        a.mov(x1, imm(lit));
      else
        s2 = op2_src();
      const a64::Gp d = rc_dst();
      switch (kind) {
      case B_ADD:
        a.add(d, s1, s2);
        break;
      case B_SUB:
        a.sub(d, s1, s2);
        break;
      case B_AND:
        a.and_(d, s1, s2);
        break;
      case B_ORR:
        a.orr(d, s1, s2);
        break;
      case B_EOR:
        a.eor(d, s1, s2);
        break;
      case B_BIC:
        a.bic(d, s1, s2);
        break;
      case B_ORN:
        a.orn(d, s1, s2);
        break;
      case B_EON:
        a.eon(d, s1, s2);
        break;
      case B_MUL:
        a.mul(d, s1, s2);
        break;
      default:
        a.umulh(d, s1, s2);
        break;
      }
      rc_done(d);
    };
    // BIS with Ra = R31 is the Alpha MOV idiom: Rc = Rb | lit.
    if (op == OP_BIS && ra == 31) {
      const a64::Gp d = rc_dst();
      if (islit)
        a.mov(d, imm(lit));
      else if (rb == 31)
        a.mov(d, imm(0));
      else {
        const int p = regalloc.host_of(rb);
        if (p >= 0) {
          if ((uint32_t)p != d.id())
            a.mov(d, a64::x((uint32_t)p));
        } else if (prev_x0 == rb && d.id() == 0) {
          // x0 already holds Rb
        } else if (prev_x0 == rb) {
          a.mov(d, x0);
        } else
          a.ldr(d, reg(rb));
      }
      rc_done(d);
      continue;
    }

    switch (op) {
    case OP_ADDQ:
      binop(B_ADD);
      continue;
    case OP_SUBQ:
      binop(B_SUB);
      continue;
    case OP_AND:
      binop(B_AND);
      continue;
    case OP_BIS:
      binop(B_ORR);
      continue;
    case OP_XOR:
      binop(B_EOR);
      continue;
    case OP_BIC:
      binop(B_BIC);
      continue;
    case OP_ORNOT:
      binop(B_ORN);
      continue;
    case OP_EQV:
      binop(B_EON);
      continue;
    case OP_MULQ:
      binop(B_MUL);
      continue;
    case OP_UMULH:
      binop(B_UMULH);
      continue;
    case OP_MULL: // 32-bit multiply, low 32 sign-extended
      if (ra == 31)
        a.mov(w0, imm(0));
      else
        mov_from_reg32(w0, ra);
      if (islit)
        a.mov(w1, imm(lit));
      else if (rb == 31)
        a.mov(w1, imm(0));
      else
        mov_from_reg32(w1, rb);
      a.mul(w0, w0, w1);
      a.sxtw(x0, w0);
      break;

    case OP_S4ADDQ:
    case OP_S8ADDQ:
    case OP_S4SUBQ:
    case OP_S8SUBQ:
      op1_x0();
      a.lsl(x0, x0, imm((op == OP_S4ADDQ || op == OP_S4SUBQ) ? 2 : 3));
      op2_x1();
      if (op == OP_S4ADDQ || op == OP_S8ADDQ)
        a.add(x0, x0, x1);
      else
        a.sub(x0, x0, x1);
      break;

    case OP_SLL: // AArch64 variable shifts take the count mod 64, like x86 CL
      op1_x0();
      op2_x1();
      a.lsl(x0, x0, x1);
      break;
    case OP_SRL:
      op1_x0();
      op2_x1();
      a.lsr(x0, x0, x1);
      break;
    case OP_SRA:
      op1_x0();
      op2_x1();
      a.asr(x0, x0, x1);
      break;

    case OP_SEXTB:
      op2_x1();
      a.sxtb(x0, w1);
      break;
    case OP_SEXTW:
      op2_x1();
      a.sxth(x0, w1);
      break;

    case OP_CTPOP: // SWAR popcount of op2
      op2_x1();
      a.lsr(x9, x1, imm(1));
      a.and_(x9, x9, imm(0x5555555555555555ull));
      a.sub(x1, x1, x9);
      a.and_(x9, x1, imm(0x3333333333333333ull));
      a.lsr(x1, x1, imm(2));
      a.and_(x1, x1, imm(0x3333333333333333ull));
      a.add(x1, x1, x9);
      a.lsr(x9, x1, imm(4));
      a.add(x1, x1, x9);
      a.and_(x1, x1, imm(0x0f0f0f0f0f0f0f0full));
      a.mov(x9, imm(0x0101010101010101ull));
      a.mul(x1, x1, x9);
      a.lsr(x0, x1, imm(56));
      break;
    case OP_CTLZ: // CLZ(0) == 64 == CTLZ(0)
      op2_x1();
      a.clz(x0, x1);
      break;
    case OP_CTTZ: // ctz = clz(bit-reverse); 0 -> 64
      op2_x1();
      a.rbit(x0, x1);
      a.clz(x0, x0);
      break;

    case OP_AMASK: // Rc = op2 & ~AMASK, this processor's extensions
      op2_x1();
      a.mov(x9, imm(m_amask));
      a.bic(x0, x1, x9);
      break;
    case OP_IMPLVER: // Rc = IMPLVER, this processor's implementation version
      a.mov(x0, imm(m_implver));
      break;

    case OP_CMPEQ:
    case OP_CMPLT:
    case OP_CMPLE:
    case OP_CMPULT:
    case OP_CMPULE: {
      const a64::Gp s1 = op1_src();
      if (islit)
        a.cmp(s1, imm(lit));
      else
        a.cmp(s1, op2_src());
      const CondCode cc = (op == OP_CMPEQ)    ? CondCode::kEQ
                          : (op == OP_CMPLT)  ? CondCode::kLT
                          : (op == OP_CMPLE)  ? CondCode::kLE
                          : (op == OP_CMPULT) ? CondCode::kLO
                                              : CondCode::kLS;
      const a64::Gp d = rc_dst();
      a.cset(d, a64_cc(cc));
      rc_done(d);
      continue;
    }

    case OP_CMPBGE: { // per-byte unsigned Ra >= op2 -> bit i (SWAR)
      op1_x0();
      op2_x1();
      // Low 7 bits per byte: (a|H) - (b&~H) never borrows across bytes, and
      // its bit 7 is (a7 >= b7). Fold in the top bits: a >= b per byte is
      // (a & ~b) | (~(a ^ b) & d), taken at bit 7.
      a.mov(x10, imm(0x8080808080808080ull));
      a.orr(x11, x0, x10);
      a.bic(x12, x1, x10);
      a.sub(x11, x11, x12); // d
      a.eor(x12, x0, x1);
      a.bic(x11, x11, x12); // d & ~(a ^ b)
      a.bic(x12, x0, x1);   // a & ~b
      a.orr(x11, x11, x12);
      a.and_(x11, x11, x10);
      a.lsr(x11, x11, imm(7)); // 0x01 per set byte
      // Gather byte i's bit into bit i: x * 0x0102040810204080 >> 56.
      a.mov(x12, imm(0x0102040810204080ull));
      a.mul(x11, x11, x12);
      a.lsr(x0, x11, imm(56));
      break;
    }

    case OP_ADDL:
    case OP_SUBL:
    case OP_S4ADDL:
    case OP_S8ADDL:
    case OP_S4SUBL:
    case OP_S8SUBL: { // sext32((Ra*scale) +/- op2)
      const bool issub = (op == OP_SUBL || op == OP_S4SUBL || op == OP_S8SUBL);
      const int sh = (op == OP_S4ADDL || op == OP_S4SUBL)   ? 2
                     : (op == OP_S8ADDL || op == OP_S8SUBL) ? 3
                                                            : 0;
      if (ra == 31)
        a.mov(w0, imm(0));
      else
        mov_from_reg32(w0, ra);
      if (sh)
        a.lsl(w0, w0, imm(sh));
      if (islit)
        a.mov(w1, imm(lit));
      else if (rb == 31)
        a.mov(w1, imm(0));
      else
        mov_from_reg32(w1, rb);
      if (issub)
        a.sub(w0, w0, w1);
      else
        a.add(w0, w0, w1);
      a.sxtw(x0, w0);
      break;
    }
    default:
      break;
    }

    if (rc != 31)
      mov_to_reg(rc, x0);
  } while (0);
}

bool CJitEngine::assemble_block(JitBlock *b, const uint32_t *words,
                                uint32_t plen, bool terminator_branch,
                                bool terminator_jmp, const HelperSet &hs,
                                JitFn *out_fn, uint32_t *out_body_off,
                                size_t *out_csz) {
  using namespace asmjit;
  const bool pal_block = (b->tag & 1) != 0;
  CodeHolder code;
  if (code.init(((JitRuntime *)m_rt)->environment()) != Error::kOk)
    return false;
#ifdef JIT_DISASM
  StringLogger logger;
  code.set_logger(&logger);
#endif
  A64EmitErrors eh;
  eh.cpu_id = m_cpu_id;
  code.set_error_handler(&eh);
  a64::Assembler a(&code);

  a64_prologue(a, (uint64_t)&m_itb_gen);
  Label done = a.new_label();
  Label body = a.new_label(); // chained re-entry (after the prologue)
  a.bind(body);
  const size_t body_off = code.code_size();
#ifdef JIT_REGPROF
  a.mov(a64::x9, imm((uint64_t)&b->rp_hits));
  a.ldr(a64::x10, a64::ptr(a64::x9));
  a.add(a64::x10, a64::x10, imm(1));
  a.str(a64::x10, a64::ptr(a64::x9));
#endif

  RegAlloc ra;
  a64_regalloc(ra);
  m_cold_pass = false;
  m_cold_base = 0;
  for (uint32_t i = 0; i < plen && i < kColdMax; ++i)
    m_cold_used[i] = false;
  // Opt this pass into the condition-branching exit: a block ends at its
  // branch, so at most one op can leave a condition pending, and the epilogue
  // below is the only consumer. A JIT_VERIFY build has no epilogue, and the
  // cold pass and the trace builder do not opt in, so both keep writing the
  // PC where they stand.
  m_pending_br_op = -1;
#ifndef JIT_VERIFY
  {
    const uint32_t lw = plen ? words[plen - 1] : 0u;
    const uint32_t lopc = lw >> 26;
    m_defer_branch_pc = terminator_branch && lopc >= 0x30 && lopc <= 0x3f;
  }
#endif
#ifndef JIT_VERIFY
  const uint32_t off_body = (uint32_t)((char *)&b->jit_body - (char *)b);
  const uint32_t off_tag = (uint32_t)((char *)&b->tag - (char *)b);
  const uint32_t off_vgen = (uint32_t)((char *)&b->vgen - (char *)b);
  // Cached direct link (x9 = next PC): tail into a live successor's body --
  // compiled, mapping this PC, and validated under the current epoch --
  // else record a link-patch request and fall through to lbl.
  const int32_t off_link = (int32_t)((char *)&b->link[0] - (char *)b);
  const int32_t epoch_rel = (int32_t)((char *)&m_epoch - (char *)&m_itb_gen);
  ExitRec *xrec = nullptr; // this code's exit record (first static exit)
  auto emit_chain = [&](const Label &lbl) {
    Label miss = a.new_label();
    a.mov(a64::x3, imm((uint64_t)b)); // this block: link table + miss record
    {
      Label ok = a.new_label(); // PALmode target needs SDE (shadow remap)
      a.tst(a64::x9, imm(1));
      a.b_eq(ok);
      a.ldrb(a64::w1, a64_cpu_field(a, m_off.sde, 0));
      a.cbz(a64::w1, miss);
      a.bind(ok);
    }
    a.ldr(a64::x10, a64::ptr(a64::x28, epoch_rel)); // x10 = m_epoch
    for (int sl = 0; sl < kLinkSlots; ++sl) {
      Label nxt = (sl + 1 < kLinkSlots) ? a.new_label() : miss;
      a.ldr(a64::x0, a64::ptr(a64::x3, off_link + 8 * sl)); // b->link[sl]
      a.cbz(a64::x0, nxt);
      a.ldr(a64::x1, a64::ptr(a64::x0, (int32_t)off_body));
      a.cbz(a64::x1, nxt);
      a.ldr(a64::x2, a64::ptr(a64::x0, (int32_t)off_tag));
      a.cmp(a64::x2, a64::x9);
      a.b_ne(nxt);
      a.ldr(a64::x2, a64::ptr(a64::x0, (int32_t)off_vgen));
      a.cmp(a64::x2, a64::x10);
      a.b_ne(nxt);
      a.br(a64::x1); // HIT: tail in (shared frame)
      if (sl + 1 < kLinkSlots)
        a.bind(nxt);
    }
    a.bind(miss);
    a.str(a64::x3, a64_cpu_field(a, m_off.link_from, 3));
  };
  // Static successor: the next PC is a compile-time constant (x9 holds it), so
  // it gets its own link slot -- no slot scan. The tag compare stays: a cache
  // slot's JitBlock can be re-recorded for another PC under the same epoch. A
  // miss records link_from = b | (slot + 1) so the dispatcher fills exactly
  // this slot, then takes lbl (return to the dispatcher).
  auto emit_static_exit = [&](uint64_t target, ExitRec *&xr, int slot,
                              const Label &lbl) {
    if (target == b->tag) {
      a.b(body); // self-loop (the gate already ran)
      return;
    }
    Label miss = a.new_label();
    if (!xr)
      xr = alloc_exit_rec();
    if (!xr) { // no record to chain through: take the dispatcher every time
      a.mov(a64::x9, imm(target));
      a.str(a64::x9, a64_cpu_field(a, m_off.state_pc, 3));
      emit_chain(lbl);
      a.b(lbl);
      return;
    }
    a.mov(a64::x3, imm((uint64_t)xr)); // this exit's record
    const int32_t off_lbody = (int32_t)offsetof(ExitRec, body);
    const int32_t off_lepoch = (int32_t)offsetof(ExitRec, epoch);
    // Every static exit is an epoch-guarded data link: the body cached for
    // this slot is entered while the epoch it was cached in is still current,
    // and any flush, ITB invalidate or ASN change bumps that epoch. An
    // epoch-free link for a target in this block's own page was tried and
    // rejected -- a cached link misses about a thousand times per 100M
    // instructions on CPU-bound guest code, so there was nothing to win, and
    // undoing such links cost a walk of the block cache on every icache
    // flush, which firmware issues once per ~1000 instructions. See
    // docs/performance.md.
    //
    // The SDE guard goes with the epoch: PALmode-ness is part of the page
    // identity here, so a PALmode target is only ever reached from PALmode
    // code that the dispatcher already gated on SDE, and no compiled
    // instruction can change SDE mid-chain (HW_MTPR ends a block).
    if (target & 1) { // PALmode target needs SDE (shadow remap)
      a.ldrb(a64::w1, a64_cpu_field(a, m_off.sde, 0));
      a.cbz(a64::w1, miss);
    }
    // Cross-page data link: the body cached for this slot is valid while the
    // epoch it was cached in is current (no pointer chase, tag or vgen
    // compare). It stays epoch-guarded because a remap of the target's page
    // leaves this block's own page, and so this block, untouched.
    a.ldr(a64::x2, a64::ptr(a64::x3, off_lepoch + 8 * slot));
    a.ldr(a64::x10, a64::ptr(a64::x28, epoch_rel)); // m_epoch
    a.cmp(a64::x2, a64::x10);
    a.b_ne(miss);
    a.ldr(a64::x1, a64::ptr(a64::x3, off_lbody + 8 * slot));
    a.br(a64::x1); // HIT: tail in (shared frame)
    a.bind(miss);
    // The PC goes to state.pc here, on the miss path, and nowhere on the hot
    // one: a hit tails into the next body, which never reads it, and a
    // 64-bit constant is up to four instructions per exit.
    a.mov(a64::x9, imm(target));
    a.str(a64::x9, a64_cpu_field(a, m_off.state_pc, 3));
    a.orr(a64::x3, a64::x3, imm((uint64_t)(slot + 1)));
    a.str(a64::x3, a64_cpu_field(a, m_off.link_from, 3));
    // The target PC too: the dispatcher caches a data link only into a block
    // with this tag (a pending request can outlive the chain, and a data link
    // has no tag check of its own).
    a.str(a64::x9, a64_cpu_field(a, m_off.link_target, 3));
    a.b(lbl);
  };
#endif
  // Test the branch's register where it lives -- its pin, the value-forward
  // slot, or one load into x0 (which then forwards it) -- and branch to
  // not_taken. Six of eight forms need no flags; BLE/BGT keep a cmp adjacent.
  auto emit_cond_test = [&](int bop, int bra, const Label &not_taken) {
    a64::Gp xr = a64::x0;
    if (bra == 31) {
      a.mov(a64::x0, imm(0)); // R31 reads as zero; keep the one code shape
    } else if (ra.host_of(bra) >= 0) {
      xr = a64::x((uint32_t)ra.host_of(bra));
    } else if (ra.rax_holds == bra) {
      // x0 already holds Ra (the compare feeding a branch is usually the
      // instruction right before it): no reload.
    } else {
      const int idx = (pal_block && ((bra & 0xc) == 0x4)) ? bra + 32 : bra;
      a.ldr(a64::x0, a64::ptr(a64::x20, idx * 8)); // x20 = guest register file
      ra.rax_holds = bra;
    }
    switch (bop) {
    case OP_BEQ: // taken if Ra == 0
      a.cbnz(xr, not_taken);
      break;
    case OP_BNE:
      a.cbz(xr, not_taken);
      break;
    case OP_BLT: // taken if Ra < 0: bit 63
      a.tbz(xr, imm(63), not_taken);
      break;
    case OP_BGE:
      a.tbnz(xr, imm(63), not_taken);
      break;
    case OP_BLBC: // taken if bit 0 clear
      a.tbnz(xr, imm(0), not_taken);
      break;
    case OP_BLBS:
      a.tbz(xr, imm(0), not_taken);
      break;
    case OP_BLE: // the two that need flags keep the cmp adjacent
      a.cmp(xr, imm(0));
      a.b_gt(not_taken);
      break;
    default: // OP_BGT
      a.cmp(xr, imm(0));
      a.b_le(not_taken);
      break;
    }
  };
#ifndef JIT_VERIFY
  Label exit_all = a.new_label(); // every exit's way to the epilogue
  ExitRec *mid_rec = nullptr;     // the in-block exits' records, two a piece
  int mid_slot = 0;
#endif
  // A conditional branch that is not the block's last instruction: its taken
  // side leaves here (count, gate if backward, static exit of its own), its
  // fall-through is simply the next instruction of this block.
  auto emit_mid_exit = [&](uint32_t i) {
    const uint32_t lw = words[i];
    const uint64_t bfall = b->tag + 4 * (uint64_t)(i + 1);
    const int64_t bdisp = (int64_t)((uint64_t)(lw & 0x1FFFFF) << 43) >> 43;
    const uint64_t btgt = bfall + (uint64_t)(bdisp * 4);
    Label not_taken = a.new_label();
    emit_cond_test(m_pending_br_op, m_pending_br_ra, not_taken);
#ifdef JIT_VERIFY
    a.mov(a64::x9, imm(btgt));
    a.str(a64::x9, a64_cpu_field(a, m_off.state_pc, 3));
    a64_add_imm(a, a64::x0, a64::x27, i + 1);
    a.b(done);
#else
    const bool taken_backward = btgt <= b->tag + 4 * (uint64_t)i;
    a64_count_add(a, i + 1);
    Label gate_out = a.new_label();
    const bool stub = taken_backward;
    if (taken_backward)
      a64_emit_gate(a, m_off, gate_out);
    if (mid_slot == kLinkSlots) {
      mid_rec = nullptr;
      mid_slot = 0;
    }
    emit_static_exit(btgt, mid_rec, mid_slot++, exit_all);
    if (stub) {
      a.bind(gate_out);
      a.mov(a64::x9, imm(btgt));
      a.str(a64::x9, a64_cpu_field(a, m_off.state_pc, 3));
      a.b(exit_all);
    }
#endif
    a.bind(not_taken);
    m_pending_br_op = -1;
  };
  m_block_last = plen ? plen - 1 : 0;
  for (uint32_t i = 0; i < plen; ++i) {
    emit_op(&a, nullptr, &done, hs, pal_block, b, words[i], i, ra);
    if (m_pending_br_op >= 0 && i + 1 < plen)
      emit_mid_exit(i);
  }
  m_block_last = ~0u;
  m_defer_branch_pc = false;

  // Epilogue: count this block, then chain (stay native) or return.
  if (terminator_jmp) {
    a64_count_add(a, plen);
#ifndef JIT_VERIFY
    Label exit_chain = a.new_label();
    a64_emit_gate(a, m_off, exit_chain);
    // Inline computed-jump cache (m_ind_cache, beside the epoch counters that
    // x28 points at): hash the target, match tag + current epoch, tail into the
    // body. A PALmode target (SDE gate), a miss or an empty slot takes the
    // jit_indirect helper, which also fills the cache.
    const int32_t ic_rel =
        (int32_t)((char *)&m_ind_cache[0] - (char *)&m_itb_gen);
    if (ic_rel > 0 && (ic_rel % 8) == 0 && ic_rel + 16 <= 32760) {
      Label slow = a.new_label();
      a.tst(a64::x9, imm(1));
      a.b_ne(slow);
      a.lsr(a64::x1, a64::x9, imm(2));
      a.and_(a64::x1, a64::x1, imm((uint64_t)((1u << kIndBits) - 1)));
      a.lsl(a64::x1, a64::x1, imm(5));
      a.add(a64::x1, a64::x28, a64::x1);         // entry - ic_rel
      a.ldr(a64::x2, a64::ptr(a64::x1, ic_rel)); // tag
      a.cmp(a64::x2, a64::x9);
      a.b_ne(slow);
      a.ldr(a64::x2, a64::ptr(a64::x28, epoch_rel)); // m_epoch
      a.ldr(a64::x3, a64::ptr(a64::x1, ic_rel + 8)); // vgen
      a.cmp(a64::x2, a64::x3);
      a.b_ne(slow);
      a.ldr(a64::x0, a64::ptr(a64::x1, ic_rel + 16)); // body
      a.cbz(a64::x0, slow);
      a.br(a64::x0);
      a.bind(slow);
    }
    a.mov(a64::x0, a64::x19); // cpu
    a.mov(a64::x1, a64::x9);  // target == state.pc
    a.mov(a64::x16, imm((uint64_t)hs.indirect_helper));
    if (void *thunk = a64_call_thunk()) {
      a.mov(a64::x17, imm((uint64_t)thunk));
      a.blr(a64::x17); // jit_indirect(cpu, target) -> body | 0
    } else {
      a64_spill_pins(a);
      a.blr(a64::x16);
      a64_reload_pins(a);
    }
    a.cbz(a64::x0, exit_chain);
    a.br(a64::x0);
    a.bind(exit_chain);
#endif
  } else if (terminator_branch) {
    a64_count_add(a, plen); // x9 still holds the next PC
#ifndef JIT_VERIFY
    Label exit_chain = a.new_label();
    const uint32_t lw = words[plen - 1];
    const uint32_t lopc = lw >> 26;
    if (lopc >= 0x30 && lopc <= 0x3f) {
      // PC-relative branch: both successors are constants. BR/BSR have one;
      // the conditional forms left the taken target in x10 (see emit_op), so
      // one compare picks the exit and each exit owns a link slot.
      //
      // Only an exit that can go backward (target <= the branch itself) is
      // gated. Every guest cycle still passes a gated backward branch, a
      // computed jump (JMP/HW_RET) or a return to the dispatcher, so budget
      // overshoot and interrupt latency stay bounded by one forward-only run.
      // The self-loop exit (target == tag) is backward, hence gated.
      const uint64_t bpc = b->tag + 4 * (uint64_t)(plen - 1);
      const uint64_t bfall = b->tag + 4 * (uint64_t)plen;
      const int64_t bdisp = (int64_t)((uint64_t)(lw & 0x1FFFFF) << 43) >> 43;
      const uint64_t btgt = bfall + (uint64_t)(bdisp * 4);
      const bool taken_backward = btgt <= bpc;
      if (lopc == 0x30 || lopc == 0x34) {
        Label gate_out = a.new_label();
        const bool stub = taken_backward;
        if (taken_backward)
          a64_emit_gate(a, m_off, gate_out);
        emit_static_exit(btgt, xrec, 0, exit_chain);
        if (stub) { // the gate's way out: the PC, then leave
          a.bind(gate_out);
          a.mov(a64::x9, imm(btgt));
          a.str(a64::x9, a64_cpu_field(a, m_off.state_pc, 3));
          a.b(exit_chain);
        }
      } else if (m_pending_br_op >= 0) {
        // Test the terminator's register here and branch on it directly. Each
        // side then materialises its own PC -- a compile-time constant --
        // instead of both being built and selected between on the hot path.
        // The register is read where it lives: its pin, or one load from the
        // guest slot (the same load the old shape did at the branch itself).
        Label not_taken = a.new_label();
        emit_cond_test(m_pending_br_op, m_pending_br_ra, not_taken);
        Label gate_out = a.new_label();
        if (taken_backward) // the gate clobbers only x1/x17 (and flags)
          a64_emit_gate(a, m_off, gate_out);
        emit_static_exit(btgt, xrec, 0, exit_chain);
        a.bind(not_taken);
        emit_static_exit(bfall, xrec, 1, exit_chain); // forward: no gate
        if (taken_backward) { // the gate's way out
          a.bind(gate_out);
          a.mov(a64::x9, imm(btgt));
          a.str(a64::x9, a64_cpu_field(a, m_off.state_pc, 3));
          a.b(exit_chain);
        }
      } else {
        Label not_taken = a.new_label();
        a.cmp(a64::x9, a64::x10);
        a.b_ne(not_taken);
        if (taken_backward) // the gate clobbers only x1/x17 (and flags)
          a64_emit_gate(a, m_off, exit_chain);
        emit_static_exit(btgt, xrec, 0, exit_chain);
        a.bind(not_taken);
        emit_static_exit(bfall, xrec, 1, exit_chain); // forward: no gate
      }
    } else {
      a64_emit_gate(a, m_off, exit_chain);
      Label not_self = a.new_label();
      a.mov(a64::x0, imm(b->tag)); // self-loop: straight back into the body
      a.cmp(a64::x9, a64::x0);
      a.b_ne(not_self);
      a.b(body);
      a.bind(not_self);
      emit_chain(exit_chain);
    }
    a.bind(exit_chain);
#endif
  } else {
    a64_count_add(a, plen);
#ifndef JIT_VERIFY
    Label exit_chain = a.new_label();
    // A fall-through only moves forward: no gate (see the branch exits). The
    // PC is written on the exit's miss path, not here.
    emit_static_exit(b->tag + 4 * (uint64_t)plen, xrec, 0, exit_chain);
    a.bind(exit_chain);
#else
    a.mov(a64::x9, imm(b->tag + 4 * (uint64_t)plen)); // fall-through PC
    a.str(a64::x9, a64_cpu_field(a, m_off.state_pc, 3));
#endif
  }
#ifndef JIT_VERIFY
  a.bind(exit_all);
#endif
  a.mov(a64::x0, a64::x27);
  a.bind(done); // bails arrive with x0 already set
  a64_epilogue(a);
  // Cold section: the recorded memory-op slow paths, after the epilogue's ret
  // so nothing falls into them. Each binds its hot-pass label, runs the helper
  // and branches back. A scratch RegAlloc keeps value-forwarding state local.
  {
    RegAlloc cra;
    a64_regalloc(cra);
    m_cold_pass = true;
    for (uint32_t i = 0; i < plen && i < kColdMax; ++i)
      if (m_cold_used[i])
        emit_op(&a, nullptr, &done, hs, pal_block, b, words[i], i, cra);
    m_cold_pass = false;
  }

  const size_t csz = code.code_size();
#ifdef JIT_DISASM
  {
    FILE *out = m_disasm_fp ? m_disasm_fp : stderr;
    fprintf(out, "[JIT][CPU%d] block @ %016llx%s  (%u instr, %llu bytes)\n%s\n",
            m_cpu_id, (unsigned long long)(b->tag & ~(uint64_t)1),
            (b->tag & 1) ? " PAL" : "", plen, (unsigned long long)csz,
            logger.data());
    fflush(out);
  }
#endif
  if (eh.failed)
    return false; // an instruction failed to encode -- don't ship the block
  JitFn fn = nullptr;
  if (!publish_code(&code, (void **)&fn))
    return false;
  *out_fn = fn;
  *out_body_off = (uint32_t)body_off;
  *out_csz = csz;
  return true;
}

bool CJitEngine::assemble_trace(JitBlock **blocks, uint32_t n_blocks,
                                const uint8_t *dram, const HelperSet &hs,
                                JitFn *out_fn, size_t *out_csz) {
  using namespace asmjit;
  CodeHolder code;
  if (code.init(((JitRuntime *)m_rt)->environment()) != Error::kOk)
    return false;
  A64EmitErrors eh;
  eh.cpu_id = m_cpu_id;
  code.set_error_handler(&eh);
  a64::Assembler a(&code);

  a64_prologue(a, (uint64_t)&m_itb_gen);
  Label done = a.new_label();
  Label body = a.new_label(); // loop re-entry (pins + count stay live)
  a.bind(body);

  RegAlloc ra;
  a64_regalloc(ra);
  m_cold_pass = false;
  for (uint32_t i = 0; i < kColdMax; ++i)
    m_cold_used[i] = false;
  uint32_t cold_base = 0;
  for (uint32_t bi = 0; bi < n_blocks; ++bi) {
    JitBlock *b = blocks[bi];
    const uint32_t plen = b->prefix_len;
    m_cold_base = cold_base; // segment-relative cold index base
    cold_base += plen;
    const uint32_t *words = (const uint32_t *)(dram + b->phys);
    const bool pal_block = (b->tag & 1) != 0;
    // Default next PC = the sequential successor (terminators overwrite it).
    a.mov(a64::x9, imm(b->tag + 4 * (uint64_t)plen));
    a.str(a64::x9, a64_cpu_field(a, m_off.state_pc, 3));
    for (uint32_t i = 0; i < plen; ++i)
      emit_op(&a, nullptr, &done, hs, pal_block, b, words[i], i, ra);
    a64_count_add(a, plen);
    a.mov(a64::x0, a64::x27); // instrs completed so far (preset for done)
    if (bi + 1 < n_blocks) {
      // Guard: did this block flow into the next fused block?
      a.mov(a64::x1, imm(blocks[bi + 1]->tag));
      a.cmp(a64::x9, a64::x1);
      a.b_ne(done);
    }
  }
#ifndef JIT_VERIFY
  // Loop closure (see the x86 emitter): back-edge to the head, gated.
  {
    JitBlock *lb = blocks[n_blocks - 1];
    const uint32_t *lw = (const uint32_t *)(dram + lb->phys);
    const uint32_t lop = lw[lb->prefix_len - 1], lopc = lop >> 26;
    if (lopc == 0x30 || lopc == 0x34 || (lopc >= 0x38 && lopc <= 0x3f)) {
      const int64_t disp = (int64_t)((uint64_t)(lop & 0x1FFFFF) << 43) >> 43;
      const uint64_t tgt =
          (((lb->tag & ~(uint64_t)1) + 4 * (uint64_t)(lb->prefix_len - 1)) + 4 +
           (uint64_t)(disp * 4)) |
          (lb->tag & 1);
      if (tgt == blocks[0]->tag) {
        a.mov(a64::x1, imm(blocks[0]->tag));
        a.cmp(a64::x9, a64::x1);
        a.b_ne(done);
        a.ldr(a64::x1, a64_cpu_field(a, m_off.jit_budget, 3));
        a.cmp(a64::x0, a64::x1);
        a.b_ge(done);
        a.ldrb(a64::w1, a64_cpu_field(a, m_off.check_int, 0));
        a.cbnz(a64::w1, done);
        a.ldrb(a64::w1, a64_cpu_field(a, m_off.check_timers, 0));
        a.cbnz(a64::w1, done);
        a.b(body);
      }
    }
  }
#endif
  a.bind(done);
  a64_epilogue(a);
  // Cold section for the fused segments (see assemble_block).
  {
    RegAlloc cra;
    a64_regalloc(cra);
    m_cold_pass = true;
    uint32_t base = 0;
    for (uint32_t bi = 0; bi < n_blocks; ++bi) {
      JitBlock *b = blocks[bi];
      const uint32_t plen = b->prefix_len;
      const uint32_t *words = (const uint32_t *)(dram + b->phys);
      m_cold_base = base;
      for (uint32_t i = 0; i < plen && base + i < kColdMax; ++i)
        if (m_cold_used[base + i])
          emit_op(&a, nullptr, &done, hs, (b->tag & 1) != 0, b, words[i], i,
                  cra);
      base += plen;
    }
    m_cold_pass = false;
    m_cold_base = 0;
  }

  if (eh.failed)
    return false;
  const size_t csz = code.code_size();
  JitFn fn = nullptr;
  if (!publish_code(&code, (void **)&fn))
    return false;
  *out_fn = fn;
  *out_csz = csz;
  return true;
}

#endif // INCLUDED_JITEMIT_A64_H
