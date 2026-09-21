/* Alphabox: the AlphaServer ES40 emulator
 *
 * Hypervisor.framework runtime -- the process runs its own code at EL1.
 *
 * The design (docs/hypervisor.md, "The build"): a VM whose stage-1 page
 * tables we own maps this process's address space onto itself, so an
 * ordinary function of this binary runs at EL1 on a vCPU with the same
 * pointers it uses outside. System calls trap to our EL1 vector, leave the
 * VM, and are performed by the host thread with the same register values;
 * the result goes back through the saved frame.
 *
 * THE MAPPING RULE. Only memory this runtime allocated through
 * hv_vm_allocate -- the page tables, the vCPU stacks, the copy arena and
 * whatever alloc() hands out -- is ever passed to hv_vm_map. Every other
 * page the VM touches is COPIED into the arena and mapped at its original
 * virtual address: the binary's text and data, the dyld shared cache, the
 * commpage, the process heap, the process stack, all of it. A page's
 * acceptability is a property of the physical frame, not of the region the
 * kernel describes it with. One such frame is known by name: a page of a
 * MAP_JIT mapping that has been used as one -- what asmjit's code cache is
 * made of -- is typed XNU_USER_JIT, and handing it to hv_vm_map takes the
 * host down instead of returning an error. There is no user-space test for
 * the frame type, so the runtime offers the framework only what the
 * framework itself allocated: hv_vm_allocate is documented as the memory
 * "suitable to be mapped as guest memory", and that is the whole of what
 * we give it. Compiled code that must run inside the VM therefore has to
 * be emitted into alloc() memory, not into a MAP_JIT region.
 *
 * The consequence is that inside the VM the process's own memory is a
 * private snapshot: a global, a malloc'd block or a stack slot written
 * inside is not seen outside. Code meant to run inside must keep every
 * byte it shares with the outside in alloc() memory. That is what phase 2
 * has to be built on -- see docs/hypervisor.md.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */
#ifdef ALPHABOX_HVF

#include "HvRuntime.hpp"

#include <Hypervisor/Hypervisor.h>
#include <asmjit/a64.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach/mach.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mutex>
#include <chrono>
#include <thread>
#include <sys/mman.h>
#include <unordered_map>
#include <vector>

namespace hv {
namespace {

using namespace asmjit;

// ---- address-space plan --------------------------------------------------
// Host pages are 16 KB; the stage-1 granule is 4 KB (four entries per host
// page). IPA space is 40 bits on this framework: host addresses below
// kIpaCopyBase are mapped identity, copies live above it.
constexpr uint64_t kHostPage = 16384;
constexpr uint64_t kIpaCopyBase = 0xC000000000ULL; // 768 GB
constexpr uint64_t kIpaLimit = 0x10000000000ULL;   // 1 TB
constexpr size_t kTableArena = 64u << 20;          // stage-1 tables
constexpr size_t kCopyArena = 512u << 20;          // copied pages
constexpr size_t kStackSize = 8u << 20;            // per vCPU
constexpr size_t kTrampSize = 16384;               // vectors + entry

constexpr uint64_t kPteTable = 0x3;
constexpr uint64_t kPtePage = 0x703; // valid page, AF, inner shareable, attr0
constexpr uint64_t kSctlrMmuOff = 0x30d00800;
constexpr uint64_t kSctlrMmuOn = kSctlrMmuOff | 0x1 | 0x4 | 0x1000; // M C I
// T0SZ=16 (48-bit low half), WB/WB, inner shareable, 4 KB, TTBR1 off, IPS 40
constexpr uint64_t kTcr = 16 | (1u << 8) | (1u << 10) | (3u << 12) |
                          (1u << 23) | (2ull << 32);
constexpr uint64_t kMair = 0xff; // attr0: normal, write-back
constexpr uint64_t kCpacrFpEnabled = 3u << 20;
constexpr uint64_t kCpsrEl1hMasked = 0x3c5;

// The saved frame the EL1 vectors build (see gen_vectors).
struct Frame {
  uint64_t x[31];
  uint64_t elr, spsr, esr, far;
  uint64_t pad; // 16-byte alignment
};
static_assert(sizeof(Frame) == 288, "frame layout");
constexpr int kFrameSize = 288;

enum Call : uint64_t {
  kCallReturn = 0,    // x1 = fn's result
  kCallException = 1, // x1 = frame: synchronous exception at EL1
  kCallIrq = 2,       // x1 = frame
  kCallBadVector = 3, // x1 = vector index: an EL0 or SError vector was taken
  kCallEscape = 4,    // x1 = frame: run x[2](x[3]) on the host, result in x[0]
};

// Hand-encoded system instructions asmjit's assembler does not spell.
constexpr uint32_t kEret = 0xD69F03E0, kHvc0 = 0xD4000002,
                   kDsbIsh = 0xD5033B9F, kIsb = 0xD5033FDF;
uint32_t mrs_elr(int rt) { return 0xD5384020 | rt; }
uint32_t mrs_spsr(int rt) { return 0xD5384000 | rt; }
uint32_t mrs_esr(int rt) { return 0xD5385200 | rt; }
uint32_t mrs_far(int rt) { return 0xD5386000 | rt; }
uint32_t msr_elr(int rt) { return 0xD5184020 | rt; }
uint32_t msr_spsr(int rt) { return 0xD5184000 | rt; }

// ---- global VM state ----------------------------------------------------
std::mutex g_lock; // page tables and arenas
bool g_inited = false, g_ok = false;
uint8_t *g_tables = nullptr; // identity-mapped arena for stage-1 tables
size_t g_tables_used = 0;
uint8_t *g_copies = nullptr; // copy arena (host VA), IPA = kIpaCopyBase + off
size_t g_copies_used = 0;
uint8_t *g_tramp = nullptr; // vectors (2 KB aligned) + entry stub
uint64_t g_entry_pc = 0;
uint64_t g_escape_pc = 0; // the in-VM stub that calls out to the host

// What code running INSIDE the VM needs to read. It cannot use ordinary
// globals: the page holding them is copied on first touch and the copy is
// never refreshed, so anything written after the first entry is invisible
// inside. This block is alloc() memory -- genuinely shared -- and the
// pointer to it is set before the VM is ever entered, so the copied
// pointer is correct.
struct Control {
  uint64_t escape_pc;
  uint32_t nstacks;
  struct {
    uint64_t base, top;
  } stacks[16];
  Stats stats; // counted on both sides, so it cannot be an ordinary global
};
Control *g_ctl = nullptr;
Stats g_stats_early; // before the control block exists
inline Stats &S() { return g_ctl ? g_ctl->stats : g_stats_early; }
// Which host pages the VM already has, as an open-addressed table of fixed
// capacity. It must not be an allocating container: the fault handler runs
// on the thread that was inside the VM, and that thread may hold the
// process allocator's lock -- when the process's own heap is mapped in
// place the guest's malloc lock IS the host's, and allocating here
// recursively locks it (libplatform kills the process for that). Nothing
// on the fault path allocates.
struct PageTable {
  uint64_t *keys = nullptr; // host page address, 0 = empty
  uint8_t *vals = nullptr;  // 1 mapped in place, 2 copied
  size_t mask = 0;
  size_t count = 0;

