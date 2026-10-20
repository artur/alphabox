/* Alphabox Alpha Emulator
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
 */

/**
 * \file
 * alphabox_fptest: the interpreter's floating-point instructions, checked
 * without a guest.
 *
 * Every IEEE and VAX operate instruction runs through the interpreter's own
 * routines (AlphaCPU_ieeefloat.cpp, AlphaCPU_vaxfloat.cpp and the DO_ macros,
 * compiled unchanged against test/fp/shim) and is compared, result word,
 * FPCR and EXC_SUM, with a model of what the architecture asks for:
 *
 *  - IEEE S and T: the host FPU gives the rounded result and its flags in all
 *    four rounding modes; the Alpha rules for traps, sticky bits, trap
 *    disables, denormal operands, NaN propagation and underflow are applied
 *    on top (ieee_model).
 *  - VAX F, G and D: an exact integer model rounds each true result
 *    (vax_model).
 *  - Hand tables of cases taken from the manuals, each with its citation.
 *
 * The same inputs also run through the routines as they were at the
 * baseline commit (fp_frozen.cpp), and every difference between the two must
 * fall under one of the items in `items` below, which is the list of
 * intended changes, each with its reason. A mismatch with the model is
 * allowed only for an item not fixed yet ("expected"); a fixed item that
 * still mismatches, an unexplained difference from the frozen code, or a
 * mismatch no item explains fails the test.
 *
 * Usage: alphabox_fptest [-v] [-n <random cases per variant>]
 **/
#include "AlphaCPU.hpp"
#include "fp_host.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#if defined(FP_HAVE_FROZEN)
namespace frozen {
#include "fp_shim_class.inc"
}
#endif

typedef unsigned __int128 u128;

/* ------------------------------------------------------------------------ */
/* The items: every intended difference from the baseline, and every known  */
/* gap between the code and the model.                                     */
/* ------------------------------------------------------------------------ */

struct Item {
  const char *id;
  bool fixed; /* the code implements the model for this item */
  const char *what;
  long model_mismatch = 0;   /* current code vs model, while not fixed */
  long frozen_explained = 0; /* frozen vs current differences it explains */
};

/* Citations: "ARM" is the Alpha Architecture Reference Manual, 4th edition
   (Compaq, January 2002); "HRM" is the Alpha 21264/EV68CB and EV68DC
   Hardware Reference Manual (DS-0031C-TE). */
static Item items[] = {
    {"FP-01", false,
     "VAX F results keep the bits below F precision, <28:0>; a single "
     "precision value in a register has them clear (ARM 4.7.1, p. 4-63)"},
    {"FP-03", false,
     "a VAX dirty zero operand takes an invalid operation trap; with /S it "
     "is treated as zero (ARM 4.7.7.1, p. 4-70 and 4-71)"},
    {"FP-04", false,
     "CMPGxx/S compares values, so a dirty zero equals zero (ARM 4.7.7.1; "
     "4.10.x VAX Floating Compare, p. 4-111)"},
    {"FP-05", false,
     "CVTTQ overflow is an invalid operation only with /S (HRM Table A-11, "
     "p. A-17); without /S it is an integer overflow only (ARM 4.7.7.2, "
     "p. 4-72; 4.7.7.9, p. 4-79)"},
    {"FP-06", false,
     "FPCR[DNZ] acts only for instructions with /S (ARM 4.7.7.11, p. 4-79)"},
    {"FP-10", false,
     "a VAX overflow writes the largest number of the format, not the "
     "rounded fraction under a clamped exponent (the result is "
     "UNPREDICTABLE, ARM 4.7.7.1)"},
    {"FP-11", false,
     "IEEE underflow is decided on the result rounded to the destination "
     "format: a true result that rounds to MIN is not an underflow (ARM "
     "4.7.5, p. 4-67)"},
    {"FP-12", false,
     "VAX CVTGQ of exactly -2**63 is in range, not an integer overflow (ARM "
     "4.7.7.9, p. 4-79)"},
    {"DENORM", false,
     "with /S, FPCR[UNFD] set and [UNDZ] clear, an underflow gives the IEEE "
     "denormal result instead of a true zero (ARM 4.7.7.10 and 4.7.7.11, "
     "p. 4-79; Table 4-11 bit 61)"},
};

static Item *item(const char *id) {
  for (Item &it : items)
    if (!strcmp(it.id, id))
      return &it;
  fprintf(stderr, "fptest: unknown item %s\n", id);
  exit(2);
}

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */

static const u64 SIGN = U64(0x8000000000000000);
static const u64 QNANBIT = U64(0x0008000000000000);
static const u64 CQNANV = U64(0xFFF8000000000000);
static const u64 FTRUE = U64(0x4000000000000000);
static const u64 FRAC52 = U64(0x000FFFFFFFFFFFFF);
static const u64 LOW29 = U64(0x000000001FFFFFFF);
/* EXC_SUM / FPCR-status bits, in EXC_SUM<6:1> order */
static const u64 X_INV = 0x02, X_DZE = 0x04, X_OVF = 0x08, X_UNF = 0x10,
                 X_INE = 0x20, X_IOV = 0x40, X_ALL = 0x7e;
static const u64 SENTINEL = U64(0x5EA1ED0DDEADBEEF);
static const u64 FPCR_N = U64(2) << 58; /* DYN = normal */

/* ------------------------------------------------------------------------ */
/* Running one instruction                                                  */
/* ------------------------------------------------------------------------ */

struct Outcome {
  u64 res;
  u64 fpcr;
  u64 exc;
  bool trapped;
  bool opcdec;
  bool operator==(const Outcome &o) const {
    return res == o.res && fpcr == o.fpcr && exc == o.exc &&
           trapped == o.trapped && opcdec == o.opcdec;
  }
};

template <class CPU> static Outcome run(u32 ins, u64 a, u64 b, u64 fpcr) {
  static CPU cpu;
  memset(&cpu.state, 0, sizeof cpu.state);
  cpu.state.fpen = true;
  cpu.state.pal_base = U64(0x8000);
  cpu.state.pc = cpu.state.current_pc = U64(0x20000);
  cpu.state.f[1] = a;
  cpu.state.f[2] = b;
  cpu.state.f[3] = SENTINEL;
  cpu.state.fpcr = fpcr;
  cpu.state.exc_sum = U64(0x5A5A); /* FPSTART clears it */
  cpu.pal_entries = 0;
  cpu.opcdec = false;
  cpu.fp_exec(ins);
  return {cpu.state.f[3], cpu.state.fpcr, cpu.state.exc_sum,
          cpu.pal_entries > 0, cpu.opcdec};
}

/* ------------------------------------------------------------------------ */
/* Instructions                                                             */
/* ------------------------------------------------------------------------ */

enum Kind {
  K_ADD,
  K_SUB,
  K_MUL,
  K_DIV,
  K_SQRT,
  K_CVTTS,
  K_CVTST,
  K_CVTTQ,
  K_CVTQS,
  K_CVTQT,
  K_CMPTEQ,
  K_CMPTLT,
  K_CMPTLE,
  K_CMPTUN,
  V_ADD,
  V_SUB,
  V_MUL,
  V_DIV,
  V_SQRT,
  V_CVTGF,
  V_CVTDG,
  V_CVTGD,
  V_CVTGQ,
  V_CVTQF,
  V_CVTQG,
  V_CMPEQ,
  V_CMPLT,
  V_CMPLE
};

enum Fmt { F_T, F_S, F_F, F_G, F_D, F_Q };

struct Op {
  const char *name;
  Kind k;
  u32 opc, base;
  Fmt src, dst;
  bool unary; /* Fa must be F31 */
  std::vector<u32> rnds, traps;
  bool vax() const { return k >= V_ADD; }
};

static const std::vector<u32> R4 = {0x000, 0x040, 0x080, 0x0c0}; /* C M N D */
static const std::vector<u32> RV = {0x000, 0x080};               /* C N */
static const std::vector<u32> T4 = {0x000, 0x100, 0x500,
                                    0x700}; /* - U SU SUI */
static const std::vector<u32> TV = {0x000, 0x100, 0x400, 0x500}; /* - U S SU */

static std::vector<Op> make_ops() {
  std::vector<Op> v;
  const struct {
    const char *n;
    Kind k;
    u32 base;
  } ar[] = {{"ADD", K_ADD, 0x00},
            {"SUB", K_SUB, 0x01},
            {"MUL", K_MUL, 0x02},
            {"DIV", K_DIV, 0x03}};
  for (const auto &a : ar) {
    v.push_back({strdup((std::string(a.n) + "S").c_str()), a.k, 0x16, a.base,
                 F_S, F_S, false, R4, T4});
    v.push_back({strdup((std::string(a.n) + "T").c_str()), a.k, 0x16,
                 a.base | 0x20, F_T, F_T, false, R4, T4});
  }
  v.push_back({"SQRTS", K_SQRT, 0x14, 0x00b, F_S, F_S, true, R4, T4});
  v.push_back({"SQRTT", K_SQRT, 0x14, 0x02b, F_T, F_T, true, R4, T4});
  v.push_back({"CVTTS", K_CVTTS, 0x16, 0x02c, F_T, F_S, true, R4, T4});
  v.push_back({"CVTST", K_CVTST, 0x16, 0x2ac, F_S, F_T, true, {0}, {0, 0x400}});
  v.push_back({"CVTTQ", K_CVTTQ, 0x16, 0x02f, F_T, F_Q, true, R4, T4});
  v.push_back({"CVTQS", K_CVTQS, 0x16, 0x03c, F_Q, F_S, true, R4, {0, 0x700}});
  v.push_back({"CVTQT", K_CVTQT, 0x16, 0x03e, F_Q, F_T, true, R4, {0, 0x700}});
  v.push_back(
      {"CMPTEQ", K_CMPTEQ, 0x16, 0x0a5, F_T, F_T, false, {0}, {0, 0x500}});
  v.push_back(
      {"CMPTLT", K_CMPTLT, 0x16, 0x0a6, F_T, F_T, false, {0}, {0, 0x500}});
  v.push_back(
      {"CMPTLE", K_CMPTLE, 0x16, 0x0a7, F_T, F_T, false, {0}, {0, 0x500}});
  v.push_back(
      {"CMPTUN", K_CMPTUN, 0x16, 0x0a4, F_T, F_T, false, {0}, {0, 0x500}});
  const struct {
    const char *n;
    Kind k;
    u32 base;
  } vr[] = {{"ADD", V_ADD, 0x00},
            {"SUB", V_SUB, 0x01},
            {"MUL", V_MUL, 0x02},
            {"DIV", V_DIV, 0x03}};
  for (const auto &a : vr) {
    v.push_back({strdup((std::string(a.n) + "F").c_str()), a.k, 0x15, a.base,
                 F_F, F_F, false, RV, TV});
    v.push_back({strdup((std::string(a.n) + "G").c_str()), a.k, 0x15,
                 a.base | 0x20, F_G, F_G, false, RV, TV});
  }
  v.push_back({"SQRTF", V_SQRT, 0x14, 0x00a, F_F, F_F, true, RV, TV});
  v.push_back({"SQRTG", V_SQRT, 0x14, 0x02a, F_G, F_G, true, RV, TV});
  v.push_back({"CVTGF", V_CVTGF, 0x15, 0x02c, F_G, F_F, true, RV, TV});
  v.push_back({"CVTDG", V_CVTDG, 0x15, 0x01e, F_D, F_G, true, RV, TV});
  v.push_back({"CVTGD", V_CVTGD, 0x15, 0x02d, F_G, F_D, true, RV, TV});
  v.push_back({"CVTGQ", V_CVTGQ, 0x15, 0x02f, F_G, F_Q, true, RV, TV});
  v.push_back({"CVTQF", V_CVTQF, 0x15, 0x03c, F_Q, F_F, true, RV, {0}});
  v.push_back({"CVTQG", V_CVTQG, 0x15, 0x03e, F_Q, F_G, true, RV, {0}});
  v.push_back(
      {"CMPGEQ", V_CMPEQ, 0x15, 0x0a5, F_G, F_G, false, {0}, {0, 0x400}});
  v.push_back(
      {"CMPGLT", V_CMPLT, 0x15, 0x0a6, F_G, F_G, false, {0}, {0, 0x400}});
  v.push_back(
      {"CMPGLE", V_CMPLE, 0x15, 0x0a7, F_G, F_G, false, {0}, {0, 0x400}});
  return v;
}

