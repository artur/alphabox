/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AlphaZ: the EV8 page. The 21464's own AMASK bits, and a live check of a
 * few of its opcode 1C short-vector SIMD and opcode 07 paired-single
 * instructions against C models of what Alphabox's EV8 sheet says they do
 * (docs/platforms/ev8-isa.md, sections 3.3 and 3.4).
 * Copyright (C) 2026 Artur Goulão
 *
 * AlphaZ is built for the 21264 (-mcpu=ev6), so the assembler takes none
 * of these mnemonics: each EV8 instruction is its word, a .long, in a
 * function of its own with no prologue, the operands in $a0 and $a1 and
 * the result in $v0 ($f10, $f11 and $f12 for paired single, reached with
 * ITOFT and FTOIT). build.sh checks every word here against the word
 * nada's own assembler writes for the mnemonic in its comment at
 * -mcpu=ev8. Nothing calls them unless AMASK has said bit 11 is present:
 * an EV6 or EV7 never executes one. */
#include <string.h>

#include "az.h"

#ifdef __nada_alpha__
/* AZ_EV8_INT(name, word, mnemonic): an integer operate instruction;
 * AZ_EV8_PAIR(name, word, mnemonic): a paired-single one. build.sh reads
 * these lines. */
#define AZ_EV8_INT(name, word, mnemonic) \
    static u64 __attribute__((naked)) name(u64 a, u64 b) \
    { __asm__("    .long " word "\n    ret\n"); }
#define AZ_EV8_PAIR(name, word, mnemonic) \
    static u64 __attribute__((naked)) name(u64 a, u64 b) \
    { __asm__("    itoft $a0, $f10\n    itoft $a1, $f11\n    .long " word "\n    ftoit $f12, $v0\n    ret\n"); }

AZ_EV8_INT(w_vaddub8, "0x72110200", "vaddub8 $a0, $a1, $v0")
AZ_EV8_INT(w_vaddsw4, "0x72110260", "vaddsw4 $a0, $a1, $v0")
AZ_EV8_INT(w_vsubub8, "0x72110300", "vsubub8 $a0, $a1, $v0")
AZ_EV8_INT(w_tabserrub8, "0x72110900", "tabserrub8 $a0, $a1, $v0")
AZ_EV8_INT(w_tmulsw4, "0x72110a60", "tmulsw4 $a0, $a1, $v0")
AZ_EV8_INT(w_pkuwb8, "0x72110500", "pkuwb8 $a0, $a1, $v0")
AZ_EV8_INT(w_upkubw4, "0x73f00580", "upkubw4 $a0, $v0")
AZ_EV8_PAIR(w_pmul, "0x1d4b100c", "pmul $f10, $f11, $f12")
AZ_EV8_PAIR(w_padd, "0x1d4b120c", "padd $f10, $f11, $f12")
AZ_EV8_PAIR(w_psub, "0x1d4b128c", "psub $f10, $f11, $f12")
AZ_EV8_PAIR(w_phadd, "0x1d4b124c", "phadd $f10, $f11, $f12")
AZ_EV8_PAIR(w_pcmplt, "0x1d4b158c", "pcmplt $f10, $f11, $f12")
#endif

/* ---- the models: the sheet's operations in C ---- */

static u64 byte_of(u64 x, int i) { return (x >> (8 * i)) & 0xff; }
static u64 word_of(u64 x, int i) { return (x >> (16 * i)) & 0xffff; }
static s64 sword_of(u64 x, int i)
{
    s64 v = (s64)word_of(x, i);
    return v >= 32768 ? v - 65536 : v;
}
static s64 clamp(s64 v, s64 lo, s64 hi) { return v < lo ? lo : v > hi ? hi : v; }

/* VADDUB8: each byte, the unsigned sum saturated to 0..255 */
static u64 m_vaddub8(u64 a, u64 b)
{
    u64 r = 0;
    int i;
    for (i = 0; i < 8; i++) r |= (u64)clamp((s64)(byte_of(a, i) + byte_of(b, i)), 0, 255) << (8 * i);
    return r;
}

/* VADDSW4: each word, the signed sum saturated to -32768..32767 */
static u64 m_vaddsw4(u64 a, u64 b)
{
    u64 r = 0;
    int i;
    for (i = 0; i < 4; i++) r |= ((u64)clamp(sword_of(a, i) + sword_of(b, i), -32768, 32767) & 0xffff) << (16 * i);
    return r;
}

/* VSUBUB8: each byte, the unsigned difference saturated at 0 */
static u64 m_vsubub8(u64 a, u64 b)
{
    u64 r = 0;
    int i;
    for (i = 0; i < 8; i++) r |= (u64)clamp((s64)byte_of(a, i) - (s64)byte_of(b, i), 0, 255) << (8 * i);
    return r;
}