  bool init(size_t capacity_pow2) {
    void *k = mmap(nullptr, capacity_pow2 * 8, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *v = mmap(nullptr, capacity_pow2, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (k == MAP_FAILED || v == MAP_FAILED)
      return false;
    keys = (uint64_t *)k;
    vals = (uint8_t *)v;
    mask = capacity_pow2 - 1;
    return true;
  }
  size_t slot(uint64_t page) const {
    size_t i = (size_t)((page >> 14) * 0x9E3779B97F4A7C15ull >> 24) & mask;
    while (keys[i] && keys[i] != page)
      i = (i + 1) & mask;
    return i;
  }
  bool has(uint64_t page) const { return keys[slot(page)] == page; }
  void put(uint64_t page, uint8_t how) {
    const size_t i = slot(page);
    if (!keys[i]) {
      keys[i] = page;
      count++;
    }
    vals[i] = how;
  }
  bool full() const { return count * 4 > mask * 3; }
} g_mapped;
std::vector<hv_vcpu_t> g_vcpus;
std::atomic<bool> g_cancel{false};
const bool g_trace = getenv("ALPHABOX_HV_TRACE") != nullptr;

bool check(hv_return_t r, const char *what) {
  if (r == HV_SUCCESS)
    return true;
  printf("%%HV-F-%s: 0x%x\n", what, (unsigned)r);
  return false;
}

// ---- stage-1 tables -----------------------------------------------------
uint64_t *alloc_table() {
  if (g_tables_used + 4096 > kTableArena) {
    printf("%%HV-F-TABLES: stage-1 table arena exhausted\n");
    abort();
  }
  uint64_t *t = (uint64_t *)(g_tables + g_tables_used);
  g_tables_used += 4096;
  memset(t, 0, 4096);
  return t;
}

uint64_t *g_root = nullptr; // L0: 512 x 512 GB

// The L3 entry for a 4 KB VA, creating the tables on the way.
uint64_t *pte_for(uint64_t va) {
  uint64_t *t = g_root;
  for (int level = 0; level < 3; level++) {
    const unsigned shift = 39 - 9 * level;
    uint64_t &e = t[(va >> shift) & 511];
    if (!(e & 1)) {
      uint64_t *n = alloc_table();
      e = (uint64_t)(uintptr_t)n | kPteTable; // tables are identity-mapped
    }
    t = (uint64_t *)(uintptr_t)(e & 0x0000fffffffff000ULL);
  }
  return &t[(va >> 12) & 511];
}

void map_4k(uint64_t va, uint64_t ipa) { *pte_for(va) = ipa | kPtePage; }

} // namespace

// The region the page is in, as the kernel describes it. Anonymous means:
// no external pager (no file behind it), not inside a submap (the shared
// cache lives in one), privately owned (SM_PRIVATE, or SM_EMPTY for pages
// not yet touched), and below the copy IPA base so it can be mapped at its
// own address. The classification is what decides whether hv_vm_map is
// ever called for it; a page is never offered on the chance that the
// framework accepts it.
PageClass classify(uint64_t page, char *why, size_t why_len) {
  mach_vm_address_t addr = page;
  mach_vm_size_t size = 0;
  natural_t depth = 0;
  vm_region_submap_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
  kern_return_t kr = mach_vm_region_recurse(mach_task_self(), &addr, &size,
                                            &depth, (vm_region_recurse_info_t)&info,
                                            &count);
  if (kr != KERN_SUCCESS || addr > page) {
    snprintf(why, why_len, "no region");
    return PageClass::kUnmapped;
  }
  // malloc's large allocations (and the reclaim ring they come back
  // through) are the process's own anonymous pages too, but the kernel
  // reports them as SM_TRUESHARED; they are accepted by tag.
  const bool malloc_tag =
      (info.user_tag >= VM_MEMORY_MALLOC && info.user_tag <= VM_MEMORY_MALLOC_NANO) ||
      info.user_tag == VM_MEMORY_VM_RECLAIM;
  const bool anon = !info.external_pager && !info.is_submap && depth == 0 &&
                    (info.share_mode == SM_PRIVATE ||
                     info.share_mode == SM_EMPTY ||
                     (info.share_mode == SM_TRUESHARED && malloc_tag)) &&
                    page < kIpaCopyBase;
  snprintf(why, why_len, "%s tag %u share %u pager %u submap %u depth %u prot %x",
           anon ? "anonymous" : "copy", info.user_tag, info.share_mode,
           info.external_pager, info.is_submap, depth, info.protection);
  if (!(info.protection & VM_PROT_READ)) {
    snprintf(why, why_len, "unreadable (prot %x)", info.protection);
    return PageClass::kUnmapped;
  }
  return anon ? PageClass::kAnonymous : PageClass::kCopy;
}

void dump_regions() {
  mach_vm_address_t addr = 0;
  int n = 0, anon = 0, copy = 0;
  printf("  %-25s %8s class      tag share pager submap prot\n", "region", "size");
  for (;;) {
    mach_vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                               (vm_region_recurse_info_t)&info,
                               &count) != KERN_SUCCESS)
      break;
    char why[128];
    const PageClass c = classify(addr, why, sizeof why);
    n++;
    if (c == PageClass::kAnonymous)
      anon++;
    else if (c == PageClass::kCopy)
      copy++;
    printf("  %012llx-%012llx %7lluK %-10s %3u %5u %5u %6u %x\n",
           (unsigned long long)addr, (unsigned long long)(addr + size),
           (unsigned long long)(size >> 10),
           c == PageClass::kAnonymous ? "anonymous"
           : c == PageClass::kCopy    ? "copy"
                                      : "unmapped",
           info.user_tag, info.share_mode, info.external_pager, info.is_submap,
           info.protection);
    addr += size;
  }
  printf("  %d regions: %d the kernel calls anonymous, %d not, %d unreadable."
         " All of them are copied into the VM; only hv::alloc memory is "
         "mapped.\n",
         n, anon, copy, n - anon - copy);
}

namespace {

// The runtime's own allocations (hv_vm_allocate) are always mapped in
// place: the vectors write frames to the vCPU stack that the host reads.
bool map_own(void *base, size_t size) {
  if (!check(hv_vm_map(base, (uint64_t)(uintptr_t)base, size,
                       HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
             "MAPOWN"))
    return false;
  for (size_t off = 0; off < size; off += 4096)
    map_4k((uint64_t)(uintptr_t)base + off, (uint64_t)(uintptr_t)base + off);
  for (size_t off = 0; off < size; off += kHostPage)
    g_mapped.put((uint64_t)(uintptr_t)base + off, 1);
  return true;
}

// Map one host page (16 KB) so the VM sees it at its own address: in place
// when it is this process's own anonymous memory, else as a private copy.
// Returns false when the address is not readable at all (a real fault for
// the caller).
bool map_host_page(uint64_t page) {
  if (g_mapped.has(page))
    return true;
  if (g_mapped.full()) {
    printf("%%HV-F-PAGETABLE: page table full (%zu pages)\n", g_mapped.count);
    return false;
  }
  char why[128];
  const PageClass cls = classify(page, why, sizeof why);
  // kUnmapped only says the kernel described no region or no read access;
  // the commpage is one such and reads fine. The copy below fails cleanly
  // when the page really cannot be read.
  (void)cls;
  // Copy: read through the kernel so an unmapped or guarded page fails
  // cleanly instead of faulting this thread.
  if (g_copies_used + kHostPage > kCopyArena) {
    printf("%%HV-F-COPIES: copy arena exhausted\n");
    return false;
  }
  uint8_t *slot = g_copies + g_copies_used;
  mach_vm_size_t got = 0;
  kern_return_t kr = mach_vm_read_overwrite(
      mach_task_self(), (mach_vm_address_t)page, kHostPage,
      (mach_vm_address_t)(uintptr_t)slot, &got);
  if (kr != KERN_SUCCESS || got != kHostPage) {
    if (g_trace)
      printf("HVT unreadable %llx (kr %d)\n", (unsigned long long)page, kr);
    return false;
  }
  const uint64_t ipa = kIpaCopyBase + g_copies_used;
  if (!check(hv_vm_map(slot, ipa, kHostPage,
                       HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
             "MAPCOPY"))
    return false;
  g_copies_used += kHostPage;
  // The vCPU fetches instructions from memory at the point of unification:
  // clean what memcpy left in this core's data cache, or it executes stale
  // bytes (zeros -- an undefined instruction, then the vector, then that
  // again, with no exit to show for it).
  sys_icache_invalidate(slot, kHostPage);
  for (int i = 0; i < 4; i++)
    map_4k(page + 4096 * i, ipa + 4096 * i);
  g_mapped.put(page, 2);
  S().pages_copied++;
  if (g_trace)
    printf("HVT map copy   %llx -> ipa %llx (%s)\n", (unsigned long long)page,
           (unsigned long long)ipa, why);
  return true;
}

// ---- the in-VM code: vectors and the entry stub --------------------------
void emit(a64::Assembler &a, uint32_t w) { a.embed(&w, 4); }

// Save x0-x30, ELR, SPSR, ESR, FAR in a frame on the EL1 stack, hand it to
// the host (x0 = kind, x1 = frame), restore and ERET. The host may rewrite
// any field of the frame before the restore.
void gen_handler(a64::Assembler &a, uint64_t kind) {
  a.sub(a64::sp, a64::sp, imm(kFrameSize));
  for (int r = 0; r < 30; r += 2)
    a.stp(a64::x(r), a64::x(r + 1), a64::ptr(a64::sp, 8 * r));
  a.str(a64::x30, a64::ptr(a64::sp, 8 * 30));
  emit(a, mrs_elr(0));
  a.str(a64::x0, a64::ptr(a64::sp, 8 * 31));
  emit(a, mrs_spsr(0));
  a.str(a64::x0, a64::ptr(a64::sp, 8 * 32));
  emit(a, mrs_esr(0));
  a.str(a64::x0, a64::ptr(a64::sp, 8 * 33));
  emit(a, mrs_far(0));
  a.str(a64::x0, a64::ptr(a64::sp, 8 * 34));
  a.mov(a64::x0, imm(kind));
  a.add(a64::x1, a64::sp, imm(0));
  emit(a, kHvc0);
  a.ldr(a64::x0, a64::ptr(a64::sp, 8 * 31));
  emit(a, msr_elr(0));
  a.ldr(a64::x0, a64::ptr(a64::sp, 8 * 32));
  emit(a, msr_spsr(0));
  a.ldr(a64::x30, a64::ptr(a64::sp, 8 * 30));
  for (int r = 28; r >= 0; r -= 2)
    a.ldp(a64::x(r), a64::x(r + 1), a64::ptr(a64::sp, 8 * r));
  a.add(a64::sp, a64::sp, imm(kFrameSize));
  emit(a, kIsb);
  emit(a, kEret);
}

// The vector table (16 entries of 0x80 bytes) followed by the handlers and
// the entry stub. We run at EL1 with SP_EL1 (entries 4-7); every other
// entry reports itself and stops.
struct Errors : public ErrorHandler {
  void handle_error(Error err, const char *message, BaseEmitter *) override {
    printf("%%HV-F-ASMJIT: error %u: %s\n", (unsigned)err, message);
    abort();
  }
};

void gen_tramp(uint8_t *base) {
  CodeHolder code;
  code.init(Environment::host(), (uint64_t)(uintptr_t)base);
  Errors eh;
  code.set_error_handler(&eh);
  a64::Assembler a(&code);
  Label sync = a.new_label(), irq = a.new_label(), entry = a.new_label(),
        esc = a.new_label();
  for (int v = 0; v < 16; v++) {
    // 0x80 bytes per entry, padded by hand (asmjit's align() stops at 64).
    if (v == 4)
      a.b(sync);
    else if (v == 5)
      a.b(irq);
    else {
      a.mov(a64::x0, imm(kCallBadVector));
      a.mov(a64::x1, imm(v));
      emit(a, kHvc0);
      Label self = a.new_label();
      a.bind(self);
      a.b(self);
    }
    while (a.offset() < (size_t)(v + 1) * 0x80)
      a.nop();
  }
  a.bind(sync);
  gen_handler(a, kCallException);
  a.bind(irq);
  gen_handler(a, kCallIrq);
  // escape: x0 = fn, x1 = arg -- hand both to the host and return what it
  // returns. Built like a handler frame so the host can read and write the
  // registers through one layout.
  a.bind(esc);
  a.sub(a64::sp, a64::sp, imm(kFrameSize));
  a.str(a64::x30, a64::ptr(a64::sp, 8 * 30));
  a.str(a64::x0, a64::ptr(a64::sp, 8 * 2)); // x[2] = fn
  a.str(a64::x1, a64::ptr(a64::sp, 8 * 3)); // x[3] = arg
  a.mov(a64::x0, imm(kCallEscape));
  a.add(a64::x1, a64::sp, imm(0));
  emit(a, kHvc0);
  a.ldr(a64::x0, a64::ptr(a64::sp, 8 * 0)); // the host left the result here
  a.ldr(a64::x30, a64::ptr(a64::sp, 8 * 30));
  a.add(a64::sp, a64::sp, imm(kFrameSize));
  a.ret(a64::x30);

  // entry: x0 = fn, x1 = arg
  a.bind(entry);
  a.mov(a64::x2, a64::x0);
  a.mov(a64::x0, a64::x1);
  a.blr(a64::x2);
  a.mov(a64::x1, a64::x0);
  a.mov(a64::x0, imm(kCallReturn));
  emit(a, kHvc0);
  Label self = a.new_label();
  a.bind(self);
  a.b(self);
  code.flatten();
  if (code.code_size() > kTrampSize) {
    printf("%%HV-F-TRAMP: vectors do not fit\n");
    abort();
  }
  code.copy_flattened_data(base, kTrampSize);
  sys_icache_invalidate(base, kTrampSize);
  g_entry_pc = (uint64_t)(uintptr_t)base + code.label_offset_from_base(entry);
  g_escape_pc = (uint64_t)(uintptr_t)base + code.label_offset_from_base(esc);
  if (g_trace) {
    const uint32_t *w = (const uint32_t *)base;
    printf("HVT tramp at %p, entry %llx, sync handler at +%llx\n", base,
           (unsigned long long)g_entry_pc,
           (unsigned long long)code.label_offset_from_base(sync));
    for (size_t off = 0x200 / 4; off < 0x200 / 4 + 2; off++)
      printf("HVT  +%03zx %08x\n", off * 4, w[off]);
    const size_t h = code.label_offset_from_base(sync) / 4;
    for (size_t off = h; off < h + 12; off++)
      printf("HVT  +%03zx %08x\n", off * 4, w[off]);
    const size_t en = code.label_offset_from_base(entry) / 4;
    for (size_t off = en; off < en + 7; off++)
      printf("HVT  +%03zx %08x\n", off * 4, w[off]);
  }
}

// ---- the per-thread vCPU -------------------------------------------------
struct Vcpu {
  hv_vcpu_t id = 0;
  hv_vcpu_exit_t *exit = nullptr;
  uint8_t *stack = nullptr;
  bool ok = false;
  // Apple's implementation-defined registers the guest wrote, by their
  // encoding: read back as written (pthread_jit_write_protect_np checks).
  std::unordered_map<uint32_t, uint64_t> apple_regs;
};
thread_local Vcpu t_vcpu;

bool vcpu_init(Vcpu &v) {
  if (!check(hv_vcpu_create(&v.id, &v.exit, nullptr), "VCPU"))
    return false;
  void *stack = nullptr;
  if (!check(hv_vm_allocate(&stack, kStackSize, HV_ALLOCATE_DEFAULT), "ALLOC"))
    return false;
  v.stack = (uint8_t *)stack;
  uint64_t tpidr = 0, tpidrro = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tpidr));
  __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tpidrro));
  bool ok =
      // The framework gives a new vCPU its own counter origin; the CPU core
      // reads cntvct_el0 directly and compares it with the host's, so the
      // offset is zeroed to make the two the same clock.
      check(hv_vcpu_set_vtimer_offset(v.id, 0), "VTIMER_OFFSET") &&
      check(hv_vcpu_set_sys_reg(v.id, HV_SYS_REG_MAIR_EL1, kMair), "MAIR") &&
      check(hv_vcpu_set_sys_reg(v.id, HV_SYS_REG_TCR_EL1, kTcr), "TCR") &&
      check(hv_vcpu_set_sys_reg(v.id, HV_SYS_REG_TTBR0_EL1,
                                (uint64_t)(uintptr_t)g_root),
            "TTBR0") &&
      check(hv_vcpu_set_sys_reg(v.id, HV_SYS_REG_VBAR_EL1,
                                (uint64_t)(uintptr_t)g_tramp),
            "VBAR") &&
      check(hv_vcpu_set_sys_reg(v.id, HV_SYS_REG_CPACR_EL1, kCpacrFpEnabled),
            "CPACR") &&
      check(hv_vcpu_set_sys_reg(v.id, HV_SYS_REG_TPIDR_EL0, tpidr), "TPIDR") &&
      check(hv_vcpu_set_sys_reg(v.id, HV_SYS_REG_TPIDRRO_EL0, tpidrro),
            "TPIDRRO") &&
      check(hv_vcpu_set_sys_reg(v.id, HV_SYS_REG_SCTLR_EL1, kSctlrMmuOn),
            "SCTLR");
  if (!ok)
    return false;
  {
    std::lock_guard<std::mutex> g(g_lock);
    g_vcpus.push_back(v.id);
    // The stack is touched by the vectors before any fault could map it.
    if (!map_own(v.stack, kStackSize))
      return false;
    if (g_ctl && g_ctl->nstacks < 16) {
      // inside() asks whether the stack pointer is on a vCPU stack.
      g_ctl->stacks[g_ctl->nstacks].base = (uint64_t)(uintptr_t)v.stack;
      g_ctl->stacks[g_ctl->nstacks].top =
          (uint64_t)(uintptr_t)v.stack + kStackSize;
      g_ctl->nstacks++;
    }
  }
  v.ok = true;
  return true;
}

