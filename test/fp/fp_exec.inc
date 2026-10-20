/* The floating-point operate decode, shared by fp_exec_current.cpp and
   fp_frozen.cpp: each includes it after its own version of the DO_ macros
   (cpu_fp_operate.hpp). No include guard.

   The decode of opcodes 0x14 to 0x17 is a copy of the one in
   CAlphaCPU::execute() (AlphaCPU.cpp); keep the two in step. The FPCR
   accessors are copies of the ones there too. */

void CAlphaCPU::write_fpcr_arch(u64 arch_val) {
  u64 val = arch_val & ~(FPCR_RAZ | U64(0x00000000FFFFFFFF));

  if (val & FPCR_ERR)
    val |= FPCR_SUM;
  else
    val &= ~FPCR_SUM;

  state.fpcr = val;
}

u64 CAlphaCPU::read_fpcr_arch() const {
  u64 val = state.fpcr & ~(FPCR_RAZ | U64(0x00000000FFFFFFFF));

  if (val & FPCR_ERR)
    val |= FPCR_SUM;
  else
    val &= ~FPCR_SUM;

  return val;
}

/* FPSTART's `break` leaves the do/while; the instruction is then done. */
#define FP_OP(name)                                                            \
  do {                                                                         \
    DO_##name;                                                                 \
  } while (0);                                                                 \
  return
#define FP_UNKNOWN                                                             \
  do {                                                                         \
    opcdec = true;                                                             \
    return;                                                                    \
  } while (0)

void CAlphaCPU::fp_exec(u32 ins) {
  UFP ufp2;
  const u32 function = (ins >> 5) & 0x7ff;

  switch (I_GETOP(ins)) {
  case 0x14: // ITFP* instructions
    switch (function) {
    case 0x004:
      FP_OP(ITOFS);
    case 0x00a:
    case 0x08a:
    case 0x10a:
    case 0x18a:
    case 0x40a:
    case 0x48a:
    case 0x50a:
    case 0x58a:
      FP_OP(SQRTF);
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
      FP_OP(SQRTS);
    case 0x014:
      FP_OP(ITOFF);
    case 0x024:
      FP_OP(ITOFT);
    case 0x02a:
    case 0x0aa:
    case 0x12a:
    case 0x1aa:
    case 0x42a:
    case 0x4aa:
    case 0x52a:
    case 0x5aa:
      FP_OP(SQRTG);
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
      FP_OP(SQRTT);
    default:
      FP_UNKNOWN;
    }

  case 0x15: // FLTV* instructions
    switch (function) {
    case 0x0a5:
    case 0x4a5:
      FP_OP(CMPGEQ);
    case 0x0a6:
    case 0x4a6:
      FP_OP(CMPGLT);
    case 0x0a7:
    case 0x4a7:
      FP_OP(CMPGLE);
    case 0x03c:
    case 0x0bc:
      FP_OP(CVTQF);
    case 0x03e:
    case 0x0be:
      FP_OP(CVTQG);
    default:
      if (function & 0x200)
        FP_UNKNOWN;
      switch (function & 0x7f) {
      case 0x000:
        FP_OP(ADDF);
      case 0x001:
        FP_OP(SUBF);
      case 0x002:
        FP_OP(MULF);
      case 0x003:
        FP_OP(DIVF);
      case 0x01e:
        FP_OP(CVTDG);
      case 0x020:
        FP_OP(ADDG);
      case 0x021:
        FP_OP(SUBG);
      case 0x022:
        FP_OP(MULG);
      case 0x023:
        FP_OP(DIVG);
      case 0x02c:
        FP_OP(CVTGF);
      case 0x02d:
        FP_OP(CVTGD);
      case 0x02f:
        FP_OP(CVTGQ);
      default:
        FP_UNKNOWN;
      }
    }

  case 0x16: // FLTI* instructions
    switch (function) {
    case 0x0a4:
    case 0x5a4:
      FP_OP(CMPTUN);
    case 0x0a5:
    case 0x5a5:
      FP_OP(CMPTEQ);
    case 0x0a6:
    case 0x5a6:
      FP_OP(CMPTLT);
    case 0x0a7:
    case 0x5a7:
      FP_OP(CMPTLE);
    case 0x2ac:
    case 0x6ac:
      FP_OP(CVTST);
    default:
      if (((function & 0x600) == 0x200) || ((function & 0x500) == 0x400))
        FP_UNKNOWN;
      switch (function & 0x3f) {
      case 0x00:
        FP_OP(ADDS);
      case 0x01:
        FP_OP(SUBS);
      case 0x02:
        FP_OP(MULS);
      case 0x03:
        FP_OP(DIVS);
      case 0x20:
        FP_OP(ADDT);
      case 0x21:
        FP_OP(SUBT);
      case 0x22:
        FP_OP(MULT);
      case 0x23:
        FP_OP(DIVT);
      case 0x2c:
        FP_OP(CVTTS);
      case 0x2f:
        FP_OP(CVTTQ);
      case 0x3c:
        if ((function & 0x300) == 0x100)
          FP_UNKNOWN;
        FP_OP(CVTQS);
      case 0x3e:
        if ((function & 0x300) == 0x100)
          FP_UNKNOWN;
        FP_OP(CVTQT);
      default:
        FP_UNKNOWN;
      }
    }

  case 0x17: // FLTL* instructions
    switch (function) {
    case 0x010:
      FP_OP(CVTLQ);
    case 0x020:
      FP_OP(CPYS);
    case 0x021:
      FP_OP(CPYSN);
    case 0x022:
      FP_OP(CPYSE);
    case 0x024:
      FP_OP(MT_FPCR);
    case 0x025:
      FP_OP(MF_FPCR);
    case 0x02a:
      FP_OP(FCMOVEQ);
    case 0x02b:
      FP_OP(FCMOVNE);
    case 0x02c:
      FP_OP(FCMOVLT);
    case 0x02d:
      FP_OP(FCMOVGE);
    case 0x02e:
      FP_OP(FCMOVLE);
    case 0x02f:
      FP_OP(FCMOVGT);
    case 0x030:
    case 0x130:
    case 0x530:
      FP_OP(CVTQL);
    default:
      FP_UNKNOWN;
    }

  default:
    FP_UNKNOWN;
  }
}

#undef FP_OP
#undef FP_UNKNOWN
