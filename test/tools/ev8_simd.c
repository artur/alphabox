/* Alphabox: the 21464's 1C short-vector SIMD set and 07 paired single
 * (docs/platforms/ev8-isa.md 3.3 and 3.4), run by a Linux program on the
 * `ev8` row and checked against scalar C. Stage S5/S6 of
 * docs/platforms/ev8.md; test/tools/ev8_simd_guest.sh compiles it in the
 * linux-es47 guest with its own gcc. binutils knows none of the words, so
 * each is a .long with its registers fixed: the integer operands in $1 and
 * $2, the result in $3; the paired ones in $f1, $f2 and $f3, moved through
 * memory (LDT/STT copy the 64 bits as they are).
 *
 * The kernels use operands whose paired-single results are exact, so that
 * no FP exception is raised at all. Prints "EV8SIMD: PASS" when every
 * result is the scalar code's.
 */
#include <stdio.h>

typedef unsigned long u64;
typedef unsigned int u32;
typedef long s64;

#define SIMD(fn, word)                                                         \
  static u64 fn(u64 a, u64 b) {                                                \
    u64 r;                                                                     \
    __asm__ volatile("mov %1,$1\n\tmov %2,$2\n\t.long " #word "\n\tmov $3,%0"  \
                     : "=r"(r)                                                 \
                     : "r"(a), "r"(b)                                          \
                     : "$1", "$2", "$3");                                      \
    return r;                                                                  \
  }
#define PAIRED(fn, word)                                                       \
  static u64 fn(u64 a, u64 b) {                                                \
    u64 r;                                                                     \
    __asm__ volatile("ldt $f1,0(%1)\n\tldt $f2,0(%2)\n\t.long " #word          \
                     "\n\tstt $f3,0(%0)"                                       \
                     :                                                         \
                     : "r"(&r), "r"(&a), "r"(&b)                               \
                     : "$f1", "$f2", "$f3", "memory");                         \
    return r;                                                                  \
  }

/* 1C words: 0x70220003 | function << 5 (Ra $1, Rb $2, Rc $3) */
SIMD(vaddub8, 0x70220203)
SIMD(vsubsw4, 0x70220363)
SIMD(tabserrub8, 0x70220903)
SIMD(tmulsw4, 0x70220a63)
SIMD(vminmaxsw4, 0x70220463)
SIMD(pkswb8, 0x70220543)
SIMD(permb8, 0x70220103)
SIMD(vsraw4, 0x70220c63)
/* 07 words: 0x1c220003 | function << 5 (Fa $f1, Fb $f2, Fc $f3; the unary
   forms Fa $f31), the plain form's function 0x080 | Fnc */
PAIRED(padd, 0x1c221203)
PAIRED(pmul, 0x1c221003)
PAIRED(phadd, 0x1c221243)
PAIRED(pcmplt, 0x1c221583)
PAIRED(pfmax, 0x1c2215c3)
PAIRED(pcvtfi, 0x1fe21723)
PAIRED(parcpl, 0x1fe21103)

/* AMASK $1,$3 and IMPLVER #1,$3, as words too: the guest's binutils may
   take neither without -mcpu */
SIMD(amask2, 0x47e10c23)
SIMD(implver2, 0x47e03d83)
static u64 amask(u64 m) { return amask2(m, 0); }
static u64 implver(void) { return implver2(0, 0); }

static long checks, bad;
static void check(const char *what, int i, u64 got, u64 want) {
  checks++;
  if (got != want) {
    bad++;
    if (bad < 20)
      printf("EV8SIMD: %s [%d]: %016lx, scalar %016lx\n", what, i, got, want);
  }
}

static u64 seed = 0x2545F4914F6CDD1DUL;
static u64 rnd(void) {
  seed = seed * 6364136223846793005UL + 1442695040888963407UL;
  return seed ^ (seed >> 29);
}
static s64 el(u64 x, int w, int i) { /* signed element */
  s64 v = (s64)((x >> (w * i)) & ((1UL << w) - 1));
  return (v >> (w - 1)) ? v - (1L << w) : v;
}
static u64 uel(u64 x, int w, int i) { return (x >> (w * i)) & ((1UL << w) - 1); }
static u32 fbits(float f) {
  union { float f; u32 u; } x;
  x.f = f;
  return x.u;
}
static u64 pr(float h, float l) { return ((u64)fbits(h) << 32) | fbits(l); }
static float hf(u64 x) {
  union { float f; u32 u; } v;
  v.u = (u32)(x >> 32);
  return v.f;
}
static float lf(u64 x) {
  union { float f; u32 u; } v;
  v.u = (u32)x;
  return v.f;
}