// Perform the guest's system call on this (the same) thread: the kernel
// sees the same process, the same pointers and the same thread. x16 is the
// number, x0-x7 the arguments; x0/x1 come back with the carry flag as the
// error indicator, exactly the ABI libSystem's stubs use.
void do_syscall(Frame &f) {
  register uint64_t x0 __asm__("x0") = f.x[0];
  register uint64_t x1 __asm__("x1") = f.x[1];
  register uint64_t x2 __asm__("x2") = f.x[2];
  register uint64_t x3 __asm__("x3") = f.x[3];
  register uint64_t x4 __asm__("x4") = f.x[4];
  register uint64_t x5 __asm__("x5") = f.x[5];
  register uint64_t x6 __asm__("x6") = f.x[6];
  register uint64_t x7 __asm__("x7") = f.x[7];
  register uint64_t x16 __asm__("x16") = f.x[16];
  uint64_t nzcv;
  __asm__ volatile("svc #0x80\n\tmrs %[nzcv], nzcv"
                   : "+r"(x0), "+r"(x1), [nzcv] "=r"(nzcv)
                   : "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x6), "r"(x7),
                     "r"(x16)
                   : "memory", "cc");
  f.x[0] = x0;
  f.x[1] = x1;
  f.spsr = (f.spsr & ~(1ull << 29)) | (nzcv & (1ull << 29));
  S().syscalls++;
  if (g_trace)
    printf("HVT syscall %lld -> %llx%s\n", (long long)f.x[16],
           (unsigned long long)x0, (nzcv & (1ull << 29)) ? " (error)" : "");
}

