/* Alphabox: the AlphaServer ES40 emulator
 *
 * `alphabox hvtest`: the VM runtime's self-test -- ordinary functions of
 * this binary run at EL1 and come back, with the libraries, the heap, the
 * stack, system calls and the clock behaving as outside, and with memory
 * from hv::alloc shared both ways.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */
#ifdef ALPHABOX_HVF

#include "HvRuntime.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach/mach_time.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 1. arithmetic only: the trampoline, the call, the return
uint64_t t_return(void *arg) { return *(uint64_t *)arg * 2 + 1; }

// 2. the C library and a system call: memset and memcpy out of the shared
//    cache, a global, snprintf on the stack, and write(2) through the
//    syscall proxy. Deliberately no malloc: the process allocator's
//    metadata is a private copy inside the VM, so an allocation made
//    inside and a free() over that copy walk a list the outside is still
//    changing. Code that runs inside allocates from hv::alloc or not at
//    all -- see t_malloc_is_unusable below.
int g_counter = 0;
struct LibcArg {
  uint8_t *buf; // hv::alloc memory
  size_t size;
  uint64_t sum;
  int fd;
};
uint64_t t_libc(void *p) {
  LibcArg *a = (LibcArg *)p;
  memset(a->buf, 0x5a, a->size);
  uint8_t stack_copy[4096];
  memcpy(stack_copy, a->buf, sizeof stack_copy);
  uint64_t s = 0;
  for (size_t i = 0; i < a->size; i += 4093)
    s += a->buf[i];
  s += stack_copy[0];
  g_counter++;
  char line[96];
  int n = snprintf(line, sizeof line,
                   "  (written from EL1 by write(2): sum %llu)\n",
                   (unsigned long long)s);
  write(a->fd, line, (size_t)n);
  a->sum = s;
  return s;
}

// 3. the clock: mach_absolute_time (commpage) and steady_clock
uint64_t t_clock(void *) {
  const uint64_t a = mach_absolute_time();
  const double t = now_ms();
  const uint64_t b = mach_absolute_time();
  return (b >= a && t > 0) ? 1 : 0;
}

// 4. printf from EL1: stdio's own buffers and locks, then the write proxy
uint64_t t_printf(void *) {
  printf("  printf from EL1: %d %s %.2f\n", 42, "ok", 3.14159);
  return 1;
}

// 5. memory from hv::alloc: written inside, read outside. The one kind of
//    memory the two sides genuinely share.
struct ShareArg {
  uint64_t *buf;
  size_t words;
};
uint64_t t_share(void *p) {
  ShareArg *a = (ShareArg *)p;
  for (size_t i = 0; i < a->words; i++)
    a->buf[i] = i * 0x9e3779b97f4a7c15ull;
  return a->words;
}

// 6. a memory-bound loop, timed inside and outside: the VM's stage-1 walk
//    versus the host's, on the same code and the same (shared) pages
struct LoopArg {
  uint64_t *buf;
  size_t words;
  int rounds;
};
uint64_t t_loop(void *p) {
  LoopArg *a = (LoopArg *)p;
  uint64_t s = 0;
  for (int r = 0; r < a->rounds; r++)
    for (size_t i = 0; i < a->words; i += 8)
      s += a->buf[(i * 2654435761u) % a->words];
  return s;
}

// 6. a second call on the same vCPU, and a call on another thread
uint64_t t_thread(void *arg) { return t_return(arg) + 1000; }

// 7. the generic timer as the CPU core reads it (mrs cntvct_el0): does the
//    VM's virtual counter run at the host's value, or with an offset?
uint64_t t_cntvct(void *) {
  uint64_t v;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}

// Everything a call inside the VM reads through a pointer has to live in
// hv::alloc memory, arguments included. The process's stack and heap are
// copied into the VM the first time they are touched, and that snapshot is
// never refreshed -- an argument struct written on the host stack after
// the page was copied is simply not there inside. All the argument blocks
// below therefore sit at the front of the shared region.
struct Args {
  uint64_t v;
  LibcArg libc;
  ShareArg share;
  LoopArg loop;
};

void report(const char *what, bool ok) {
  printf("  %-44s %s\n", what, ok ? "ok" : "FAILED");
}

} // namespace

