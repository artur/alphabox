/* AXPbox -- JIT engine, AArch64 backend.
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
  if (((JitRuntime *)m_rt)->add(&fn, &code) != Error::kOk)
    return nullptr;
  m_call_thunk = (void *)fn;
  return m_call_thunk;
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
    // MISC barriers (TRAPB/EXCB/MB/WMB) and prefetch/cache hints: AArch64 has
    // no store-serializing instruction like x86 mfence, so both emit nothing
    // and the block extends straight past them.
    if (op == OP_NOP || op == OP_MFENCE)
      continue;

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
      if (rb == 31)
        a.mov(x2, imm(disp));
      else {
        mov_from_reg(x2, rb);
        if (disp)
          a64_add_imm(a, x2, x2, disp);
      }
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
    // x2 = va. On a hit: x10 = host page base, x11 = page offset. Misses (slot
    // tag, {cm,asn0}, MMIO/none) branch to slow. Clobbers x10-x12.
    const uint32_t dpc_cm_rel = m_off.dpc_cm - m_off.dpc_virt_page;
    const uint32_t dpc_host_rel = m_off.dpc_host_base - m_off.dpc_virt_page;
    // {cm, asn0} key: one unscaled load off x20 (production x20 = state.r[0])
    // when state.cm sits within its +-256 window, else via [cpu + off].
    const int32_t key_rel = (int32_t)m_off.state_cm - (int32_t)m_off.regs;
    const bool key_via_regs = key_rel >= -256 && key_rel <= 255;
    auto dpc_probe = [&](bool write_row, const Label &slow) {
      const uint32_t row =
          m_off.dpc_virt_page + (write_row ? m_off.dpc_write_row : 0);
      a.lsr(x10, x2, imm(13));
      a.and_(x10, x10, imm((uint64_t)m_off.dpc_mask));
      int32_t base = 0; // row offset still to add in the field loads
      if (m_off.dpc_stride == 40 && (row % 8) == 0 &&
          row + dpc_host_rel <= 32760 && row + dpc_cm_rel <= 32760) {
        // x10 = cpu + idx * 40 via two shifted adds (was mov/mul/mov/add/add);
        // the row base folds into each field load's displacement.
        a.add(x11, x10, x10, a64::lsl(2)); // idx * 5
        a.add(x10, kCpu, x11, a64::lsl(3));
        base = (int32_t)row;
      } else {
        a.mov(x11, imm(m_off.dpc_stride));
        a.mul(x10, x10, x11);
        a.mov(x11, imm((uint64_t)row));
        a.add(x10, x10, x11);
        a.add(x10, kCpu, x10); // x10 = &data_page_cache[row][dpc_index(va)]
      }
      a.and_(x11, x2, imm(~(uint64_t)0x1FFF));
      a.ldr(x12, a64::ptr(x10, base)); // slot virt_page
      a.cmp(x12, x11);
      a.b_ne(slow);
      if (key_via_regs) // {cm, asn0} (adjacent in state)
        a.ldur(x11, a64::ptr(kRegs, key_rel));
      else
        a.ldr(x11, fld(m_off.state_cm, 3));
      a.ldr(x12, a64::ptr(x10, base + (int32_t)dpc_cm_rel)); // slot {cm, asn}
      a.cmp(x12, x11);
      a.b_ne(slow);
      a.ldr(x10, a64::ptr(x10, base + (int32_t)dpc_host_rel)); // 0 = MMIO
      a.cbz(x10, slow);
      a.and_(x11, x2, imm(0x1FFF));
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
        if (size_bits == 64)
          a.ldr(x0, m);
        else if (size_bits == 32)
          a.ldrsw(x0, m);
        else if (size_bits == 16)
          a.ldrh(w0, m);
        else
          a.ldrb(w0, m);
        mov_to_reg(ra, x0);
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
      load_from(a64::ptr(x10, x11));
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
      const a64::Mem m = a64::ptr(x10, x11);
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
        a.ldr(x0, a64::ptr(x10, x11));
        a.str(x0, fld(m_off.f_base + (uint32_t)fa * 8, 3));
      } else {
        a.ldr(x12, fld(m_off.f_base + (uint32_t)fa * 8, 3));
        a.str(x12, a64::ptr(x10, x11));
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
      // checks this path too. AXPBOX_IRQTRACE keeps the helper (it logs).
      static const bool irqtrace = getenv("AXPBOX_IRQTRACE") != nullptr;
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
        a.mov(x9, imm(tgt));
      } else {
        if (ra == 31)
          a.mov(x0, imm(0));
        else
          mov_from_reg(x0, ra);
        a.mov(x9, imm(fall));
        a.mov(x10, imm(tgt));
        if (op == OP_BLBC || op == OP_BLBS)
          a.tst(x0, imm(1));
        else
          a.tst(x0, x0);
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

    case OP_AMASK: // Rc = op2 & ~CPU_AMASK (keep in sync w/ cpu_defs.h)
      op2_x1();
      a.mov(x9, imm(0x1307));
      a.bic(x0, x1, x9);
      break;
    case OP_IMPLVER: // Rc = CPU_IMPLVER (keep in sync w/ cpu_defs.h)
      a.mov(x0, imm(2));
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
  for (uint32_t i = 0; i < plen; ++i)
    emit_op(&a, nullptr, &done, hs, pal_block, b, words[i], i, ra);

  // Epilogue: count this block, then chain (stay native) or return.
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
  auto emit_static_exit = [&](uint64_t target, int slot, const Label &lbl) {
    if (target == b->tag) {
      a.b(body); // self-loop (the gate already ran)
      return;
    }
    // AXPBOX_JIT_NO_DLINK=1: the tag-checked scan exit instead (A/B switch).
    static const bool no_dlink = getenv("AXPBOX_JIT_NO_DLINK") != nullptr;
    if (no_dlink) {
      emit_chain(lbl);
      a.b(lbl);
      return;
    }
    Label miss = a.new_label();
    if (!xrec)
      xrec = alloc_exit_rec();
    a.mov(a64::x3, imm((uint64_t)xrec)); // this code's exit record
    if (target & 1) { // PALmode target needs SDE (shadow remap)
      a.ldrb(a64::w1, a64_cpu_field(a, m_off.sde, 0));
      a.cbz(a64::w1, miss);
    }
    // Data link: the body cached for this slot is valid while the epoch it was
    // cached in is current (no pointer chase, tag or vgen compare).
    const int32_t off_lbody = (int32_t)offsetof(ExitRec, body);
    const int32_t off_lepoch = (int32_t)offsetof(ExitRec, epoch);
    a.ldr(a64::x2, a64::ptr(a64::x3, off_lepoch + 8 * slot));
    a.ldr(a64::x10, a64::ptr(a64::x28, epoch_rel)); // m_epoch
    a.cmp(a64::x2, a64::x10);
    a.b_ne(miss);
    a.ldr(a64::x1, a64::ptr(a64::x3, off_lbody + 8 * slot));
    a.br(a64::x1); // HIT: tail in (shared frame)
    a.bind(miss);
    a.orr(a64::x3, a64::x3, imm((uint64_t)(slot + 1)));
    a.str(a64::x3, a64_cpu_field(a, m_off.link_from, 3));
    // The target PC too: the dispatcher caches a data link only into a block
    // with this tag (a pending request can outlive the chain, and a data link
    // has no tag check of its own).
    a.str(a64::x9, a64_cpu_field(a, m_off.link_target, 3));
    a.b(lbl);
  };
#endif
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
    a64_emit_gate(a, m_off, exit_chain);
    const uint32_t lw = words[plen - 1];
    const uint32_t lopc = lw >> 26;
    if (lopc >= 0x30 && lopc <= 0x3f) {
      // PC-relative branch: both successors are constants. BR/BSR have one;
      // the conditional forms left the taken target in x10 (see emit_op), so
      // one compare picks the exit and each exit owns a link slot.
      const uint64_t bfall = b->tag + 4 * (uint64_t)plen;
      const int64_t bdisp = (int64_t)((uint64_t)(lw & 0x1FFFFF) << 43) >> 43;
      const uint64_t btgt = bfall + (uint64_t)(bdisp * 4);
      if (lopc == 0x30 || lopc == 0x34) {
        emit_static_exit(btgt, 0, exit_chain);
      } else {
        Label not_taken = a.new_label();
        a.cmp(a64::x9, a64::x10);
        a.b_ne(not_taken);
        emit_static_exit(btgt, 0, exit_chain);
        a.bind(not_taken);
        emit_static_exit(bfall, 1, exit_chain);
      }
    } else {
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
    a.mov(a64::x9, imm(b->tag + 4 * (uint64_t)plen)); // fall-through PC
    a.str(a64::x9, a64_cpu_field(a, m_off.state_pc, 3));
    a64_count_add(a, plen);
#ifndef JIT_VERIFY
    Label exit_chain = a.new_label();
    a64_emit_gate(a, m_off, exit_chain);
    emit_static_exit(b->tag + 4 * (uint64_t)plen, 0, exit_chain);
    a.bind(exit_chain);
#endif
  }
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
  if (((JitRuntime *)m_rt)->add(&fn, &code) != Error::kOk)
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
  if (((JitRuntime *)m_rt)->add(&fn, &code) != Error::kOk)
    return false;
  *out_fn = fn;
  *out_csz = csz;
  return true;
}

#endif // INCLUDED_JITEMIT_A64_H