// A synchronous exception at EL1: page it in, do the syscall, or report.
// Returns false to stop the VM.
bool on_exception(Frame &f) {
  const unsigned ec = (unsigned)(f.esr >> 26) & 0x3f;
  const unsigned fsc = (unsigned)f.esr & 0x3f;
  S().el1_ec[ec & 63]++;
  S().el1_far = f.far;
  S().el1_pc = f.elr;
  if (g_trace)
    printf("HVT exception EC 0x%x FSC 0x%x pc %llx far %llx\n", ec, fsc,
           (unsigned long long)f.elr, (unsigned long long)f.far);
  switch (ec) {
  case 0x15: // SVC64
    do_syscall(f);
    return true;
  case 0x21: // instruction abort, same EL
  case 0x25: // data abort, same EL
    if ((fsc & 0x3c) == 0x04) { // translation fault, any level
      std::lock_guard<std::mutex> g(g_lock);
      S().stage1_faults++;
      if (map_host_page(f.far & ~(kHostPage - 1))) {
        __asm__ volatile("dsb ish" ::: "memory");
        return true;
      }
    }
    printf("%%HV-F-FAULT: EC 0x%x FSC 0x%x at va %llx, pc %llx (unmappable)\n",
           ec, fsc, (unsigned long long)f.far, (unsigned long long)f.elr);
    return false;
  default:
    printf("%%HV-F-EXCEPTION: EC 0x%x ESR %llx pc %llx far %llx\n", ec,
           (unsigned long long)f.esr, (unsigned long long)f.elr,
           (unsigned long long)f.far);
    return false;
  }
}

} // namespace