int main_hvtest(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc >= 2 && strcmp(argv[1], "--regions") == 0) {
    printf("Alphabox hvtest --regions: how each region of this process would "
           "be treated (no VM is created)\n");
    hv::dump_regions();
    return 0;
  }
  printf("Alphabox hvtest: this process's code at EL1\n");
  if (!hv::init()) {
    printf("  VM unavailable\n");
    return 1;
  }
  int fails = 0;
  const size_t kShared = 64u << 20;
  const size_t kArgs = 64u << 10;
  uint8_t *block = (uint8_t *)hv::alloc(kShared + kArgs);
  if (!block) {
    printf("  hv::alloc failed; every check needs it\n");
    return 1;
  }
  Args *args = (Args *)block;            // arguments: shared, not on the stack
  uint64_t *shared = (uint64_t *)(block + kArgs); // the data under test

  args->v = 20;
  uint64_t r = hv::call(t_return, &args->v);
  report("call/return", r == 41);
  fails += r != 41;

  args->libc = LibcArg{(uint8_t *)shared, 4u << 20, 0, STDOUT_FILENO};
  r = hv::call(t_libc, &args->libc);
  const uint64_t expect = ((4u << 20) + 4092) / 4093 * 0x5a + 0x5a;
  report("libc, stack, global, write(2) inside", r == expect);
  fails += r != expect;
  // Everything of the process the VM touches is a private copy, so a
  // global written inside is not seen outside. Stated, not judged: it is
  // the rule the inside code has to be written to.
  printf("  %-44s %s\n", "global written inside, seen outside",
         g_counter == 1 ? "yes" : "no, as expected (process memory is copied)");

  r = hv::call(t_clock, nullptr);
  report("mach_absolute_time / steady_clock", r == 1);
  fails += r != 1;

  r = hv::call(t_printf, nullptr);
  report("printf", r == 1);
  fails += r != 1;

  args->share = ShareArg{shared, kShared / 8};
  memset(shared, 0, kShared);
  const uint64_t wrote = hv::call(t_share, &args->share);
  bool seen = wrote == args->share.words;
  for (size_t i = 0; seen && i < args->share.words; i += 4096)
    seen = shared[i] == i * 0x9e3779b97f4a7c15ull;
  report("hv::alloc memory written inside, read outside", seen);
  fails += !seen;

  args->loop = LoopArg{shared, kShared / 8, 4};
  LoopArg &la = args->loop;
  for (size_t i = 0; i < la.words; i++)
    la.buf[i] = i;
  const uint64_t s_host = t_loop(&la);
  double t0 = now_ms();
  const uint64_t s_host2 = t_loop(&la);
  double t_host = now_ms() - t0;
  hv::call(t_loop, &la); // first pass: page everything in
  t0 = now_ms();
  const uint64_t s_vm = hv::call(t_loop, &la);
  double t_vm = now_ms() - t0;
  report("64 MB strided sum on shared memory, same result",
         s_vm == s_host && s_host2 == s_host);
  fails += s_vm != s_host;
  printf("    host %.1f ms, EL1 %.1f ms (%.2fx)\n", t_host, t_vm, t_vm / t_host);

  uint64_t r2 = 0;
  std::thread th([&] { r2 = hv::call(t_thread, &args->v); });
  th.join();
  report("call from a second thread (own vCPU)", r2 == 1041);
  fails += r2 != 1041;

  uint64_t h0, h1;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(h0));
  const uint64_t inside = hv::call(t_cntvct, nullptr);
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(h1));
  const bool in_window = inside >= h0 && inside <= h1;
  report("cntvct_el0 inside == host's counter", in_window);
  fails += !in_window;
  if (!in_window)
    printf("    host %llu..%llu, inside %llu (offset %lld)\n",
           (unsigned long long)h0, (unsigned long long)h1,
           (unsigned long long)inside, (long long)(inside - h0));

  const hv::Stats &st = hv::stats();
  printf("  entries %llu, pages direct %llu copied %llu, stage-1 faults %llu, "
         "syscalls %llu, irqs %llu\n",
         (unsigned long long)st.entries, (unsigned long long)st.pages_direct,
         (unsigned long long)st.pages_copied,
         (unsigned long long)st.stage1_faults, (unsigned long long)st.syscalls,
         (unsigned long long)st.irqs);
  printf("hvtest: %d failure(s)\n", fails);
  return fails ? 1 : 0;
}

#endif // ALPHABOX_HVF
