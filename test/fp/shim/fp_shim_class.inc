/* The part of CAlphaCPU that the floating-point routines use. No include
   guard: fp_frozen.cpp includes it a second time inside namespace frozen, so
   the routines as they were before the fixes compile beside the current
   ones. Member names and signatures follow src/cpu/AlphaCPU.hpp. */
class CAlphaCPU {
public:
  struct {
    u64 f[64];
    u64 r[64];
    u64 fpcr;
    u64 exc_sum;
    u64 exc_addr;
    u64 pc;
    u64 current_pc;
    u64 pal_base;
    int iProcNum;
    int cm;
    bool fpen;
    bool sde;
  } state;
  FpShimSystem sys;
  FpShimSystem *cSystem = &sys;
  int pal_entries = 0; /* GO_PAL calls in this instruction */
  /* The native EV8's trap path (GO_PAL, ARITH_TRAP): never taken here. */
  static constexpr bool m_ev8n = false;
  void ev8_go_pal(u64) {}
  u64 ev8_regs_of(u32) const { return 0; }
  bool opcdec = false; /* the decode refused the instruction */

  void set_pc(u64 p_pc) {
    state.pc = p_pc;
    pal_entries++;
  }

  /* One floating-point operate instruction (opcodes 0x14 to 0x17) through
     the DO_ macros, decoded as AlphaCPU.cpp's execute() decodes it. */
  void fp_exec(u32 ins);

  void write_fpcr_arch(u64 arch_val);
  u64 read_fpcr_arch() const;

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
};