bool init() {
  std::lock_guard<std::mutex> g(g_lock);
  if (g_inited)
    return g_ok;
  g_inited = true;
  hv_vm_config_t cfg = hv_vm_config_create();
  uint32_t ipa = 0;
  if (hv_vm_config_get_max_ipa_size(&ipa) == HV_SUCCESS && ipa >= 40)
    hv_vm_config_set_ipa_size(cfg, 40);
  if (!check(hv_vm_create(cfg), "VMCREATE (is the binary signed with the "
                                "hypervisor entitlement?)"))
    return false;
  // The runtime's own memory comes from the framework's allocator, the
  // memory it documents as "suitable to be mapped as guest memory".
  void *tables = nullptr, *copies = nullptr, *tramp = nullptr;
  if (!check(hv_vm_allocate(&tables, kTableArena, HV_ALLOCATE_DEFAULT),
             "ALLOC") ||
      !check(hv_vm_allocate(&copies, kCopyArena, HV_ALLOCATE_DEFAULT),
             "ALLOC") ||
      !check(hv_vm_allocate(&tramp, kTrampSize, HV_ALLOCATE_DEFAULT), "ALLOC"))
    return false;
  g_tables = (uint8_t *)tables;
  g_copies = (uint8_t *)copies;
  g_tramp = (uint8_t *)tramp;
  // The tables and the trampoline are identity-mapped up front (the walker
  // and the vectors cannot fault their way in); the copy arena is mapped
  // page by page as copies are made.
  if (!g_mapped.init(1u << 21)) { // 2M pages = 32 GB of address space
    printf("%%HV-F-PAGETABLE: cannot allocate the page table\n");
    return false;
  }
  g_root = alloc_table();
  if (!map_own(g_tables, kTableArena) || !map_own(g_tramp, kTrampSize))
    return false;
  gen_tramp(g_tramp);
  void *ctl = nullptr;
  if (!check(hv_vm_allocate(&ctl, kHostPage, HV_ALLOCATE_DEFAULT), "ALLOC") ||
      !map_own(ctl, kHostPage))
    return false;
  g_ctl = (Control *)ctl;
  memset(g_ctl, 0, sizeof *g_ctl);
  g_ctl->escape_pc = g_escape_pc;
  g_ok = true;
  return true;
}