/* TABSERRUB8: the sum of the bytes' absolute differences (PERR's result) */
static u64 m_tabserrub8(u64 a, u64 b)
{
    s64 s = 0, d;
    int i;
    for (i = 0; i < 8; i++) {
        d = (s64)byte_of(a, i) - (s64)byte_of(b, i);
        s += d < 0 ? -d : d;
    }
    return (u64)s;
}

/* TMULSW4: the sum of the signed words' products, as a 64-bit integer */
static u64 m_tmulsw4(u64 a, u64 b)
{
    s64 s = 0;
    int i;
    for (i = 0; i < 4; i++) s += sword_of(a, i) * sword_of(b, i);
    return (u64)s;
}

/* PKUWB8: Ra's four unsigned words saturated to bytes 0-3, Rb's to 4-7 */
static u64 m_pkuwb8(u64 a, u64 b)
{
    u64 r = 0;
    int i;
    for (i = 0; i < 4; i++) {
        r |= (u64)clamp((s64)word_of(a, i), 0, 255) << (8 * i);
        r |= (u64)clamp((s64)word_of(b, i), 0, 255) << (8 * (4 + i));
    }
    return r;
}

/* UPKUBW4: Rb's four low bytes zero-extended to words */
static u64 m_upkubw4(u64 a, u64 b)
{
    u64 r = 0;
    int i;
    (void)b;
    for (i = 0; i < 4; i++) r |= byte_of(a, i) << (16 * i);
    return r;
}

/* paired single: the high single in <63:32>, the low in <31:0> */
union az_single {
    u32 u;
    float f;
};
static float half(u64 x, int hi)
{
    union az_single s;
    s.u = (u32)(hi ? x >> 32 : x);
    return s.f;
}
static u64 pair(float h, float l)
{
    union az_single a, b;
    a.f = h;
    b.f = l;
    return (u64)a.u << 32 | b.u;
}
static u64 pair_of(float h, float l) { return pair(h, l); }

static u64 m_pmul(u64 a, u64 b) { return pair(half(a, 1) * half(b, 1), half(a, 0) * half(b, 0)); }
static u64 m_padd(u64 a, u64 b) { return pair(half(a, 1) + half(b, 1), half(a, 0) + half(b, 0)); }
static u64 m_psub(u64 a, u64 b) { return pair(half(a, 1) - half(b, 1), half(a, 0) - half(b, 0)); }
/* PHADD: the high half the sum of Fa's halves, the low half Fb's */
static u64 m_phadd(u64 a, u64 b) { return pair(half(a, 1) + half(a, 0), half(b, 1) + half(b, 0)); }
/* PCMPLT: each half IEEE 1.0 where Fa's is less than Fb's, else 0 */
static u64 m_pcmplt(u64 a, u64 b)
{
    u64 h = half(a, 1) < half(b, 1) ? 0x3f800000ULL : 0;
    u64 l = half(a, 0) < half(b, 0) ? 0x3f800000ULL : 0;
    return h << 32 | l;
}

/* ---- the check ---- */

typedef u64 (*az_op)(u64, u64);

struct az_case {
    const char *name;
    const char *what;
    az_op insn, model;
    u64 a, b;
};

static struct az_case cases[16];
static int ncases;
static u64 got[16], want[16];
static int ran, failures;

static void add_case(const char *name, const char *what, az_op insn, az_op model, u64 a, u64 b)
{
    struct az_case *c = &cases[ncases++];
    c->name = name;
    c->what = what;
    c->insn = insn;
    c->model = model;
    c->a = a;
    c->b = b;
}

/* The operands push each instruction to its edges: sums past 255 and
 * past 32767, differences below 0, words over 255 to pack. Every paired
 * single operand and result is exact, so no rounding enters. */
static void cases_init(void)
{
    ncases = 0;
#ifdef __nada_alpha__
    add_case("vaddub8", "bytes, unsigned, saturating", w_vaddub8, m_vaddub8, 0x01807fff10f0c048ULL, 0x017f80ff10101080ULL);
    add_case("vaddsw4", "words, signed, saturating", w_vaddsw4, m_vaddsw4, 0x7ff08000123400ffULL, 0x0100ff000001ff80ULL);
    add_case("vsubub8", "bytes, unsigned, saturating", w_vsubub8, m_vsubub8, 0x0010ff0120300040ULL, 0x0020010010300180ULL);
    add_case("tabserrub8", "sum of byte differences", w_tabserrub8, m_tabserrub8, 0x00ff7f8001020304ULL, 0xff00807f04030201ULL);
    add_case("tmulsw4", "sum of signed word products", w_tmulsw4, m_tmulsw4, 0x7fff8000ffff0003ULL, 0x7fff7fff00020005ULL);
    add_case("pkuwb8", "words to bytes, saturating", w_pkuwb8, m_pkuwb8, 0x0100008000ff0010ULL, 0xffff004000010000ULL);
    add_case("upkubw4", "bytes to words", w_upkubw4, m_upkubw4, 0x12345678ff807f01ULL, 0);
    add_case("pmul", "paired single multiply", w_pmul, m_pmul, pair_of(1.5f, -0.5f), pair_of(2.0f, 4.0f));
    add_case("padd", "paired single add", w_padd, m_padd, pair_of(1.25f, -3.0f), pair_of(0.75f, 1.0f));
    add_case("psub", "paired single subtract", w_psub, m_psub, pair_of(8.0f, 0.5f), pair_of(2.5f, 1.5f));
    add_case("phadd", "paired single across-add", w_phadd, m_phadd, pair_of(1.0f, 2.0f), pair_of(-4.0f, 0.5f));
    add_case("pcmplt", "paired single compare", w_pcmplt, m_pcmplt, pair_of(1.0f, 5.0f), pair_of(2.0f, 3.0f));
#endif
}

