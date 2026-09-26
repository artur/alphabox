/* axpbench -- an Alpha/NT benchmark for alphabox, one section per JIT datapath.
 *
 * Built with nada (`-t alpha-windows`), so this is a real PE32 running as a
 * real NT process, not a batch loop. It exists because the workload we had --
 * cmd.exe running `for /l` -- is integer-only with a handful of hot code pages,
 * and so cannot show pressure on the FP path, the address path, the block cache
 * or calls. Each section isolates one of those, and each reports its own time,
 * so a change can be attributed instead of guessed at.
 *
 * Read the numbers as ratios between builds, never between sections: the
 * sections do different amounts of work on purpose.
 *
 * Two properties of nada's output matter when reading results. It runs no
 * optimizer, so values move through memory far more than MSVC's would -- which
 * stresses the address path harder than typical NT code. And it targets the
 * base architecture as an EV4/EV5 has it: no BWX, so every byte access is
 * ldq_u/extbl/insbl/mskbl, which is what `byte` below is for.
 *
 *   axpbench.exe [section] [scale]     section: all (default) or one name
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N_STRIDE (6 << 20) /* bigger than any plausible page-cache reach */

/* A pause before each section. The emulator's idle pacing turns it into a
 * stretch of near-zero MIPS in ALPHABOX_RATE's report, so perf_ab.py can
 * tell the sections apart and give each its own MIPS from the same run. */
void __stdcall Sleep(unsigned long ms);
#define SECTION_GAP_MS 300

static unsigned long lfsr_state = 0xACE1u;
static unsigned long lfsr(void) { /* deterministic, data-dependent branches */
  lfsr_state = (lfsr_state >> 1) ^ (-(lfsr_state & 1ul) & 0xB400u);
  return lfsr_state;
}

static long b_alu(long n) { /* integer ALU in registers: emitted code quality */
  long a = 1, b = 2, c = 3, d = 4, i;
  for (i = 0; i < n; i++) {
    a = a * 3 + b; b = b ^ (c << 2); c = c + (d >> 1); d = d - a;
    a &= 0xffffff; b &= 0xffffff; c &= 0xffffff; d &= 0xffffff;
  }
  return a + b + c + d;
}

static long b_branch(long n) { /* unpredictable branches: block linking */
  long hits = 0, i;
  for (i = 0; i < n; i++) {
    unsigned long v = lfsr();
    if (v & 1) hits++;
    else if (v & 2) hits += 2;
    else if (v & 4) hits += 3;
    else hits -= 1;
  }
  return hits;
}

static long leaf(long x) { return x * 3 + 1; }
static long b_call(long n) { /* JSR/RET: what a return stack would address */
  long s = 0, i;
  for (i = 0; i < n; i++) s += leaf(i) + leaf(i + 1) + leaf(i + 2);
  return s;
}

static long b_ldst(long *v, long n, long reps) { /* sequential load/store */
  long s = 0, r, i;
  for (r = 0; r < reps; r++)
    for (i = 0; i < n; i++) { v[i] = v[i] + r; s += v[i]; }
  return s;
}

static long b_stride(long *v, long n, long reps) {
  /* One touch per 8 KB page over a buffer far larger than the page cache: this
   * is the section that moves when the address path changes. */
  long s = 0, r, i;
  const long step = 8192 / (long)sizeof(long);
  for (r = 0; r < reps; r++)
    for (i = 0; i < n; i += step) s += v[i];
  return s;
}

static long b_fp(long n) { /* IEEE double: the FP emitter and the FPCR path */
  double s = 0.0, x;
  long i;
  for (i = 1; i <= n; i++) { x = (double)i; s += (x * 1.5 + 2.0) / (x + 0.25); }
  return (long)s;
}

static long b_byte(char *buf, long n, long reps) {
  /* No BWX on the base architecture, so each of these is a ldq_u/extbl or
   * insbl/mskbl sequence -- several instructions per byte. */
  long s = 0, r, i;
  for (r = 0; r < reps; r++) {
    for (i = 0; i < n; i++) buf[i] = (char)(i + r);
    for (i = 0; i < n; i++) s += buf[i];
  }
  return s;
}

static long b_div(long n) { /* Alpha has no divide: nada emits a loop */
  long s = 0, i;
  for (i = 1; i <= n; i++) s += (n * 7 + i) / i;
  return s;
}

static int cmp_long(const void *a, const void *b) {
  long x = *(const long *)a, y = *(const long *)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}
static long b_sort(long *v, long n, long reps) { /* calls, compares, memory */
  long r, i, acc = 0;
  for (r = 0; r < reps; r++) {
    for (i = 0; i < n; i++)
      v[i] = (long)(lfsr() ^ (unsigned long)(i * 2654435761ul));
    qsort(v, (unsigned long)n, sizeof(long), cmp_long);
    acc += v[0] + v[n - 1];
  }
  return acc;
}

static void report(const char *name, clock_t t0, long r) {
  long ms = (long)((clock() - t0) / (CLOCKS_PER_SEC / 1000));
  printf("%-7s %6ld ms  (%ld)\n", name, ms, r);
}

int main(int argc, char **argv) {
  const char *only = argc > 1 ? argv[1] : "all";
  long scale = argc > 2 ? atoi(argv[2]) : 1;
  long *v, *big;
  char *buf;
  clock_t t0, tall;
  if (scale < 1) scale = 1;

  v = (long *)malloc(sizeof(long) * 262144);
  big = (long *)malloc(sizeof(long) * N_STRIDE);
  buf = (char *)malloc(1 << 20);
  if (!v || !big || !buf) { printf("axpbench: out of memory\n"); return 1; }
  memset(big, 0, sizeof(long) * N_STRIDE);

  printf("axpbench scale=%ld section=%s\n", scale, only);
  tall = clock();
#define RUN(nm, expr)                                                          \
  if (!strcmp(only, "all") || !strcmp(only, nm)) {                             \
    Sleep(SECTION_GAP_MS);                                                     \
    t0 = clock();                                                              \
    report(nm, t0, (expr));                                                    \
  }
  /* Sized from a measured scale=1 run so each section takes about three
   * seconds. That matters more than it looks: NT's clock() advances on the
   * scheduler quantum, so a section that finishes in 7 ms reports quantization
   * noise and nothing else. The first version of this file made exactly that
   * mistake. Keep every section in the seconds, and scale rather than trim. */
  RUN("alu", b_alu(250000000L * scale))
  RUN("branch", b_branch(200000000L * scale))
  RUN("call", b_call(150000000L * scale))
  RUN("ldst", b_ldst(v, 262144, 900L * scale))
  RUN("stride", b_stride(big, N_STRIDE, 1700L * scale))
  RUN("fp", b_fp(80000000L * scale))
  RUN("byte", b_byte(buf, 1 << 20, 200L * scale))
  RUN("div", b_div(15000000L * scale))
  RUN("sort", b_sort(v, 262144, 4L * scale))
  printf("total %ld ms\n", (long)((clock() - tall) / (CLOCKS_PER_SEC / 1000)));
  return 0;
}