// Memory the VM and the outside world genuinely share: the framework's own
// allocator, mapped at its own address. Everything else the VM touches is a
// private copy, so this is where anything the inside writes and the outside
// reads has to live.
void *alloc(size_t size) {
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_ok)
    return nullptr;
  const size_t rounded = (size + kHostPage - 1) & ~(kHostPage - 1);
  void *p = nullptr;
  if (!check(hv_vm_allocate(&p, rounded, HV_ALLOCATE_DEFAULT), "ALLOC"))
    return nullptr;
  if (!map_own(p, rounded)) {
    hv_vm_deallocate(p, rounded);
    return nullptr;
  }
  if (g_trace)
    printf("HVT alloc %zu bytes at %p (shared with the VM)\n", rounded, p);
  return p;
}

bool inside() {
  if (!g_ctl)
    return false;
  uint64_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  for (uint32_t i = 0; i < g_ctl->nstacks; i++)
    if (sp >= g_ctl->stacks[i].base && sp < g_ctl->stacks[i].top)
      return true;
  return false;
}

uint64_t escape(uint64_t (*fn)(void *), void *arg) {
  if (!inside())
    return fn(arg);
  uint64_t (*stub)(uint64_t (*)(void *), void *) =
      (uint64_t(*)(uint64_t(*)(void *), void *))(uintptr_t)g_ctl->escape_pc;
  return stub(fn, arg);
}

bool available() { return g_ok; }

bool g_enabled = false;
void enable() { g_enabled = g_ok; }
bool enabled() { return g_enabled; }

const Stats &stats() { return S(); }

void cancel_all() {
  g_cancel.store(true);
  std::lock_guard<std::mutex> g(g_lock);
  if (!g_vcpus.empty())
    hv_vcpus_exit(g_vcpus.data(), (uint32_t)g_vcpus.size());
}