static u32 encode(const Op &op, u32 fn) {
  const u32 fa = op.unary ? 31 : 1;
  return (op.opc << 26) | (fa << 21) | (2u << 16) | (fn << 5) | 3;
}

/* ------------------------------------------------------------------------ */
/* Register formats                                                         */
/* ------------------------------------------------------------------------ */

/* LDS: S memory word to register (ARM 4.8.3, MAP_S). */
static u64 s_to_reg(u32 f) {
  const u64 sign = (u64)(f >> 31) << 63;
  const u32 e = (f >> 23) & 0xff;
  const u64 m = f & 0x7fffff;
  const u64 re = e == 0 ? 0 : e == 0xff ? 0x7ff : (u64)e + 896;
  return sign | (re << 52) | (m << 29);
}

/* STS: register to S memory word (ARM 4.8.7). */
static u32 reg_to_s(u64 r) {
  return (u32)((((r >> 62) & 3) << 30) | ((r >> 29) & 0x3fffffff));
}

/* LDF: F memory word to register (ARM 4.8.1, MAP_F). */
static u64 f_to_reg(u32 w) {
  const u64 sign = (u64)((w >> 15) & 1) << 63;
  const u32 e = (w >> 7) & 0xff;
  const u64 re = e == 0 ? 0 : (u64)e + 896;
  const u64 frac = ((u64)(w & 0x7f) << 16) | (w >> 16);
  return sign | (re << 52) | (frac << 29);
}

enum Cls { C_ZERO, C_DEN, C_FIN, C_INF, C_QNAN, C_SNAN };

static Cls cls_t(u64 x) {
  const u32 e = (x >> 52) & 0x7ff;
  const u64 f = x & FRAC52;
  if (e == 0)
    return f ? C_DEN : C_ZERO;
  if (e == 0x7ff)
    return f == 0 ? C_INF : ((f & QNANBIT) ? C_QNAN : C_SNAN);
  return C_FIN;
}
static bool is_nan(Cls c) { return c == C_QNAN || c == C_SNAN; }

/* ------------------------------------------------------------------------ */
/* Expected outcome                                                         */
/* ------------------------------------------------------------------------ */

struct Expect {
  u64 res = 0;
  bool res_known = true;
  u64 det = 0;  /* exceptions detected (FPCR status bits, IEEE only) */
  u64 trap = 0; /* exceptions whose trap is taken */
  u64 dc = 0; /* exceptions that may or may not show (after an operand trap) */
};

struct Ctx {
  u32 ins;
  u64 fpcr;
  bool S, U, SUI;
  int rnd;
  bool dnz;
  bool vax;
};

static Ctx ctx_of(u32 ins, u64 fpcr, bool vax) {
  Ctx c;
  c.ins = ins;
  c.fpcr = fpcr;
  c.S = (ins & 0x8000) != 0;
  c.U = (ins & 0x2000) != 0;
  c.SUI = ((ins >> 13) & 7) == 7;
  c.rnd = (ins >> 11) & 3;
  if (c.rnd == 3)
    c.rnd = (int)((fpcr >> 58) & 3);
  c.dnz = (fpcr & FPCR_DNZ) && c.S;
  c.vax = vax;
  return c;
}

/* The outcome an Expect stands for: FPCR gets the detected status bits;
   EXC_SUM gets the taken traps, SET_xxx (bits 47:42) for status bits newly
   set, SWC for /S and the destination register (HRM Table 5-9). */
static Outcome derive(const Ctx &c, const Expect &e, u64 res) {
  Outcome o;
  o.res = res;
  o.opcdec = false;
  u64 bits = e.trap;
  if (c.vax) {
    o.fpcr = c.fpcr;
  } else {
    o.fpcr = c.fpcr | (e.det << 51);
    bits |= (e.det & ~((c.fpcr >> 51) & X_ALL)) << 41;
  }
  o.trapped = bits != 0;
  o.exc = bits ? (bits | (c.S ? 1 : 0) | (U64(3) << 8)) : 0;
  return o;
}

static bool matches(const Ctx &c, const Expect &e, const Outcome &a) {
  if (a.opcdec)
    return false;
  if (e.res_known && a.res != e.res)
    return false;
  const Outcome x = derive(c, e, a.res);
  const u64 fmask = ~(e.dc << 51);
  if ((a.fpcr & fmask) != (x.fpcr & fmask))
    return false;
  const u64 xmask = ~(e.dc | (e.dc << 41));
  if ((a.exc & xmask) != (x.exc & xmask))
    return false;
  return a.trapped == x.trapped;
}

/* ------------------------------------------------------------------------ */
/* IEEE model                                                               */
/* ------------------------------------------------------------------------ */

static void ieee_inv(Expect &e, const Ctx &c) {
  e.det |= X_INV;
  if (!(c.S && (c.fpcr & FPCR_INVD)))
    e.trap |= X_INV;
}
static void ieee_ine(Expect &e, const Ctx &c) {
  e.det |= X_INE;
  if (c.SUI && !(c.fpcr & FPCR_INED))
    e.trap |= X_INE;
}

/* An operand: a denormal is a signed zero under DNZ with /S, else it takes
   the unmaskable invalid-operation trap (HRM A.8, p. A-14) and the result is
   UNPREDICTABLE; a signalling NaN is an invalid operation. */
static void ieee_operand(Expect &e, const Ctx &c, u64 &x) {
  const Cls k = cls_t(x);
  if (k == C_DEN) {
    if (c.dnz) {
      x &= SIGN;
      return;
    }
    e.det |= X_INV;
    e.trap |= X_INV;
    e.res_known = false;
    e.dc = X_ALL & ~X_INV;
    return;
  }
  if (k == C_SNAN)
    ieee_inv(e, c);
}

/* A finite rounded result from the host, with its flags. */
static void ieee_result(Expect &e, const Ctx &c, u64 reg, bool den, bool zero,
                        const HostFlags &f) {
  if (f.invalid) { /* NaN made from non-NaN operands */
    e.res = CQNANV;
    ieee_inv(e, c);
    return;
  }
  if (f.divzero) {
    e.res = reg;
    e.det |= X_DZE;
    if (!(c.S && (c.fpcr & FPCR_DZED)))
      e.trap |= X_DZE;
    return;
  }
  if (f.overflow) {
    e.res = reg;
    e.det |= X_OVF;
    if (!(c.S && (c.fpcr & FPCR_OVFD)))
      e.trap |= X_OVF;
    ieee_ine(e, c);
    return;
  }
  /* Underflow: the result rounded to the destination format is below MIN
     (ARM 4.7.5); the Alpha definition does not depend on whether
     the result is exact (4.7.7.7). */
  if (den || (zero && f.inexact)) {
    if (c.S && c.U && (c.fpcr & FPCR_UNFD) && !(c.fpcr & FPCR_UNDZ)) {
      /* The nontrapping IEEE result (4.7.7.10). */
      e.res = reg;
      e.det |= X_UNF;
      if (f.inexact)
        ieee_ine(e, c);
    } else {
      /* A true zero (4.7.7.7); zeroing is inexact (4.7.7.11). */
      e.res = 0;
      e.det |= X_UNF;
      if (c.U && !(c.S && (c.fpcr & FPCR_UNFD)))
        e.trap |= X_UNF;
      ieee_ine(e, c);
    }
    return;
  }
  e.res = reg;
  if (f.inexact)
    ieee_ine(e, c);
}

static void ieee_result_t(Expect &e, const Ctx &c, const HostT &h) {
  const u32 ex = (h.bits >> 52) & 0x7ff;
  ieee_result(e, c, h.bits, ex == 0 && (h.bits & FRAC52), (h.bits << 1) == 0,
              h.flags);
}

static void ieee_result_s(Expect &e, const Ctx &c, const HostS &h) {
  const u32 ex = (h.bits >> 23) & 0xff;
  ieee_result(e, c, s_to_reg(h.bits), ex == 0 && (h.bits & 0x7fffff),
              (h.bits << 1) == 0, h.flags);
}

static u64 quiet(u64 x, bool s) {
  x |= QNANBIT;
  return s ? (x & ~LOW29) : x;
}

/* CVTTQ: the true value rounded to an integer, its low 64 bits on overflow
   (ARM 4.7.7.9). */