int az_ev8_check(void)
{
    int i;
    cases_init();
    failures = 0;
    for (i = 0; i < ncases; i++) {
        got[i] = cases[i].insn(cases[i].a, cases[i].b);
        want[i] = cases[i].model(cases[i].a, cases[i].b);
        if (got[i] != want[i]) failures++;
    }
    ran = 1;
    return failures;
}

void az_ev8_rows(void)
{
    char src[AZ_SOURCE];
    int i, simd = (az_cpu.present & AZ_AMASK_SIMD) != 0;
    az_add(AZ_EV8, "IMPLVER", "instruction IMPLVER", "%llu: %s", az_cpu.implver, az_implver_name(az_cpu.implver));
    az_add(AZ_EV8, "SMT", "AMASK bit 10", "%s", az_cpu.present & AZ_AMASK_SMT ? "present: LDL_ARM, LDQ_ARM, QUIESCE" : "absent");
    az_add(AZ_EV8, "SIMD and paired single", "AMASK bit 11", "%s", simd ? "present: opcode 1C SIMD, opcode 07 paired single" : "absent");
    az_add(AZ_EV8, "Tarantula", "AMASK bit 3", "%s",
           az_cpu.present & AZ_AMASK_TARANTULA ? "present: the vector extension" : "absent: no vector instruction is run");
    if (!simd) {
        az_add(AZ_EV8, "Self-check", "", "not run: AMASK bit 11 absent, so no EV8 instruction is executed");
        return;
    }
    if (!ran) az_ev8_check();
    az_add(AZ_EV8, "Self-check", "instructions against C models", "%d of %d agree with the sheet's operations",
           ncases - failures, ncases);
    for (i = 0; i < ncases; i++) {
        az_snprintf(src, sizeof src, "instruction %s, %s", cases[i].name, cases[i].what);
        if (got[i] == want[i])
            az_add(AZ_EV8, cases[i].name, src, "ok %016llx", got[i]);
        else
            az_add(AZ_EV8, cases[i].name, src, "MISMATCH %016llx, C says %016llx", got[i], want[i]);
    }
}

/* ---- the SIMD loop: vaddub8 over 256 quadwords, a call a quadword,
 * against the C model on the same data ---- */

#define AZ_QW 256
static u64 sa[AZ_QW], sb[AZ_QW], sc[AZ_QW];

static void simd_round(az_op op)
{
    int i;
    for (i = 0; i < AZ_QW; i++) sc[i] = op(sa[i], sb[i]);
}

static void timed(struct az_bench *b, const char *name, az_op op, u64 (*now)(void), u64 freq, u64 ms)
{
    u64 t0, t, c0, c1, want = freq / 1000 * ms;
    memset(b, 0, sizeof *b);
    b->name = name;
    b->work = AZ_QW * 8;
    t0 = now();
    c0 = az_rpcc() & 0xffffffffULL;
    do {
        simd_round(op);
        b->rounds++;
        t = now();
    } while (t - t0 < want);
    c1 = az_rpcc() & 0xffffffffULL;
    b->ticks = t - t0;
    b->cycles = (c1 - c0) & 0xffffffffULL;
    b->check = sc[0];
}

void az_ev8_bench(u64 (*now)(void), u64 freq, u64 ms, struct az_bench *simd, struct az_bench *scalar)
{
    int i;
    u64 x = 0x0123456789abcdefULL;
    for (i = 0; i < AZ_QW; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        sa[i] = x;
        sb[i] = x >> 7 | x << 57;
    }
#ifdef __nada_alpha__
    if (az_cpu.present & AZ_AMASK_SIMD) {
        timed(simd, "SIMD: vaddub8 (byte adds)", w_vaddub8, now, freq, ms);
        timed(scalar, "The same in C (byte adds)", m_vaddub8, now, freq, ms);
        return;
    }
#endif
    memset(simd, 0, sizeof *simd);
    memset(scalar, 0, sizeof *scalar);
}