namespace {
uint64_t read_cntvct(void *) {
  uint64_t v;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}
uint64_t run_once(Vcpu &v, uint64_t (*fn)(void *), void *arg);

// The vCPU's virtual counter starts at the physical counter, which is not
// what this process reads as cntvct_el0 (the kernel keeps an offset of its
// own). The CPU core compares its counter reads with the host's, so the
// vCPU's offset is set to make the two agree: read inside, read outside,
// apply the difference, read again to confirm the sign.
void calibrate_counter(Vcpu &v) {
  for (int attempt = 0; attempt < 6; attempt++) {
    uint64_t h0, h1;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(h0));
    const uint64_t inside = run_once(v, read_cntvct, nullptr);
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(h1));
    if (inside == ~0ULL)
      return;
    const int64_t delta = (int64_t)(inside - (h0 + h1) / 2);
    if (delta > -48 && delta < 48) // within 2 us at 24 MHz: the same clock
      return;
    uint64_t off = 0;
    hv_vcpu_get_vtimer_offset(v.id, &off);
    hv_vcpu_set_vtimer_offset(v.id, off + (uint64_t)delta);
    if (g_trace)
      printf("HVT counter offset %lld -> %llu\n", (long long)delta,
             (unsigned long long)(off + (uint64_t)delta));
  }
}
} // namespace

uint64_t call(uint64_t (*fn)(void *), void *arg) {
  if (!g_ok)
    return ~0ULL;
  Vcpu &v = t_vcpu;
  if (!v.ok) {
    if (!vcpu_init(v))
      return ~0ULL;
    calibrate_counter(v);
  }
  return run_once(v, fn, arg);
}