static Expect ieee_cvttq(const Ctx &c, u64 b) {
  Expect e;
  ieee_operand(e, c, b);
  if (!e.res_known)
    return e;
  const Cls k = cls_t(b);
  if (k == C_INF || is_nan(k)) { /* ARM 4.7.10.1: result 0 */
    e.res = 0;
    ieee_inv(e, c);
    return e;
  }
  if (k == C_ZERO) {
    e.res = 0;
    return e;
  }
  const bool sign = b >> 63;
  const u128 m = (u128)((b & FRAC52) | (U64(1) << 52));
  const int ex = (int)((b >> 52) & 0x7ff) - 1075;
  u128 n;
  bool inexact = false, ovf = false;
  if (ex >= 0) {
    if (ex >= 64) {
      n = 0;
      ovf = true;
    } else {
      n = m << ex;
    }
  } else {
    const int sh = -ex;
    n = sh >= 128 ? 0 : (m >> sh);
    const u128 rem = sh >= 128 ? m : (m & (((u128)1 << sh) - 1));
    inexact = rem != 0;
    bool up = false;
    switch (c.rnd) {
    case 2: { /* nearest, ties to even */
      if (sh <= 64) {
        const u128 half = (u128)1 << (sh - 1);
        up = rem > half || (rem == half && (n & 1));
      }
      break;
    }
    case 1:
      up = sign && inexact;
      break;
    case 3:
      up = !sign && inexact;
      break;
    default:
      break;
    }
    n += up;
  }
  if (!ovf)
    ovf = n > (sign ? ((u128)1 << 63) : (((u128)1 << 63) - 1));
  const u64 lo = (u64)n;
  e.res = sign ? (u64)(0 - lo) : lo;
  if (inexact)
    ieee_ine(e, c);
  if (ovf) {
    e.det |= X_IOV;
    if (c.U) /* /V */
      e.trap |= X_IOV;
    if (c.S) /* HRM Table A-11: "Integer overflow ... Invalid Op" */
      ieee_inv(e, c);
  }
  return e;
}

static Expect ieee_model(const Op &op, const Ctx &c, u64 a, u64 b) {
  Expect e;
  const bool sdst = op.dst == F_S;
  switch (op.k) {
  case K_ADD:
  case K_SUB:
  case K_MUL:
  case K_DIV:
  case K_SQRT: {
    if (!op.unary)
      ieee_operand(e, c, a);
    ieee_operand(e, c, b);
    if (!e.res_known)
      return e;
    /* NaN propagation, ARM 4.7.10.4 */
    if (is_nan(cls_t(b))) {
      e.res = quiet(b, sdst);
      return e;
    }
    if (!op.unary && is_nan(cls_t(a))) {
      e.res = quiet(a, sdst);
      return e;
    }
    const HostOp ho = op.k == K_ADD   ? HostOp::Add
                      : op.k == K_SUB ? HostOp::Sub
                      : op.k == K_MUL ? HostOp::Mul
                      : op.k == K_DIV ? HostOp::Div
                                      : HostOp::Sqrt;
    if (sdst)
      ieee_result_s(e, c, host_s(ho, reg_to_s(a), reg_to_s(b), c.rnd));
    else
      ieee_result_t(e, c, host_t(ho, a, b, c.rnd));
    return e;
  }
  case K_CVTTS: {
    ieee_operand(e, c, b);
    if (!e.res_known)
      return e;
    const Cls k = cls_t(b);
    if (is_nan(k)) { /* ARM 4.7.10.1 */
      e.res = quiet(b, true);
      return e;
    }
    ieee_result_s(e, c, host_t_to_s(b, c.rnd));
    return e;
  }
  case K_CVTST: {
    /* An S denormal is exact in T and converts without a trap unless DNZ
       with /S makes it a signed zero (the interpreter's documented
       treatment, AlphaCPU_ieeefloat.cpp ieee_cvtst). */
    const Cls k = cls_t(b);
    if (k == C_DEN) {
      if (c.dnz) {
        e.res = b & SIGN;
      } else {
        const u32 s = reg_to_s(b);
        float f;
        memcpy(&f, &s, 4);
        const double d = f;
        memcpy(&e.res, &d, 8);
      }
      return e;
    }
    if (k == C_SNAN)
      ieee_inv(e, c);
    e.res = is_nan(k) ? (b | QNANBIT) : b;
    return e;
  }
  case K_CVTTQ:
    return ieee_cvttq(c, b);
  case K_CVTQS:
    ieee_result_s(e, c, host_q_to_s((int64_t)b, c.rnd));
    return e;
  case K_CVTQT:
    ieee_result_t(e, c, host_q_to_t((int64_t)b, c.rnd));
    return e;
  default: { /* compares */
    ieee_operand(e, c, a);
    ieee_operand(e, c, b);
    if (!e.res_known)
      return e;
    const int r = host_cmp_t(a, b);
    if (r == 2 && (op.k == K_CMPTLT || op.k == K_CMPTLE))
      ieee_inv(e, c); /* HRM Table A-11: any NaN */
    bool t;
    switch (op.k) {
    case K_CMPTEQ:
      t = r == 0;
      break;
    case K_CMPTLT:
      t = r == -1;
      break;
    case K_CMPTLE:
      t = r == -1 || r == 0;
      break;
    default:
      t = r == 2;
      break;
    }
    e.res = t ? FTRUE : 0;
    return e;
  }
  }
}

/* ------------------------------------------------------------------------ */
/* VAX model: exact values, rounded                                         */
/* ------------------------------------------------------------------------ */

/* (-1)^sign * m * 2^e */
struct XV {
  bool zero = true;
  int sign = 0;
  u128 m = 0;
  int e = 0;
};

static int bitlen(u128 m) {
  const u64 hi = (u64)(m >> 64), lo = (u64)m;
  if (hi)
    return 128 - __builtin_clzll(hi);
  return lo ? 64 - __builtin_clzll(lo) : 0;
}

/* G layout (also F in a register): 0.1f * 2^(exp-1024) */
static XV xv_g(u64 r) {
  XV x;
  x.zero = false;
  x.sign = (int)(r >> 63);
  x.m = (u128)((r & FRAC52) | (U64(1) << 52));
  x.e = (int)((r >> 52) & 0x7ff) - 1024 - 53;
  return x;
}

/* D layout: 0.1f * 2^(exp-128), 55 fraction bits */
static XV xv_d(u64 r) {
  XV x;
  x.zero = false;
  x.sign = (int)(r >> 63);
  x.m = (u128)((r & U64(0x007FFFFFFFFFFFFF)) | (U64(1) << 55));
  x.e = (int)((r >> 55) & 0xff) - 128 - 56;
  return x;
}

static XV xv_int(u64 v) {
  XV x;
  if (!v)
    return x;
  x.zero = false;
  x.sign = (int)(v >> 63);
  x.m = x.sign ? (u128)(0 - v) : (u128)v;
  x.e = 0;
  return x;
}

static XV xv_add(XV x, XV y) {
  if (x.zero)
    return y;
  if (y.zero)
    return x;
  if (x.e < y.e || (x.e == y.e && x.m < y.m)) {
    XV t = x;
    x = y;
    y = t;
  }
  /* x has the larger exponent; both operands have the same precision, so
     it is also the larger magnitude unless the exponents are equal */
  const int d = x.e - y.e;
  XV r;
  r.zero = false;
  u128 xm, ym;
  if (d <= 66) {
    xm = x.m << d;
    ym = y.m;
    r.e = y.e;
  } else { /* y is far below the rounding point: a sticky bit */
    xm = x.m << 66;
    ym = 1;
    r.e = x.e - 66;
  }
  if (x.sign == y.sign) {
    r.m = xm + ym;
    r.sign = x.sign;
  } else if (xm >= ym) {
    r.m = xm - ym;
    r.sign = x.sign;
  } else {
    r.m = ym - xm;
    r.sign = y.sign;
  }
  if (r.m == 0)
    return XV();
  return r;
}

static XV xv_mul(const XV &x, const XV &y) {
  if (x.zero || y.zero)
    return XV();
  XV r;
  r.zero = false;
  r.sign = x.sign ^ y.sign;
  r.m = x.m * y.m;
  r.e = x.e + y.e;
  return r;
}

static XV xv_div(const XV &x, const XV &y) { /* y nonzero */
  if (x.zero)
    return XV();
  XV r;
  r.zero = false;
  r.sign = x.sign ^ y.sign;
  const u128 n = x.m << 70;
  const u128 q = n / y.m;
  r.m = (q << 1) | ((n % y.m) ? 1 : 0);
  r.e = x.e - 70 - y.e - 1;
  return r;
}