int main(void) {
  int i, k;
  printf("EV8SIMD: IMPLVER %lu, AMASK of bit 11: %lu (0: the set is there)\n",
         implver(), amask(1UL << 11));
  for (i = 0; i < 200; i++) {
    const u64 a = rnd(), b = rnd();
    u64 w = 0;
    s64 s = 0;
    /* a brightness step: unsigned bytes, saturating */
    for (k = 0; k < 8; k++) {
      u64 v = uel(a, 8, k) + uel(b, 8, k);
      w |= (v > 255 ? 255 : v) << (8 * k);
    }
    check("vaddub8", i, vaddub8(a, b), w);
    w = 0;
    for (k = 0; k < 4; k++) {
      s64 v = el(a, 16, k) - el(b, 16, k);
      v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
      w |= ((u64)v & 0xffff) << (16 * k);
    }
    check("vsubsw4", i, vsubsw4(a, b), w);
    /* a block-matching sum of absolute differences */
    s = 0;
    for (k = 0; k < 8; k++) {
      s64 d = (s64)uel(a, 8, k) - (s64)uel(b, 8, k);
      s += d < 0 ? -d : d;
    }
    check("tabserrub8", i, tabserrub8(a, b), (u64)s);
    /* a dot product of four 16-bit lanes */
    s = 0;
    for (k = 0; k < 4; k++)
      s += el(a, 16, k) * el(b, 16, k);
    check("tmulsw4", i, tmulsw4(a, b), (u64)s);
    /* packing words into bytes, saturating */
    w = 0;
    for (k = 0; k < 8; k++) {
      s64 v = el(k < 4 ? a : b, 16, k & 3);
      v = v > 127 ? 127 : v < -128 ? -128 : v;
      w |= ((u64)v & 0xff) << (8 * k);
    }
    check("pkswb8", i, pkswb8(a, b), w);
    /* an arithmetic shift of every word */
    w = 0;
    for (k = 0; k < 4; k++)
      w |= ((u64)(el(a, 16, k) >> (b & 15)) & 0xffff) << (16 * k);
    check("vsraw4", i, vsraw4(a, b), w);
    /* a byte reverse: selectors 7..0 */
    w = 0;
    for (k = 0; k < 8; k++)
      w |= uel(a, 8, 7 - k) << (8 * k);
    check("permb8", i, permb8(a, 0x01234567UL), w);
  }
  /* the running minimum and maximum of a 64-word array */
  {
    u64 run = 0x7fffUL | (0x8000UL << 32);
    s64 mn = 32767, mx = -32768;
    for (i = 0; i < 16; i++) {
      const u64 a = rnd();
      run = vminmaxsw4(a, run);
      for (k = 0; k < 4; k++) {
        if (el(a, 16, k) < mn)
          mn = el(a, 16, k);
        if (el(a, 16, k) > mx)
          mx = el(a, 16, k);
      }
    }
    check("vminmaxsw4", 0, run,
          ((u64)mn & 0xffffffffUL) | (((u64)mx & 0xffffffffUL) << 32));
  }
  /* paired single: y = a * x + y over pairs, a horizontal sum, compares,
     a max, a conversion, a reciprocal; small integers and quarters, so
     every result is exact */
  for (i = 0; i < 100; i++) {
    const float x0 = (float)((int)(rnd() % 64) - 32) / 4,
                x1 = (float)((int)(rnd() % 64) - 32) / 4;
    const float y0 = (float)((int)(rnd() % 64) - 32),
                y1 = (float)((int)(rnd() % 64) - 32);
    const float c = 3.0f;
    const u64 x = pr(x0, x1), y = pr(y0, y1), cc = pr(c, c);
    const u64 ax = pmul(cc, x);
    check("pmul", i, ax, pr(c * x0, c * x1));
    const u64 yy = padd(ax, y);
    check("padd", i, yy, pr(c * x0 + y0, c * x1 + y1));
    check("phadd", i, phadd(x, y), pr(x0 + x1, y0 + y1));
    check("pcmplt", i, pcmplt(x, y),
          pr(x0 < y0 ? 1.0f : 0.0f, x1 < y1 ? 1.0f : 0.0f));
    check("pfmax", i, pfmax(x, y), pr(x0 < y0 ? y0 : x0, x1 < y1 ? y1 : x1));
    check("pcvtfi", i, pcvtfi(0, y),
          ((u64)(u32)(int)y0 << 32) | (u32)(int)y1);
    {
      const int e0 = (int)(rnd() % 20) - 10, e1 = (int)(rnd() % 20) - 10;
      float p0 = 1, p1 = 1;
      for (k = 0; k < (e0 < 0 ? -e0 : e0); k++)
        p0 = e0 < 0 ? p0 / 2 : p0 * 2;
      for (k = 0; k < (e1 < 0 ? -e1 : e1); k++)
        p1 = e1 < 0 ? p1 / 2 : p1 * 2;
      check("parcpl", i, parcpl(0, pr(p0, p1)), pr(1 / p0, 1 / p1));
    }
    (void)hf;
    (void)lf;
  }
  printf("EV8SIMD: %ld checks, %ld mismatches\n", checks, bad);
  printf("EV8SIMD: %s\n", bad ? "FAIL" : "PASS");
  return bad != 0;
}