namespace {
uint64_t run_once(Vcpu &v, uint64_t (*fn)(void *), void *arg) {
  const uint64_t sp = (uint64_t)(uintptr_t)v.stack + kStackSize - 64;
  if (!check(hv_vcpu_set_reg(v.id, HV_REG_X0, (uint64_t)(uintptr_t)fn), "X0") ||
      !check(hv_vcpu_set_reg(v.id, HV_REG_X1, (uint64_t)(uintptr_t)arg), "X1") ||
      !check(hv_vcpu_set_reg(v.id, HV_REG_PC, g_entry_pc), "PC") ||
      !check(hv_vcpu_set_reg(v.id, HV_REG_CPSR, kCpsrEl1hMasked), "CPSR") ||
      !check(hv_vcpu_set_sys_reg(v.id, HV_SYS_REG_SP_EL1, sp), "SP_EL1"))
    return ~0ULL;
  if (const char *w = getenv("ALPHABOX_HV_WATCHDOG")) {
    const int secs = atoi(w);
    std::thread([secs] {
      std::this_thread::sleep_for(std::chrono::seconds(secs));
      cancel_all();
    }).detach();
  }
  for (;;) {
    __asm__ volatile("dsb ish" ::: "memory"); // table writes before entry
    if (!check(hv_vcpu_run(v.id), "RUN"))
      return ~0ULL;
    S().entries++;
    const hv_vcpu_exit_t &e = *v.exit;
    if (e.reason == HV_EXIT_REASON_CANCELED) {
      if (g_cancel.load()) {
        // ALPHABOX_HV_WATCHDOG: say where the vCPU was when it was stopped.
        uint64_t pc = 0, cpsr = 0, elr = 0, esr = 0, far = 0, sp1 = 0,
                 sctlr = 0, spsr = 0, x0 = 0;
        hv_vcpu_get_reg(v.id, HV_REG_PC, &pc);
        hv_vcpu_get_reg(v.id, HV_REG_CPSR, &cpsr);
        hv_vcpu_get_reg(v.id, HV_REG_X0, &x0);
        hv_vcpu_get_sys_reg(v.id, HV_SYS_REG_ELR_EL1, &elr);
        hv_vcpu_get_sys_reg(v.id, HV_SYS_REG_ESR_EL1, &esr);
        hv_vcpu_get_sys_reg(v.id, HV_SYS_REG_FAR_EL1, &far);
        hv_vcpu_get_sys_reg(v.id, HV_SYS_REG_SP_EL1, &sp1);
        hv_vcpu_get_sys_reg(v.id, HV_SYS_REG_SCTLR_EL1, &sctlr);
        hv_vcpu_get_sys_reg(v.id, HV_SYS_REG_SPSR_EL1, &spsr);
        printf("%%HV-I-STOPPED: pc %llx cpsr %llx x0 %llx | elr %llx esr %llx "
               "far %llx spsr %llx sp_el1 %llx sctlr %llx (entry %llx tramp %p "
               "root %p)\n",
               (unsigned long long)pc, (unsigned long long)cpsr,
               (unsigned long long)x0, (unsigned long long)elr,
               (unsigned long long)esr, (unsigned long long)far,
               (unsigned long long)spsr, (unsigned long long)sp1,
               (unsigned long long)sctlr, (unsigned long long)g_entry_pc,
               (void *)g_tramp, (void *)g_root);
        return ~0ULL;
      }
      continue; // a spurious cancel: resume
    }
    if (e.reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
      hv_vcpu_set_vtimer_mask(v.id, true);
      S().other_exits++;
      continue;
    }
    if (e.reason != HV_EXIT_REASON_EXCEPTION) {
      printf("%%HV-F-EXIT: reason %u\n", (unsigned)e.reason);
      return ~0ULL;
    }
    const unsigned ec = (unsigned)(e.exception.syndrome >> 26) & 0x3f;
    S().by_ec[ec & 63]++;
    if (g_trace && S().entries <= 50) {
      uint64_t pc = 0;
      hv_vcpu_get_reg(v.id, HV_REG_PC, &pc);
      printf("HVT exit %llu: EC 0x%x syndrome %llx pc %llx va %llx ipa %llx\n",
             (unsigned long long)S().entries, ec,
             (unsigned long long)e.exception.syndrome, (unsigned long long)pc,
             (unsigned long long)e.exception.virtual_address,
             (unsigned long long)e.exception.physical_address);
    }
    if (ec == 0x16) { // HVC64: the vectors or the entry stub talking to us
      uint64_t kind = 0, x1 = 0;
      hv_vcpu_get_reg(v.id, HV_REG_X0, &kind);
      hv_vcpu_get_reg(v.id, HV_REG_X1, &x1);
      switch (kind) {
      case kCallReturn:
        return x1;
      case kCallException:
        if (on_exception(*(Frame *)(uintptr_t)x1))
          continue;
        return ~0ULL;
      case kCallEscape: {
        // Perform the call out here, with the process's real memory under
        // it, and leave the result where the stub will pick it up.
        Frame &f = *(Frame *)(uintptr_t)x1;
        uint64_t (*fn)(void *) = (uint64_t(*)(void *))(uintptr_t)f.x[2];
        f.x[0] = fn((void *)(uintptr_t)f.x[3]);
        S().escapes++;
        continue;
      }
      case kCallIrq:
        S().irqs++;
        continue;
      case kCallBadVector:
        printf("%%HV-F-VECTOR: exception vector %llu taken\n",
               (unsigned long long)x1);
        return ~0ULL;
      default:
        printf("%%HV-F-HVC: unknown call %llu\n", (unsigned long long)kind);
        return ~0ULL;
      }
    }
    if (ec == 0x18) { // a trapped MRS: the framework keeps some registers
      // Apple's virtual counter (S3_4_C15_C10_6, what mach_absolute_time
      // reads) and the architected counters are answered with the host's
      // own clock; anything else is reported.
      const uint32_t iss = (uint32_t)e.exception.syndrome & 0x1ffffff;
      const unsigned op0 = (iss >> 20) & 3, op2 = (iss >> 17) & 7,
                     op1 = (iss >> 14) & 7, crn = (iss >> 10) & 15,
                     rt = (iss >> 5) & 31, crm = (iss >> 1) & 15,
                     read = iss & 1;
      S().last_sysreg = iss;
      hv_vcpu_get_reg(v.id, HV_REG_PC, &S().last_pc);
      const bool apple_cnt = op0 == 3 && op1 == 4 && crn == 15 && crm == 10 && op2 == 6;
      const bool cntvct = op0 == 3 && op1 == 3 && crn == 14 && crm == 0 && op2 == 2;
      const bool cntpct = op0 == 3 && op1 == 3 && crn == 14 && crm == 0 && op2 == 1;
      if (read && (apple_cnt || cntvct || cntpct)) {
        uint64_t pc = 0;
        hv_vcpu_get_reg(v.id, HV_REG_PC, &pc);
        if (rt != 31)
          hv_vcpu_set_reg(v.id, (hv_reg_t)(HV_REG_X0 + rt), mach_absolute_time());
        hv_vcpu_set_reg(v.id, HV_REG_PC, pc + 4);
        S().other_exits++;
        continue;
      }
      if (op0 == 3 && (op1 == 4 || op1 == 6) && crn == 15) {
        // Apple's implementation-defined registers (the JIT write-protect
        // switch pthread_jit_write_protect_np flips, and friends): stage 1
        // is ours and has no W^X, so a write only remembers the value and
        // a read returns it -- the switch reads back what it wrote and
        // traps (brk) when the value did not take.
        uint64_t pc = 0;
        hv_vcpu_get_reg(v.id, HV_REG_PC, &pc);
        const uint32_t key = (op1 << 12) | (crn << 8) | (crm << 4) | op2;
        if (read) {
          auto it = v.apple_regs.find(key);
          if (rt != 31)
            hv_vcpu_set_reg(v.id, (hv_reg_t)(HV_REG_X0 + rt),
                            it == v.apple_regs.end() ? 0 : it->second);
        } else {
          uint64_t val = 0;
          if (rt != 31)
            hv_vcpu_get_reg(v.id, (hv_reg_t)(HV_REG_X0 + rt), &val);
          v.apple_regs[key] = val;
        }
        hv_vcpu_set_reg(v.id, HV_REG_PC, pc + 4);
        S().other_exits++;
        if (g_trace)
          printf("HVT sysreg %s S%u_%u_C%u_C%u_%u ignored\n",
                 read ? "read" : "write", op0, op1, crn, crm, op2);
        continue;
      }
      printf("%%HV-F-SYSREG: %s S%u_%u_C%u_C%u_%u (x%u) not handled\n",
             read ? "read of" : "write to", op0, op1, crn, crm, op2, rt);
      return ~0ULL;
    }
    // A stage-2 fault: a VA we mapped whose IPA the framework does not
    // back, or a write to a page mapped read-only. Both are bugs here.
    uint64_t pc = 0;
    hv_vcpu_get_reg(v.id, HV_REG_PC, &pc);
    printf("%%HV-F-STAGE2: EC 0x%x syndrome %llx va %llx ipa %llx pc %llx\n", ec,
           (unsigned long long)e.exception.syndrome,
           (unsigned long long)e.exception.virtual_address,
           (unsigned long long)e.exception.physical_address,
           (unsigned long long)pc);
    return ~0ULL;
  }
}
} // namespace

} // namespace hv

#endif // ALPHABOX_HVF
