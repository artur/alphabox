/* Threads contending a spin lock the 21464 IDS's way (Rev 1.1k, PDF
 * p. 674): try the lock with LDQ_L/STQ_C; while it is busy, LDQ_ARM it,
 * QUIESCE if it is still taken, and look again. Built on the guest with its
 * own gcc (gcc -O2 -pthread quiesce_lock.c), Linux on an Alphabox ES47 with
 * the `ev8` row (docs/platforms/ev8.md, S3). Binutils knows neither
 * instruction, so they are words: LDQ_ARM $1,($2) = 0x6022B400 and QUIESCE
 * = 0x63FFB800 (docs/platforms/ev8-isa.md 3.1), and both are used only when
 * AMASK says bit 10 (SMT) is there, as the IDS requires. Each of the
 * `threads` (two by default; one per processor, its TPUs included, with S4's
 * `tpus`) is pinned to a processor of its own, 0, 1, ...; each adds 1 to a
 * shared count, inside the lock, `n` times, and holds the lock a while each
 * time so that the others wait. Usage: quiesce_lock [n [hold [threads]]].
 * Prints the counts, the QUIESCEs each executed, the AMASK test and Linux's
 * AT_HWCAP (its ELF_HWCAP: ~AMASK(-1)), and "QTEST PASS" when the count is
 * exact and every thread finished.
 * Alphabox's ALPHABOX_TRACE_QUIESCE=1 tells what woke each QUIESCE. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>

static volatile long lock_word __attribute__((aligned(64)));
static volatile long shared_count;
static long n = 20000, hold = 200;
static int smt;

static long amask(long bits) {
  long r;
  __asm__ __volatile__("amask %1,%0" : "=r"(r) : "r"(bits));
  return r;
}

static long ldq_arm(volatile long *p) {
  register long v __asm__("$1");
  register volatile long *a __asm__("$2") = p;
  __asm__ __volatile__(".long 0x6022B400" : "=r"(v) : "r"(a) : "memory");
  return v;
}

static void quiesce(void) {
  __asm__ __volatile__(".long 0x63FFB800" ::: "memory");
}

static int try_lock(volatile long *l) {
  long tmp, ok;
  __asm__ __volatile__("1: ldq_l %0,%2\n"
                       "   bne %0,2f\n"
                       "   lda %1,1\n"
                       "   stq_c %1,%2\n"
                       "   beq %1,1b\n"
                       "   mb\n"
                       "   br 3f\n"
                       "2: mov 0,%1\n"
                       "3:\n"
                       : "=&r"(tmp), "=&r"(ok), "+m"(*l)
                       :
                       : "memory");
  return (int)ok;
}

struct worker {
  int cpu;
  long done, quiesces;
};

static void acquire(struct worker *w) {
  for (;;) {
    if (try_lock(&lock_word))         /* GetLock */
      return;
    for (;;) {                        /* HandleBusyLock */
      if (smt) {
        if (ldq_arm(&lock_word) == 0) /* free: try for it */
          break;
        quiesce();
        w->quiesces++;
      }
      if (lock_word == 0)             /* CheckLock */
        break;
    }
  }
}

static void release(void) {
  __asm__ __volatile__("mb" ::: "memory");
  lock_word = 0;
}

static void *run(void *arg) {
  struct worker *w = arg;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(w->cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set))
    fprintf(stderr, "thread on cpu %d: not pinned\n", w->cpu);
  for (long i = 0; i < n; i++) {
    acquire(w);
    long c = shared_count;
    for (volatile long k = 0; k < hold; k++)
      ;
    shared_count = c + 1;
    release();
    w->done++;
  }
  return NULL;
}

#define MAX_THREADS 32

int main(int argc, char **argv) {
  int threads = 2;
  if (argc > 1)
    n = atol(argv[1]);
  if (argc > 2)
    hold = atol(argv[2]);
  if (argc > 3)
    threads = atoi(argv[3]);
  if (threads < 1 || threads > MAX_THREADS)
    threads = 2;
  smt = amask(0x400) == 0;
  printf("AMASK(0x400) = %#lx: %s; AT_HWCAP = %#lx\n", amask(0x400),
         smt ? "SMT, LDQ_ARM and QUIESCE used" : "no SMT, a plain spin",
         (long)getauxval(AT_HWCAP));
  struct worker w[MAX_THREADS];
  pthread_t t[MAX_THREADS];
  for (int i = 0; i < threads; i++) {
    w[i].cpu = i;
    w[i].done = w[i].quiesces = 0;
    pthread_create(&t[i], NULL, run, &w[i]);
  }
  int ok = 1;
  for (int i = 0; i < threads; i++) {
    pthread_join(t[i], NULL);
    ok = ok && w[i].done == n;
  }
  ok = ok && shared_count == threads * n;
  printf("count %ld of %ld", shared_count, threads * n);
  for (int i = 0; i < threads; i++)
    printf("; thread %d (cpu %d) %ld, %ld QUIESCEs", i, w[i].cpu, w[i].done,
           w[i].quiesces);
  printf("\n");
  printf("QTEST %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