static u128 isqrt128(u128 n) {
  u128 r = 0, bit = (u128)1 << 126;
  while (bit > n)
    bit >>= 2;
  while (bit) {
    if (n >= r + bit) {
      n -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  return r;
}

static XV xv_sqrt(XV x) { /* x positive */
  if (x.zero)
    return x;
  if (x.e & 1) {
    x.m <<= 1;
    x.e -= 1;
  }
  int sh = (120 - bitlen(x.m)) & ~1;
  x.m <<= sh;
  x.e -= sh;
  const u128 s = isqrt128(x.m);
  XV r;
  r.zero = false;
  r.m = (s << 1) | ((s * s != x.m) ? 1 : 0);
  r.e = x.e / 2 - 1;
  return r;
}

enum VStatus { V_OK, V_OVF, V_UNF };

/* Round to the format's precision (VAX normal rounding is half away from
   zero, ARM 4.7.5) with an unbounded exponent, then check the
   range. */
static u64 vax_round(const XV &x, Fmt f, bool normal, VStatus &st) {
  st = V_OK;
  if (x.zero)
    return 0;
  const int p = f == F_F ? 24 : f == F_G ? 53 : 56;
  u128 m = x.m;
  int e = x.e;
  const int L = bitlen(m);
  if (L > p) {
    const int sh = L - p;
    const bool rb = (m >> (sh - 1)) & 1;
    m >>= sh;
    e += sh;
    if (normal && rb) {
      m += 1;
      if (m >> p) {
        m >>= 1;
        e += 1;
      }
    }
  } else {
    m <<= (p - L);
    e -= (p - L);
  }
  const int E = e + p; /* value = 0.1xxx * 2^E */
  const u64 sign = (u64)x.sign << 63;
  if (f == F_D) {
    const int field = E + 128;
    if (field > 255) {
      st = V_OVF;
      return sign | (U64(255) << 55) | U64(0x007FFFFFFFFFFFFF);
    }
    if (field < 1) {
      st = V_UNF;
      return 0;
    }
    return sign | ((u64)field << 55) | ((u64)m & U64(0x007FFFFFFFFFFFFF));
  }
  const int field = f == F_F ? E + 128 : E + 1024;
  const int fmax = f == F_F ? 255 : 2047;
  if (field > fmax) {
    st = V_OVF;
    return f == F_F ? (sign | (U64(1151) << 52) | (U64(0x7FFFFF) << 29))
                    : (sign | (U64(2047) << 52) | FRAC52);
  }
  if (field < 1) {
    st = V_UNF;
    return 0;
  }
  const u64 rexp = (u64)(E + 1024);
  if (f == F_F)
    return sign | (rexp << 52) | (((u64)m & 0x7FFFFF) << 29);
  return sign | (rexp << 52) | ((u64)m & FRAC52);
}

static bool vax_dirty(u64 r, bool dfmt) {
  const u32 ex = dfmt ? (r >> 55) & 0xff : (r >> 52) & 0x7ff;
  const u64 fr = dfmt ? (r & U64(0x007FFFFFFFFFFFFF)) : (r & FRAC52);
  return ex == 0 && !(r >> 63) && fr;
}

static Expect vax_model(const Op &op, const Ctx &c, u64 a, u64 b) {
  Expect e;
  const bool normal = (c.ins >> 12) & 1;
  /* exp = 0: true zero; sign set: reserved operand, invalid operation in
     every mode; sign clear and fraction set: dirty zero, invalid operation
     except with /S, which treats it as zero (ARM 4.7.7.1). */
  auto operand = [&](u64 r, bool dfmt) -> XV {
    const u32 ex = dfmt ? (r >> 55) & 0xff : (r >> 52) & 0x7ff;
    if (ex == 0) {
      if ((r >> 63) || (vax_dirty(r, dfmt) && !c.S)) {
        e.trap |= X_INV;
        e.res_known = false;
        e.dc = X_ALL & ~X_INV;
      }
      return XV();
    }
    return dfmt ? xv_d(r) : xv_g(r);
  };
  auto finish = [&](const XV &x, Fmt f) {
    VStatus st;
    const u64 r = vax_round(x, f, normal, st);
    if (st == V_OVF)
      e.trap |= X_OVF;
    if (st == V_UNF && c.U)
      e.trap |= X_UNF;
    e.res = r;
  };
  switch (op.k) {
  case V_ADD:
  case V_SUB: {
    XV x = operand(a, false), y = operand(b, false);
    if (op.k == V_SUB && !y.zero)
      y.sign ^= 1;
    finish(xv_add(x, y), op.dst);
    break;
  }
  case V_MUL: {
    XV x = operand(a, false), y = operand(b, false);
    finish(xv_mul(x, y), op.dst);
    break;
  }
  case V_DIV: {
    XV x = operand(a, false), y = operand(b, false);
    if (y.zero) {
      e.trap |= X_DZE;
      e.res_known = false; /* UNPREDICTABLE */
      break;
    }
    finish(xv_div(x, y), op.dst);
    break;
  }
  case V_SQRT: {
    XV x = operand(b, false);
    if (!x.zero && x.sign) {
      e.trap |= X_INV;
      e.res_known = false;
      break;
    }
    finish(xv_sqrt(x), op.dst);
    break;
  }
  case V_CVTGF:
    finish(operand(b, false), F_F);
    break;
  case V_CVTGD:
    finish(operand(b, false), F_D);
    break;
  case V_CVTDG:
    finish(operand(b, true), F_G);
    break;
  case V_CVTQF:
    finish(xv_int(b), F_F);
    break;
  case V_CVTQG:
    finish(xv_int(b), F_G);
    break;
  case V_CVTGQ: {
    XV x = operand(b, false);
    if (x.zero) {
      e.res = 0;
      break;
    }
    u128 n;
    bool ovf = false;
    if (x.e >= 0) {
      if (x.e >= 64) {
        n = 0;
        ovf = true;
      } else {
        n = x.m << x.e;
      }
    } else {
      const int sh = -x.e;
      n = sh >= 128 ? 0 : (x.m >> sh);
      if (normal && sh <= 128 && ((x.m >> (sh - 1)) & 1))
        n += 1;
    }
    if (!ovf)
      ovf = n > (x.sign ? ((u128)1 << 63) : (((u128)1 << 63) - 1));
    const u64 lo = (u64)n;
    e.res = x.sign ? (u64)(0 - lo) : lo;
    if (ovf && c.U) /* /V */
      e.trap |= X_IOV;
    break;
  }
  default: { /* CMPGxx */
    XV x = operand(a, false), y = operand(b, false);
    int r;
    XV d = y;
    if (!d.zero)
      d.sign ^= 1;
    const XV s = xv_add(x, d);
    r = s.zero ? 0 : (s.sign ? -1 : 1);
    bool t = op.k == V_CMPEQ ? r == 0 : op.k == V_CMPLT ? r < 0 : r <= 0;
    e.res = t ? FTRUE : 0;
    break;
  }
  }
  return e;
}

/* ------------------------------------------------------------------------ */
/* Classifying a difference: which item explains it                         */
/* ------------------------------------------------------------------------ */

static bool is_den_t(u64 x) { return cls_t(x) == C_DEN; }

static const char *classify(const Op &op, const Ctx &c, u64 a, u64 b,
                            const Outcome &bad, const Outcome &good) {
  if (op.vax()) {
    const bool dsrc = op.src == F_D;
    const bool dirty = (!op.unary && op.src != F_Q && vax_dirty(a, false)) ||
                       (op.src != F_Q && vax_dirty(b, dsrc));
    if (dirty) {
      const bool cmp = op.k == V_CMPEQ || op.k == V_CMPLT || op.k == V_CMPLE;
      return (cmp && c.S) ? "FP-04" : "FP-03";
    }
    if (op.k == V_CVTGQ && b == U64(0xC400000000000000))
      return "FP-12";
    if ((bad.exc & X_OVF) && (good.exc & X_OVF) && bad.res != good.res &&
        bad.fpcr == good.fpcr && bad.exc == good.exc)
      return "FP-10";
    if (op.dst == F_F && bad.fpcr == good.fpcr && bad.exc == good.exc &&
        ((bad.res ^ good.res) & ~LOW29) == 0 && (good.res & LOW29) == 0)
      return "FP-01";
    return nullptr;
  }
  const bool fp_src = op.src != F_Q;
  if ((c.fpcr & FPCR_DNZ) && !c.S && fp_src &&
      ((!op.unary && is_den_t(a)) || is_den_t(b)))
    return "FP-06";
  if (op.k == K_CVTTQ && !c.S && cls_t(b) == C_FIN &&
      ((b >> 52) & 0x7ff) >= 1023 + 62 && bad.res == good.res)
    return "FP-05";
  const u64 min =
      op.dst == F_S ? U64(0x3810000000000000) : U64(0x0010000000000000);
  if ((good.res & ~SIGN) == min && bad.res == 0)
    return "FP-11";
  if (c.S && c.U && (c.fpcr & FPCR_UNFD) && !(c.fpcr & FPCR_UNDZ) &&
      bad.res == 0 && ((good.res >> 52) & 0x7ff) == 0)
    return "DENORM";
  return nullptr;
}

/* ------------------------------------------------------------------------ */
/* Operands                                                                 */
/* ------------------------------------------------------------------------ */

static u64 rng_state = U64(0x243F6A8885A308D3);
static u64 rnd64() {
  u64 z = (rng_state += U64(0x9E3779B97F4A7C15));
  z = (z ^ (z >> 30)) * U64(0xBF58476D1CE4E5B9);
  z = (z ^ (z >> 27)) * U64(0x94D049BB133111EB);
  return z ^ (z >> 31);
}
static u64 rndn(u64 n) { return rnd64() % n; }

static u64 mk_t(int sign, int bexp, u64 frac) {
  return ((u64)sign << 63) | ((u64)(bexp & 0x7ff) << 52) | (frac & FRAC52);
}
static u64 mk_s(int sign, int bexp, u32 frac) {
  return s_to_reg(((u32)sign << 31) | ((u32)(bexp & 0xff) << 23) |
                  (frac & 0x7fffff));
}

static const std::vector<u64> t_fixed = {0,
                                         SIGN,
                                         U64(0x0000000000000001),
                                         U64(0x8000000000000001),
                                         U64(0x000FFFFFFFFFFFFF),
                                         U64(0x0008000000000000),
                                         U64(0x800FFFFFFFFFFFFF),
                                         U64(0x0010000000000000),
                                         U64(0x8010000000000000),
                                         U64(0x0010000000000001),
                                         U64(0x001FFFFFFFFFFFFF),
                                         U64(0x0020000000000000),
                                         U64(0x3FEFFFFFFFFFFFFF),
                                         U64(0x3FF0000000000000),
                                         U64(0xBFF0000000000000),
                                         U64(0x3FF0000000000001),
                                         U64(0x3FF8000000000000),
                                         U64(0x4000000000000000),
                                         U64(0x3FE0000000000000),
                                         U64(0x4008000000000000),
                                         U64(0x3FD5555555555555),
                                         U64(0x3FB999999999999A),
                                         U64(0xC004000000000000),
                                         U64(0x4004000000000000),
                                         U64(0x7FEFFFFFFFFFFFFF),
                                         U64(0xFFEFFFFFFFFFFFFF),
                                         U64(0x7FE0000000000000),
                                         U64(0x7FDFFFFFFFFFFFFF),
                                         U64(0x7FF0000000000000),
                                         U64(0xFFF0000000000000),
                                         U64(0x7FF8000000000000),
                                         U64(0xFFF8000000000000),
                                         U64(0x7FF8000000000123),
                                         U64(0x7FF0000000000001),
                                         U64(0xFFF4000000000000),
                                         U64(0x4330000000000000),
                                         U64(0x4340000000000001),
                                         U64(0x43E0000000000000),
                                         U64(0xC3E0000000000000),
                                         U64(0x43DFFFFFFFFFFFFF),
                                         U64(0x43F0000000000000),
                                         U64(0xC3E0000000000001),
                                         U64(0x3FE0000000000001),
                                         U64(0x3FDFFFFFFFFFFFFF),
                                         U64(0x1FF0000000000000),
                                         U64(0x2000000000000000),
                                         U64(0x5FF0000000000000),
                                         U64(0x3810000000000000),
                                         U64(0x380FFFFFE0000000),
                                         U64(0x47EFFFFFE0000000),
                                         U64(0x47EFFFFFF0000000),
                                         U64(0x36A0000000000000),
                                         U64(0x36A8000000000000),
                                         U64(0x3690000000000000),
                                         U64(0x0018000000000000),
                                         U64(0x3CA0000000000000),
                                         U64(0x3C90000000000000)};

static const std::vector<u32> s_fixed_words = {
    0,          0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0x3F000000,
    0x3FC00000, 0x40400000, 0x3EAAAAAB, 0x3DCCCCCD, 0xC0200000, 0x00800000,
    0x00800001, 0x80800000, 0x00000001, 0x007FFFFF, 0x00400000, 0x80000001,
    0x7F7FFFFF, 0xFF7FFFFF, 0x7F000000, 0x7F800000, 0xFF800000, 0x7FC00000,
    0xFFC00000, 0x7FC00123, 0x7F800001, 0x7FA00000, 0x3F7FFFFF, 0x3F800001,
    0x4B000000, 0x4B800001, 0x1F800000, 0x5F800000, 0x20000000, 0x60000000,
    0x00C00000, 0x33800000, 0x34000000};

static const std::vector<u64> q_fixed = {0,
                                         1,
                                         U64(0xFFFFFFFFFFFFFFFF),
                                         2,
                                         3,
                                         U64(0x0000000001000001),
                                         U64(0x0000000001000003),
                                         U64(0x0020000000000001),
                                         U64(0x0020000000000003),
                                         U64(0x7FFFFFFFFFFFFFFF),
                                         U64(0x8000000000000000),
                                         U64(0x8000000000000001),
                                         U64(0x7FFFFFFFFFFFFE00),
                                         U64(0x0000000100000001),
                                         U64(0xFFFFFFFFFF000001),
                                         U64(0x00000000FFFFFFFF),
                                         U64(0x4000000000000001)};

static u64 rnd_t() {
  const int sign = (int)rndn(2);
  const u64 fr = rnd64();
  switch (rndn(9)) {
  case 0:
    return rnd64();
  case 1:
    return mk_t(sign, (int)rndn(70), fr); /* denormal / underflow edge */
  case 2:
    return mk_t(sign, 1023 - 60 + (int)rndn(120), fr); /* about 1 */
  case 3:
    return mk_t(sign, 2047 - 70 + (int)rndn(70), fr); /* overflow edge */
  case 4:
    return mk_t(sign, 0x7ff, rndn(3) ? 0 : fr); /* Inf, NaN */
  case 5: {                                     /* integral values */
    const double d = (double)((int64_t)rnd64() >> rndn(64));
    u64 v;
    memcpy(&v, &d, 8);
    return v;
  }
  case 6:
    return mk_t(sign, 1023 + 40 + (int)rndn(40), fr); /* CVTTQ range edge */
  case 7:
    return mk_t(sign, 896 - 40 + (int)rndn(60), fr); /* S range low edge */
  default:
    return mk_t(sign, 1150 - 20 + (int)rndn(30), fr); /* S range high edge */
  }
}

static u64 rnd_s() {
  const int sign = (int)rndn(2);
  const u32 fr = (u32)rnd64();
  switch (rndn(5)) {
  case 0:
    return s_to_reg((u32)rnd64());
  case 1:
    return mk_s(sign, (int)rndn(40), fr);
  case 2:
    return mk_s(sign, 127 - 30 + (int)rndn(60), fr);
  case 3:
    return mk_s(sign, 255 - 40 + (int)rndn(40), fr);
  default:
    return mk_s(sign, 0xff, rndn(3) ? 0 : fr);
  }
}

static u64 rnd_q() {
  const u64 v = rnd64();
  switch (rndn(3)) {
  case 0:
    return v;
  case 1:
    return (u64)((int64_t)v >> rndn(64));
  default:
    return v & (~U64(0) >> rndn(64));
  }
}

/* Two operands whose result lands near an edge of the range. */
static void rnd_pair_ieee(const Op &op, u64 &a, u64 &b) {
  const bool s = op.src == F_S;
  const int bias = s ? 127 : 1023, emax = s ? 254 : 2046;
  const int lo = s ? -126 : -1022, hi = s ? 127 : 1023;
  const int target =
      rndn(2) ? lo - (int)rndn(s ? 26 : 56) + 2 : hi - 2 + (int)rndn(4);
  auto mk = [&](int ue) -> u64 {
    int be = ue + bias;
    if (be < 1)
      be = 1;
    if (be > emax)
      be = emax;
    const int sign = (int)rndn(2);
    return s ? mk_s(sign, be, (u32)rnd64()) : mk_t(sign, be, rnd64());
  };
  const int ea = lo + (int)rndn(hi - lo);
  switch (op.k) {
  case K_MUL:
    a = mk(ea);
    b = mk(target - ea);
    break;
  case K_DIV:
    a = mk(ea);
    b = mk(ea - target);
    break;
  default: { /* ADD/SUB: two close values near MIN */
    const int e0 = lo + (int)rndn(3);
    a = mk(e0);
    b = (a & ~SIGN) ^ (rndn(2) ? SIGN : 0);
    b += s ? ((u64)rndn(5) << 29) : rndn(5);
    break;
  }
  }
}

static u64 mk_g(int sign, int ex, u64 fr) {
  return ((u64)sign << 63) | ((u64)(ex & 0x7ff) << 52) | (fr & FRAC52);
}
static u64 mk_f(int sign, int ex, u64 fr) { /* F in a register: canonical */
  return mk_g(sign, ex, fr) & ~LOW29;
}

static const std::vector<u64> g_fixed = {
    0,
    U64(0x0000000000000001), /* dirty zero */
    U64(0x0000000020000000), /* dirty zero, F-canonical */
    SIGN,                    /* reserved operand */
    U64(0x8000000000000005), /* reserved operand */
    U64(0x4010000000000000), /* 1.0 */
    U64(0xC010000000000000),
    U64(0x4000000000000000), /* 0.5 */
    U64(0x4020000000000000), /* 2.0 */
    U64(0x4014000000000000), /* 1.25 */
    U64(0x3FF5555555555555), /* 1/3 */
    U64(0x3E30000000000000), /* 2^-30 */
    U64(0x0010000000000000), /* G MIN */
    U64(0x8010000000000000),
    U64(0x7FFFFFFFFFFFFFFF), /* G MAX */
    U64(0xFFFFFFFFFFFFFFFF),
    U64(0x3810000000000000), /* F MIN */
    U64(0x47FFFFFFE0000000), /* F MAX */
    U64(0x47FFFFFFF0000000), /* above F MAX */
    U64(0x380FFFFFF0000000), /* below F MIN */
    U64(0xC400000000000000), /* -2^63 */
    U64(0x4400000000000000), /* 2^63 */
    U64(0x43FFFFFFFFFFFFFF),
    U64(0x4024000000000000), /* 2.5 */
    U64(0xC024000000000000), /* -2.5 */
    U64(0x4360000000000000), /* 2^53 */
    U64(0x4360000000000001),
    U64(0x4010000000000001), /* 1 + ulp */
    U64(0x4010000010000000), /* 1 + half an F ulp */
};

static u64 rnd_g(bool f) {
  const int sign = (int)rndn(2);
  const u64 fr = rnd64();
  int ex;
  switch (rndn(6)) {
  case 0:
    ex = f ? 897 + (int)rndn(10) : 1 + (int)rndn(10);
    break;
  case 1:
    ex = f ? 1151 - (int)rndn(10) : 2047 - (int)rndn(10);
    break;
  case 2:
    ex = 1024 - 40 + (int)rndn(80);
    break;
  case 3:
    ex = f ? 897 + (int)rndn(255) : 1 + (int)rndn(2047);
    break;
  case 4:
    ex = 1024 + 55 + (int)rndn(15); /* CVTGQ edge */
    break;
  default:
    if (rndn(4) == 0) /* zero, dirty zero, reserved operand */
      return rndn(3) == 0 ? 0 : ((u64)sign << 63) | (fr & FRAC52);
    ex = 1 + (int)rndn(2047);
    break;
  }
  return f ? mk_f(sign, ex, fr) : mk_g(sign, ex, fr);
}

static u64 rnd_d() {
  const int sign = (int)rndn(2);
  const u64 fr = rnd64() & U64(0x007FFFFFFFFFFFFF);
  if (rndn(10) == 0)
    return rndn(2) ? ((u64)sign << 63) | (fr & 7) : 0;
  return ((u64)sign << 63) | ((u64)(1 + rndn(255)) << 55) | fr;
}

static void rnd_pair_vax(const Op &op, u64 &a, u64 &b) {
  const bool f = op.src == F_F;
  const int lo = f ? 897 : 1, hi = f ? 1151 : 2047;
  auto mk = [&](int ex) -> u64 {
    if (ex < lo)
      ex = lo;
    if (ex > hi)
      ex = hi;
    const int sign = (int)rndn(2);
    return f ? mk_f(sign, ex, rnd64()) : mk_g(sign, ex, rnd64());
  };
  /* exponent fields add with a bias of 1024 */
  const int ea = lo + (int)rndn(hi - lo);
  const int target = rndn(2) ? lo + (int)rndn(4) - 2 : hi + (int)rndn(4) - 1;
  if (op.k == V_MUL) {
    a = mk(ea);
    b = mk(target - ea + 1024);
  } else if (op.k == V_DIV) {
    a = mk(ea);
    b = mk(ea - target + 1024);
  } else {
    a = mk(lo + (int)rndn(3));
    b = (a & ~SIGN) ^ (rndn(2) ? SIGN : 0);
  }
}

/* ------------------------------------------------------------------------ */
/* The sweep                                                                */
/* ------------------------------------------------------------------------ */

static bool verbose = false;
static long n_random = 3000;
static long n_cases = 0, n_failed = 0, n_frozen_cmp = 0;
static int printed = 0;

static void report(const char *what, const Op &op, u32 ins, u64 a, u64 b,
                   u64 fpcr, const Outcome &got, const Outcome *want,
                   const char *tag) {
  if (printed++ >= (verbose ? 100000 : 60))
    return;
  printf("  %s %s ins=%08x fa=%016" PRIx64 " fb=%016" PRIx64 " fpcr=%016" PRIx64
         "\n    got  res=%016" PRIx64 " fpcr=%016" PRIx64 " exc=%04" PRIx64
         " trap=%d%s\n",
         what, op.name, ins, a, b, fpcr, got.res, got.fpcr, got.exc,
         got.trapped, got.opcdec ? " OPCDEC" : "");
  if (want)
    printf("    want res=%016" PRIx64 " fpcr=%016" PRIx64 " exc=%04" PRIx64
           " trap=%d   [%s]\n",
           want->res, want->fpcr, want->exc, want->trapped,
           tag ? tag : "no item");
}

static void check(const Op &op, u32 ins, u64 a, u64 b, u64 fpcr) {
  const Ctx c = ctx_of(ins, fpcr, op.vax());
  const Expect e = op.vax() ? vax_model(op, c, a, b) : ieee_model(op, c, a, b);
  const Outcome cur = run<CAlphaCPU>(ins, a, b, fpcr);
  n_cases++;
  if (!matches(c, e, cur)) {
    const Outcome want = derive(c, e, e.res_known ? e.res : cur.res);
    const char *tag = classify(op, c, a, b, cur, want);
    Item *it = tag ? item(tag) : nullptr;
    if (it && !it->fixed) {
      it->model_mismatch++;
      if (verbose)
        report("expected", op, ins, a, b, fpcr, cur, &want, tag);
    } else {
      n_failed++;
      report("MODEL", op, ins, a, b, fpcr, cur, &want, tag);
    }
  }
#if defined(FP_HAVE_FROZEN)
  const Outcome old = run<frozen::CAlphaCPU>(ins, a, b, fpcr);
  n_frozen_cmp++;
  if (!(old == cur)) {
    const char *tag = classify(op, c, a, b, old, cur);
    Item *it = tag ? item(tag) : nullptr;
    if (it && it->fixed) {
      it->frozen_explained++;
    } else {
      n_failed++;
      report("FROZEN", op, ins, a, b, fpcr, old, &cur, tag);
    }
  }
#endif
}

static const std::vector<u64> ieee_fpcrs = {
    FPCR_N,
    FPCR_N | FPCR_INE,
    FPCR_N | (U64(0x3f) << 52), /* every status bit already set */
    FPCR_N | FPCR_UNFD,
    FPCR_N | FPCR_UNFD | FPCR_UNDZ,
    FPCR_N | FPCR_DNZ,
    FPCR_N | FPCR_INVD | FPCR_DZED | FPCR_OVFD | FPCR_INED | FPCR_UNFD,
    FPCR_N | FPCR_INVD | FPCR_DZED | FPCR_OVFD | FPCR_INED | FPCR_UNFD |
        FPCR_UNDZ | FPCR_DNZ,
    U64(0) << 58 | FPCR_UNFD,            /* DYN chopped */
    U64(1) << 58 | FPCR_UNFD | FPCR_INE, /* DYN minus infinity */
    U64(3) << 58 | FPCR_UNFD,            /* DYN plus infinity */
};

static void sweep(const Op &op) {
  std::vector<u64> fixed;
  switch (op.src) {
  case F_T:
    fixed = t_fixed;
    break;
  case F_S:
    for (u32 w : s_fixed_words)
      fixed.push_back(s_to_reg(w));
    break;
  case F_Q:
    fixed = q_fixed;
    break;
  case F_F:
    for (u64 g : g_fixed)
      fixed.push_back(g & ~LOW29);
    break;
  case F_D:
    fixed = {0,
             1,
             SIGN,
             U64(0x4080000000000000),
             U64(0x3FD5555555555555),
             U64(0x7FFFFFFFFFFFFFFF),
             U64(0x00800000000000FF)};
    break;
  default:
    fixed = g_fixed;
    break;
  }
  const std::vector<u64> fpcrs =
      op.vax() ? std::vector<u64>{FPCR_N, FPCR_N | (U64(0x3f) << 52)}
               : ieee_fpcrs;
  auto rnd_operand = [&]() -> u64 {
    switch (op.src) {
    case F_T:
      return rnd_t();
    case F_S:
      return rnd_s();
    case F_Q:
      return rnd_q();
    case F_F:
      return rnd_g(true);
    case F_D:
      return rnd_d();
    default:
      return rnd_g(false);
    }
  };
  const bool binary = !op.unary;
  for (u32 t : op.traps)
    for (u32 r : op.rnds) {
      const u32 ins = encode(op, op.base | t | r);
      for (u64 fpcr : fpcrs) {
        if (binary) {
          for (u64 a : fixed)
            for (u64 b : fixed)
              check(op, ins, a, b, fpcr);
          for (long k = 0; k < n_random; k++) {
            u64 a, b;
            if (k & 1) {
              a = rnd_operand();
              b = rnd_operand();
            } else if (op.vax()) {
              rnd_pair_vax(op, a, b);
            } else {
              rnd_pair_ieee(op, a, b);
            }
            check(op, ins, a, b, fpcr);
          }
        } else {
          for (u64 b : fixed)
            check(op, ins, 0, b, fpcr);
          for (long k = 0; k < 4 * n_random; k++)
            check(op, ins, 0, rnd_operand(), fpcr);
        }
      }
    }
}

/* ------------------------------------------------------------------------ */
/* Hand tables                                                              */
/* ------------------------------------------------------------------------ */

struct Hand {
  const char *item; /* the item this case proves, or nullptr */
  const char *cite;
  const char *op;
  u32 fn; /* rounding | trap qualifier bits added to the base function */
  u64 a, b, fpcr;
  u64 res;
  u64 det, trap; /* status bits detected, traps taken */
};

/* qualifier bits */
static const u32 QC = 0x000, QM = 0x040, QN = 0x080, QD = 0x0c0;
static const u32 QU = 0x100, QSU = 0x500, QSUI = 0x700, QS = 0x400;

static const Hand hands[] = {
    /* IEEE rounding (ARM 4.7.5) */
    {nullptr, "4.7.5 normal rounding: tie to even", "ADDT", QN,
     U64(0x3FF0000000000000), U64(0x3CA0000000000000), FPCR_N,
     U64(0x3FF0000000000000), X_INE, 0},
    {nullptr, "4.7.5 plus infinity (/D, FPCR<DYN>=11)", "ADDT", QD,
     U64(0x3FF0000000000000), U64(0x3CA0000000000000), U64(3) << 58,
     U64(0x3FF0000000000001), X_INE, 0},
    {nullptr, "4.7.5 chopped", "ADDT", QC, U64(0x3FF0000000000000),
     U64(0x3CA8000000000000), FPCR_N, U64(0x3FF0000000000000), X_INE, 0},
    {nullptr, "4.7.5 minus infinity of a negative", "ADDT", QM,
     U64(0xBFF0000000000000), U64(0xBCA8000000000000), FPCR_N,
     U64(0xBFF0000000000001), X_INE, 0},
    /* signed zero (IEEE 754 by reference, ARM 4.7.10) */
    {nullptr, "exact cancellation is +0", "SUBT", QN, U64(0x3FF0000000000000),
     U64(0x3FF0000000000000), FPCR_N, 0, 0, 0},
    {nullptr, "exact cancellation under minus infinity is -0", "SUBT", QM,
     U64(0x3FF0000000000000), U64(0x3FF0000000000000), FPCR_N, SIGN, 0, 0},
    {nullptr, "HRM Table A-11 SQRT -0 = -0", "SQRTT", QN, 0, SIGN, FPCR_N, SIGN,
     0, 0},
    /* overflow by rounding mode (4.7.5) */
    {nullptr, "4.7.5 normal: overflow to +Inf", "MULT", QN,
     U64(0x7FEFFFFFFFFFFFFF), U64(0x4000000000000000), FPCR_N,
     U64(0x7FF0000000000000), X_OVF | X_INE, X_OVF},
    {nullptr, "4.7.5 chopped: overflow to MAX", "MULT", QC,
     U64(0x7FEFFFFFFFFFFFFF), U64(0x4000000000000000), FPCR_N,
     U64(0x7FEFFFFFFFFFFFFF), X_OVF | X_INE, X_OVF},
    {nullptr, "4.7.5 minus infinity: positive overflow to MAX", "MULT", QM,
     U64(0x7FEFFFFFFFFFFFFF), U64(0x4000000000000000), FPCR_N,
     U64(0x7FEFFFFFFFFFFFFF), X_OVF | X_INE, X_OVF},
    {nullptr, "4.7.5 minus infinity: negative overflow to -Inf", "MULT", QM,
     U64(0xFFEFFFFFFFFFFFFF), U64(0x4000000000000000), FPCR_N,
     U64(0xFFF0000000000000), X_OVF | X_INE, X_OVF},
    {nullptr, "4.7.7.10 OVFD with /S: no trap", "MULT", QSU | QN,
     U64(0x7FEFFFFFFFFFFFFF), U64(0x4000000000000000), FPCR_N | FPCR_OVFD,
     U64(0x7FF0000000000000), X_OVF | X_INE, 0},
    {nullptr, "S overflow: the S MAX in a register (chopped)", "MULS", QC,
     U64(0x47EFFFFFE0000000), U64(0x4000000000000000), FPCR_N,
     U64(0x47EFFFFFE0000000), X_OVF | X_INE, X_OVF},
    /* underflow (4.7.5, 4.7.7.7, 4.7.7.10, 4.7.7.11) */
    {nullptr, "4.7.7.7 underflow without /U: true zero", "MULT", QN,
     U64(0x0010000000000000), U64(0x3FE0000000000000), FPCR_N, 0, X_UNF | X_INE,
     0},
    {nullptr, "4.7.7.7 a negative underflow is still a true zero", "MULT", QN,
     U64(0x8010000000000000), U64(0x3FE0000000000000), FPCR_N, 0, X_UNF | X_INE,
     0},
    {nullptr, "4.7.7.7 /U: underflow trap, true zero", "MULT", QU | QN,
     U64(0x0010000000000000), U64(0x3FE0000000000000), FPCR_N, 0, X_UNF | X_INE,
     X_UNF},
    {nullptr, "4.7.7.11 UNFD+UNDZ with /S: true zero, inexact, no trap", "MULT",
     QSU | QN, U64(0x0010000000000000), U64(0x3FE0000000000000),
     FPCR_N | FPCR_UNFD | FPCR_UNDZ, 0, X_UNF | X_INE, 0},
    {nullptr, "4.7.7.10 UNFD without /S has no effect", "MULT", QU | QN,
     U64(0x0010000000000000), U64(0x3FE0000000000000), FPCR_N | FPCR_UNFD, 0,
     X_UNF | X_INE, X_UNF},
    {"DENORM", "4.7.7.10: UNFD with /S, UNDZ clear: the exact denormal", "MULT",
     QSU | QN, U64(0x0010000000000000), U64(0x3FE0000000000000),
     FPCR_N | FPCR_UNFD, U64(0x0008000000000000), X_UNF, 0},
    {"DENORM", "4.7.7.10: a negative denormal keeps its sign", "MULT", QSU | QN,
     U64(0x8010000000000000), U64(0x3FE0000000000000), FPCR_N | FPCR_UNFD,
     U64(0x8008000000000000), X_UNF, 0},
    {"DENORM", "4.7.7.10: a rounded denormal is inexact (/SUI traps INE)",
     "DIVT", QSUI | QN, U64(0x0010000000000000), U64(0x4008000000000000),
     FPCR_N | FPCR_UNFD, U64(0x0005555555555555), X_UNF | X_INE, X_INE},
    {"DENORM", "4.7.7.10: S denormal in register format", "MULS", QSU | QN,
     U64(0x3810000000000000), U64(0x3FE0000000000000), FPCR_N | FPCR_UNFD,
     U64(0x0008000000000000), X_UNF, 0},
    {"DENORM", "4.7.7.10: below half the least denormal rounds to -0", "MULT",
     QSU | QN, U64(0x8000000000000001) | U64(0x0010000000000000),
     U64(0x3C80000000000000), FPCR_N | FPCR_UNFD, SIGN, X_UNF | X_INE, 0},
    {"DENORM", "4.7.7.10: plus infinity rounds a tiny value up", "MULT",
     QSU | QD, U64(0x0010000000000000), U64(0x0010000000000000),
     (U64(3) << 58) | FPCR_UNFD, U64(0x0000000000000001), X_UNF | X_INE, 0},
    {"FP-11", "4.7.5: a true result of MIN - 1/2 LSB rounds to MIN", "MULT", QN,
     U64(0x3FEFFFFFFFFFFFFF), U64(0x0010000000000000), FPCR_N,
     U64(0x0010000000000000), X_INE, 0},
    {"FP-11", "4.7.5: plus infinity maps only <= MIN - 1 LSB to underflow",
     "MULT", QD, U64(0x3FEFFFFFFFFFFFFF), U64(0x0010000000000000), U64(3) << 58,
     U64(0x0010000000000000), X_INE, 0},
    {nullptr, "4.7.5: chopped maps < MIN to underflow", "MULT", QC,
     U64(0x3FEFFFFFFFFFFFFF), U64(0x0010000000000000), FPCR_N, 0, X_UNF | X_INE,
     0},
    /* denormal operands (HRM A.8; ARM 4.7.7.11) */
    {nullptr, "HRM A.8: a denormal operand traps, unmaskable", "MULT", QSU | QN,
     U64(0x0000000000000001), 0, FPCR_N | FPCR_INVD, 0, X_INV, X_INV},
    {nullptr, "4.7.7.11: DNZ with /S: the denormal is a signed zero", "ADDT",
     QSU | QN, U64(0x0000000000000001), U64(0x3FF0000000000000),
     FPCR_N | FPCR_DNZ, U64(0x3FF0000000000000), 0, 0},
    {"FP-06", "4.7.7.11: DNZ acts only with /S", "MULT", QN,
     U64(0x0000000000000001), 0, FPCR_N | FPCR_DNZ, 0, X_INV, X_INV},
    {nullptr, "4.7.7.11: DNZ, /S: x/denormal is a division by zero", "DIVT",
     QSU | QN, U64(0x3FF0000000000000), U64(0x8000000000000001),
     FPCR_N | FPCR_DNZ, U64(0xFFF0000000000000), X_DZE, X_DZE},
    {nullptr, "4.7.7.11: DNZ, /S: SQRT of a negative denormal is -0", "SQRTT",
     QSU | QN, 0, U64(0x8000000000000001), FPCR_N | FPCR_DNZ, SIGN, 0, 0},
    /* NaNs (4.7.10.1, 4.7.10.3, 4.7.10.4) */
    {nullptr, "4.7.10.4 rule 1: Fb's quiet NaN", "ADDT", QN,
     U64(0x7FF8000000000001), U64(0x7FF8000000000002), FPCR_N,
     U64(0x7FF8000000000002), 0, 0},
    {nullptr, "4.7.10.4 rule 2: Fb's signalling NaN, quieted", "ADDT", QN,
     U64(0x7FF8000000000001), U64(0x7FF0000000000002), FPCR_N,
     U64(0x7FF8000000000002), X_INV, X_INV},
    {nullptr, "4.7.10.4 rule 4: Fa's signalling NaN, quieted", "MULT", QN,
     U64(0xFFF0000000000003), U64(0x3FF0000000000000), FPCR_N,
     U64(0xFFF8000000000003), X_INV, X_INV},
    {nullptr, "4.7.10.3 Inf - Inf: the canonical quiet NaN", "SUBT", QN,
     U64(0x7FF0000000000000), U64(0x7FF0000000000000), FPCR_N, CQNANV, X_INV,
     X_INV},
    {nullptr, "4.7.10.1 CVTTS of a NaN clears <28:0>", "CVTTS", QN, 0,
     U64(0x7FF8000012345678), FPCR_N, U64(0x7FF8000000000000), 0, 0},
    {nullptr, "HRM Table A-11 0 * Inf: CQNaN, invalid", "MULT", QN, 0,
     U64(0x7FF0000000000000), FPCR_N, CQNANV, X_INV, X_INV},
    {nullptr, "HRM Table A-11 Inf/0: Inf, no exception", "DIVT", QN,
     U64(0x7FF0000000000000), 0, FPCR_N, U64(0x7FF0000000000000), 0, 0},
    {nullptr, "HRM Table A-11 A/0: Inf, division by zero", "DIVT", QN,
     U64(0xBFF0000000000000), 0, FPCR_N, U64(0xFFF0000000000000), X_DZE, X_DZE},
    {nullptr, "HRM Table A-11 CMPTLT of a quiet NaN: invalid", "CMPTLT", 0,
     U64(0x7FF8000000000000), 0, FPCR_N, 0, X_INV, X_INV},
    {nullptr, "HRM Table A-11 CMPTEQ of a quiet NaN: no exception", "CMPTEQ", 0,
     U64(0x7FF8000000000000), 0, FPCR_N, 0, 0, 0},
    {nullptr, "HRM Table A-11 CMPTUN of a quiet NaN: true", "CMPTUN", 0,
     U64(0x7FF8000000000000), 0, FPCR_N, FTRUE, 0, 0},
    {nullptr, "CMPTEQ: -0 equals +0", "CMPTEQ", 0, SIGN, 0, FPCR_N, FTRUE, 0,
     0},
    /* conversions to integer (4.7.7.9, 4.7.10.1, Table A-11) */
    {"FP-05", "4.7.7.9: 2^63 overflows, low 64 bits", "CVTTQ", QC, 0,
     U64(0x43E0000000000000), FPCR_N, U64(0x8000000000000000), X_IOV, 0},
    {"FP-05", "4.7.7.9: /V traps the integer overflow", "CVTTQ", QU | QC, 0,
     U64(0x43E0000000000000), FPCR_N, U64(0x8000000000000000), X_IOV, X_IOV},
    {nullptr, "HRM Table A-11 with /S: integer overflow is also invalid",
     "CVTTQ", QSU | QC, 0, U64(0x43E0000000000000), FPCR_N,
     U64(0x8000000000000000), X_IOV | X_INV, X_IOV | X_INV},
    {"FP-05", "4.7.7.2: without /V an overflow does not trap", "CVTTQ", QC, 0,
     U64(0x43F0000000000001), FPCR_N, U64(0x0000000000001000) << 0, X_IOV, 0},
    {nullptr, "-2^63 is in range", "CVTTQ", QU | QC, 0, U64(0xC3E0000000000000),
     FPCR_N, U64(0x8000000000000000), 0, 0},
    {nullptr, "4.7.10.1: NaN converts to 0, invalid", "CVTTQ", QC, 0,
     U64(0x7FF8000000000000), FPCR_N, 0, X_INV, X_INV},
    {nullptr, "4.7.10.1: Inf converts to 0, invalid", "CVTTQ", QC, 0,
     U64(0xFFF0000000000000), FPCR_N, 0, X_INV, X_INV},
    {nullptr, "normal rounding: 2.5 -> 2", "CVTTQ", QN, 0,
     U64(0x4004000000000000), FPCR_N, 2, X_INE, 0},
    {nullptr, "minus infinity: -2.5 -> -3", "CVTTQ", QM, 0,
     U64(0xC004000000000000), FPCR_N, U64(0xFFFFFFFFFFFFFFFD), X_INE, 0},
    {nullptr, "plus infinity (/D): 2.5 -> 3", "CVTTQ", QD, 0,
     U64(0x4004000000000000), U64(3) << 58, 3, X_INE, 0},
    {nullptr, "/SVI traps the inexact", "CVTTQ", QSUI | QC, 0,
     U64(0x3FF8000000000000), FPCR_N, 1, X_INE, X_INE},
    {nullptr, "CVTQS: 2^24+1 rounds to even", "CVTQS", QN, 0, U64(0x1000001),
     FPCR_N, U64(0x4170000000000000), X_INE, 0},
    {nullptr, "CVTQS/D plus infinity: 2^24+1 -> 2^24+2", "CVTQS", QD, 0,
     U64(0x1000001), U64(3) << 58, U64(0x4170000020000000), X_INE, 0},
    {nullptr, "CVTQT: 2^53+1 rounds to even", "CVTQT", QN, 0,
     U64(0x20000000000001), FPCR_N, U64(0x4340000000000000), X_INE, 0},
    {nullptr, "CVTTS: above S MAX + 1/2 LSB overflows", "CVTTS", QN, 0,
     U64(0x47EFFFFFF0000000), FPCR_N, U64(0x7FF0000000000000), X_OVF | X_INE,
     X_OVF},
    {"DENORM", "CVTTS/SU, UNFD: 1.5 * 2^-149 rounds to the even 2^-148",
     "CVTTS", QSU | QN, 0, U64(0x36A8000000000000), FPCR_N | FPCR_UNFD,
     U64(0x0000000040000000), X_UNF | X_INE, 0},
    {nullptr, "CVTST of an S denormal is exact", "CVTST", 0, 0,
     U64(0x0000000020000000), FPCR_N, U64(0x36A0000000000000), 0, 0},
    {nullptr, "CVTST/S, DNZ: an S denormal is a signed zero", "CVTST", QS, 0,
     U64(0x8000000020000000), FPCR_N | FPCR_DNZ, SIGN, 0, 0},
    {"FP-06", "CVTST without /S ignores DNZ", "CVTST", 0, 0,
     U64(0x0000000020000000), FPCR_N | FPCR_DNZ, U64(0x36A0000000000000), 0, 0},

    /* VAX (ARM 4.7.1, 4.7.5, 4.7.7.1, 4.10) */
    {"FP-01", "4.7.1: F result canonical: 1 + 2^-30 is 1.0 exactly", "ADDF", QN,
     U64(0x4010000000000000), U64(0x3E30000000000000), 0,
     U64(0x4010000000000000), 0, 0},
    {"FP-01", "4.7.1: chopped F result canonical", "ADDF", QC,
     U64(0x4010000000000000), U64(0x3E30000000000000), 0,
     U64(0x4010000000000000), 0, 0},
    {"FP-01", "4.10 CVTGF rounds to F precision: 1/3", "CVTGF", QN, 0,
     U64(0x3FF5555555555555), 0, U64(0x3FF5555560000000), 0, 0},
    {"FP-01", "4.10 CVTGF chopped: 1/3", "CVTGF", QC, 0,
     U64(0x3FF5555555555555), 0, U64(0x3FF5555540000000), 0, 0},
    {nullptr, "4.7.5 VAX normal rounding is biased: CVTQF 2^24+1 -> 2^24+2",
     "CVTQF", QN, 0, U64(0x1000001), 0, U64(0x4190000020000000), 0, 0},
    {"FP-01", "4.7.1: CVTQF result canonical: 2^30+1 -> 2^30", "CVTQF", QN, 0,
     U64(0x40000001), 0, U64(0x41F0000000000000), 0, 0},
    {nullptr, "4.7.5 VAX normal rounding: 1 + 2^-53 rounds away", "ADDG", QN,
     U64(0x4010000000000000), U64(0x3CC0000000000000), 0,
     U64(0x4010000000000001), 0, 0},
    {nullptr, "4.7.7.1: a reserved operand traps, also with /S", "ADDG",
     QS | QN, SIGN, 0, 0, 0, 0, X_INV},
    {"FP-03", "4.7.7.1: a dirty zero traps without /S", "ADDG", QN,
     U64(0x0000000000000001), U64(0x4010000000000000), 0,
     U64(0x4010000000000000), 0, X_INV},
    {nullptr, "4.7.7.1: /S treats a dirty zero as zero", "ADDG", QS | QN,
     U64(0x0000000000000001), U64(0x4010000000000000), 0,
     U64(0x4010000000000000), 0, 0},
    {"FP-04", "CMPGEQ/S: a dirty zero equals zero", "CMPGEQ", QS, 0,
     U64(0x0000000000000001), 0, FTRUE, 0, 0},
    {"FP-04", "CMPGLT/S: a dirty zero is not above zero", "CMPGLT", QS, 0,
     U64(0x0000000000000001), 0, 0, 0, 0},
    {"FP-10", "4.7.7.1: overflow traps; the result is the G MAX", "MULG", QN,
     U64(0x7FFFFFFFFFFFFFFF), U64(0x7FFFFFFFFFFFFFFF), 0,
     U64(0x7FFFFFFFFFFFFFFF), 0, X_OVF},
    {nullptr, "4.7.7.1: an underflow is a zero without a trap", "MULG", QN,
     U64(0x0010000000000000), U64(0x0010000000000000), 0, 0, 0, 0},
    {nullptr, "4.7.7.1: /U traps the underflow, result zero", "MULG", QU | QN,
     U64(0x0010000000000000), U64(0x0010000000000000), 0, 0, 0, X_UNF},
    {nullptr, "CVTGQ: 2.5 -> 3 (biased)", "CVTGQ", QN, 0,
     U64(0x4024000000000000), 0, 3, 0, 0},
    {nullptr, "CVTGQ/C: 2.5 -> 2", "CVTGQ", QC, 0, U64(0x4024000000000000), 0,
     2, 0, 0},
    {nullptr, "CVTGQ: -2.5 -> -3", "CVTGQ", QN, 0, U64(0xC024000000000000), 0,
     U64(0xFFFFFFFFFFFFFFFD), 0, 0},
    {"FP-12", "4.7.7.9: CVTGQ/V of -2^63 is in range", "CVTGQ", QU | QN, 0,
     U64(0xC400000000000000), 0, U64(0x8000000000000000), 0, 0},
    {nullptr, "4.7.7.9: CVTGQ/V of 2^63 overflows", "CVTGQ", QU | QN, 0,
     U64(0x4400000000000000), 0, U64(0x8000000000000000), 0, X_IOV},
    {"FP-10", "CVTGD of 2^200 overflows D; the result is the D MAX", "CVTGD",
     QN, 0, U64(0x4C90000000000000), 0, U64(0x7FFFFFFFFFFFFFFF), 0, X_OVF},
};

static int run_hands() {
  const std::vector<Op> ops = make_ops();
  int bad = 0, xfail = 0;
  for (const Hand &h : hands) {
    const Op *op = nullptr;
    for (const Op &o : ops)
      if (!strcmp(o.name, h.op))
        op = &o;
    if (!op) {
      printf("  hand table: no op %s\n", h.op);
      bad++;
      continue;
    }
    const u32 ins = encode(*op, op->base | h.fn);
    const Ctx c = ctx_of(ins, h.fpcr, op->vax());
    Expect e;
    e.res = h.res;
    e.det = op->vax() ? 0 : h.det;
    e.trap = h.trap;
    /* the model must agree with the hand table... */
    const Expect m =
        op->vax() ? vax_model(*op, c, h.a, h.b) : ieee_model(*op, c, h.a, h.b);
    const Outcome hv = derive(c, e, h.res);
    if (!matches(c, m, hv)) {
      printf("  HAND/MODEL %s %s: model res=%016" PRIx64 " det=%02" PRIx64
             " trap=%02" PRIx64 "\n",
             h.op, h.cite, m.res, m.det, m.trap);
      bad++;
    }
    /* ...and the code with both */
    const Outcome got = run<CAlphaCPU>(ins, h.a, h.b, h.fpcr);
    if (!matches(c, e, got)) {
      if (h.item && !item(h.item)->fixed) {
        xfail++;
        if (verbose)
          report("expected(hand)", *op, ins, h.a, h.b, h.fpcr, got, &hv,
                 h.item);
      } else {
        bad++;
        printf("  HAND %s\n", h.cite);
        report("HAND", *op, ins, h.a, h.b, h.fpcr, got, &hv, h.item);
      }
    } else if (h.item && !item(h.item)->fixed) {
      bad++;
      printf("  HAND passes but %s is not marked fixed: %s\n", h.item, h.cite);
    }
  }
  printf("hand table: %d cases, %d failed, %d expected (items not fixed)\n",
         (int)(sizeof hands / sizeof hands[0]), bad, xfail);
  return bad;
}

/* ------------------------------------------------------------------------ */
/* Loads, stores and the non-arithmetic operates                            */
/* ------------------------------------------------------------------------ */

static int run_misc() {
  int bad = 0;
  CAlphaCPU cpu;
  memset(&cpu.state, 0, sizeof cpu.state);
  for (long k = 0; k < 200000; k++) {
    const u32 w = (u32)rnd64();
    /* LDS/STS (ARM 4.8.3, 4.8.7) and LDF/STF (4.8.1, 4.8.5) */
    if (cpu.ieee_lds(w) != s_to_reg(w) || cpu.ieee_sts(s_to_reg(w)) != w)
      bad++;
    const u64 fr = f_to_reg(w);
    if (cpu.vax_ldf(w) != fr || (fr & LOW29))
      bad++;
    if (((w >> 7) & 0xff) && cpu.vax_stf(fr) != w)
      bad++;
  }
  /* MT_FPCR/MF_FPCR: SUM derived, bits 47:0 read as zero (HRM Table 2-14) */
  const u64 ins_mt = (0x17u << 26) | (1u << 21) | (1u << 16) | (0x024 << 5) | 1;
  const u64 ins_mf = (0x17u << 26) | (4u << 21) | (4u << 16) | (0x025 << 5) | 4;
  cpu.state.fpen = true;
  cpu.state.f[1] = U64(0x0100FFFFFFFFFFFF);
  cpu.fp_exec((u32)ins_mt);
  cpu.fp_exec((u32)ins_mf);
  if (cpu.state.f[4] != U64(0x8100000000000000))
    bad++;
  /* CVTQL: integer overflow sets IOV; /V traps (4.7.7.9) */
  const u32 cvtql_v =
      (0x17u << 26) | (31u << 21) | (2u << 16) | (0x130 << 5) | 3;
  Outcome o = run<CAlphaCPU>(cvtql_v, 0, U64(0x100000000), FPCR_N);
  if (!(o.fpcr & FPCR_IOV) || !(o.exc & X_IOV) || !o.trapped)
    bad++;
  o = run<CAlphaCPU>(cvtql_v, 0, U64(0xFFFFFFFF80000000), FPCR_N);
  if (o.trapped || o.res != U64(0x8000000000000000))
    bad++;
  printf("loads, stores, FPCR, CVTQL: %d failed\n", bad);
  return bad;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-v"))
      verbose = true;
    else if (!strcmp(argv[i], "-n") && i + 1 < argc)
      n_random = atol(argv[++i]);
  }
  int bad = run_misc();
  bad += run_hands();
  const std::vector<Op> ops = make_ops();
  for (const Op &op : ops) {
    const long before = n_cases, fbefore = n_failed;
    sweep(op);
    if (verbose || n_failed != fbefore)
      printf("%-7s %9ld cases, %ld failed\n", op.name, n_cases - before,
             n_failed - fbefore);
  }
  printf("sweep: %ld cases", n_cases);
#if defined(FP_HAVE_FROZEN)
  printf(", %ld compared with the frozen code", n_frozen_cmp);
#else
  printf(" (built without the frozen code: no baseline comparison)");
#endif
  printf(", %ld failed\n", n_failed);
  printf("items:\n");
  for (const Item &it : items)
    printf("  %-6s %-8s model mismatches %8ld, frozen differences %8ld  %s\n",
           it.id, it.fixed ? "fixed" : "expected", it.model_mismatch,
           it.frozen_explained, it.what);
  bad += (int)(n_failed > 0);
  for (const Item &it : items)
    if (!it.fixed && it.model_mismatch == 0) {
      printf("item %s is not marked fixed but nothing mismatches the model\n",
             it.id);
    }
  printf("%s\n", bad ? "FAIL" : "PASS");
  return bad ? 1 : 0;
}
