/* Alphabox -- Hypervisor.framework probe.
 *
 * `alphabox hvprobe` measures the mechanics docs/hypervisor.md rests on, on
 * this Mac, with nothing emulated: can a VM be created at all (the binary
 * must carry the com.apple.security.hypervisor entitlement -- the build
 * signs it ad hoc), what the ID registers say (4 KB granule, ASID width,
 * physical range), what one exit round trip costs (an HVC, and a data
 * abort on an unmapped IPA -- the MMIO case), and what a stage-1 fault
 * handled INSIDE the VM costs (the TB-miss case: a page table the guest
 * owns, a vector at EL1 that maps the page and returns). Guest code is
 * generated with asmjit at IPA == VA, so the numbers are for the machine,
 * not for any emulator design.
 *
 * Built only with -DALPHABOX_HVF=ON on macOS/arm64 and the JIT's asmjit.
 */
#ifdef ALPHABOX_HVF

#include <Hypervisor/Hypervisor.h>
#include <asmjit/a64.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <atomic>
#include <thread>

namespace {

using namespace asmjit;

constexpr uint64_t kIpaBase = 0x40000000;   // guest memory: 32 MB here
constexpr size_t kMemSize = 32u << 20;
// Each test's code at its own address: the vCPU's instruction cache is not
// invalidated by a host-side write, so reusing one address ran stale code.
constexpr uint64_t kOffCodeA = 0x10000, kOffCodeB = 0x11000,
                   kOffCodeC = 0x12000, kOffCodeD = 0x13000;
constexpr uint64_t kOffL1 = 0x200000;       // stage-1 tables (4 KB granule)
constexpr uint64_t kOffL2 = 0x201000;
constexpr uint64_t kOffL3 = 0x210000;       // 16 L3 tables x 4 KB = 32 MB
constexpr uint64_t kOffVectors = 0x300000;  // 2 KB aligned
constexpr uint64_t kOffProbe = 0x1000000;   // upper 16 MB: 4096 pages
constexpr uint64_t kIpaMmio = 0x80000000;   // nothing mapped here
constexpr uint64_t kSctlrMmuOff = 0x30d00800; // RES1 bits, M=C=I=0
constexpr uint64_t kSctlrMmuOn = kSctlrMmuOff | 0x1 | 0x4 | 0x1000;
constexpr uint64_t kPtePage = 0x703; // valid page, AF, inner shareable, Attr0
constexpr uint64_t kPteTable = 0x3;

uint8_t *g_mem = nullptr;
hv_vcpu_t g_vcpu = 0;
hv_vcpu_exit_t *g_exit = nullptr;

double now_ns() {
  return (double)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool check(hv_return_t r, const char *what) {
  if (r == HV_SUCCESS)
    return true;
  printf("hvprobe: %s failed: 0x%x%s\n", what, (unsigned)r,
         r == HV_DENIED
             ? " (HV_DENIED: the binary lacks the com.apple.security.hypervisor "
               "entitlement -- the build signs it ad hoc; re-run cmake --build)"
             : "");
  return false;
}

// An instruction asmjit cannot encode must stop the tool, not silently
// vanish: the first version lost `orr x, x, #0x703` (not a logical
// immediate) from the fault handler, wrote page-table entries without their
// valid bits, and the guest faulted on the same page forever.
struct ProbeErrors : public ErrorHandler {
  void handle_error(Error err, const char *message, BaseEmitter *) override {
    printf("hvprobe: asmjit error %u: %s\n", (unsigned)err, message);
    exit(2);
  }
};

// Assemble into the guest memory at `off` (IPA == VA there), return the size.
size_t place(uint64_t off, void (*gen)(a64::Assembler &)) {
  CodeHolder code;
  code.init(Environment::host());
  ProbeErrors eh;
  code.set_error_handler(&eh);
  a64::Assembler a(&code);
  gen(a);
  code.flatten();
  const size_t sz = code.code_size();
  code.copy_flattened_data(g_mem + off, sz);
  return sz;
}

void emit_word(a64::Assembler &a, uint32_t w) { a.embed(&w, 4); }
constexpr uint32_t kMrsX10FarEl1 = 0xD538600A; // mrs x10, far_el1
constexpr uint32_t kEret = 0xD69F03E0;
constexpr uint32_t kDsbIsh = 0xD5033B9F;
constexpr uint32_t kIsb = 0xD5033FDF;
constexpr uint32_t kHvc0 = 0xD4000002;

// Test A: exits. loop: hvc #0; b loop
void gen_hvc_loop(a64::Assembler &a) {
  Label l = a.new_label();
  a.bind(l);
  emit_word(a, kHvc0);
  a.b(l);
}
// Test B: MMIO. x2 = unmapped IPA. loop: ldr x1, [x2]; b loop
void gen_mmio_loop(a64::Assembler &a) {
  Label l = a.new_label();
  a.mov(a64::x2, imm(kIpaMmio));
  a.bind(l);
  a.ldr(a64::x1, a64::ptr(a64::x2));
  a.b(l);
}
// Test C: touch 4096 pages, one load each, then hvc. Faults (if any) are
// taken at EL1 by the vector below and never leave the VM.
void gen_touch_loop(a64::Assembler &a) {
  Label l = a.new_label();
  a.mov(a64::x3, imm(kIpaBase + kOffProbe));
  a.mov(a64::x4, imm(4096));
  a.bind(l);
  a.ldr(a64::x1, a64::ptr(a64::x3));
  a.add(a64::x3, a64::x3, imm(4096));
  a.subs(a64::x4, a64::x4, imm(1));
  a.b_ne(l);
  emit_word(a, kHvc0);
}
// Test D: a plain loop of N iterations, no memory, then hvc: the VM's raw
// speed, for scale.
void gen_alu_loop(a64::Assembler &a) {
  Label l = a.new_label();
  a.mov(a64::x4, imm(400000000));
  a.bind(l);
  a.add(a64::x5, a64::x5, imm(1));
  a.add(a64::x6, a64::x6, a64::x5);
  a.eor(a64::x7, a64::x7, a64::x6);
  a.subs(a64::x4, a64::x4, imm(1));
  a.b_ne(l);
  emit_word(a, kHvc0);
}
// The EL1 synchronous vector (current EL, SP_ELx: VBAR + 0x200): map the
// faulting page in the L3 table and return to retry the load.
void gen_fault_handler(a64::Assembler &a) {
  emit_word(a, kMrsX10FarEl1);                 // x10 = faulting VA
  a.mov(a64::x11, imm(kIpaBase));
  a.sub(a64::x12, a64::x10, a64::x11);         // offset in the region
  a.lsr(a64::x12, a64::x12, imm(12));          // page index
  a.mov(a64::x13, imm(kIpaBase + kOffL3));     // L3 tables are contiguous
  a.add(a64::x13, a64::x13, a64::x12, a64::lsl(3));
  a.and_(a64::x14, a64::x10, imm(~(uint64_t)0xfff));
  a.mov(a64::x15, imm(kPtePage)); // 0x703 is not a logical immediate
  a.orr(a64::x14, a64::x14, a64::x15);
  a.str(a64::x14, a64::ptr(a64::x13));
  emit_word(a, kDsbIsh);
  emit_word(a, kIsb);
  emit_word(a, kEret);
}

// Identity stage-1 tables for the 32 MB: L1[1] -> L2, L2[0..15] -> L3[k],
// L3 pages valid except, when `hole`, the probe region's 4096 pages.
void build_tables(bool hole) {
  uint64_t *l1 = (uint64_t *)(g_mem + kOffL1);
  uint64_t *l2 = (uint64_t *)(g_mem + kOffL2);
  memset(l1, 0, 4096);
  memset(l2, 0, 4096);
  l1[(kIpaBase >> 30) & 511] = (kIpaBase + kOffL2) | kPteTable;
  for (int k = 0; k < 16; k++) {
    uint64_t *l3 = (uint64_t *)(g_mem + kOffL3 + 4096 * k);
    l2[k] = (kIpaBase + kOffL3 + 4096 * k) | kPteTable;
    for (int e = 0; e < 512; e++) {
      const uint64_t pa = kIpaBase + ((uint64_t)k << 21) + ((uint64_t)e << 12);
      const bool in_probe = pa >= kIpaBase + kOffProbe;
      l3[e] = (hole && in_probe) ? 0 : (pa | kPtePage);
    }
  }
}

bool set_start(uint64_t pc, bool mmu) {
  return check(hv_vcpu_set_reg(g_vcpu, HV_REG_PC, pc), "set PC") &&
         check(hv_vcpu_set_reg(g_vcpu, HV_REG_CPSR, 0x3c5), "set CPSR") &&
         check(hv_vcpu_set_sys_reg(g_vcpu, HV_SYS_REG_SCTLR_EL1,
                                   mmu ? kSctlrMmuOn : kSctlrMmuOff),
               "set SCTLR");
}

std::atomic<bool> g_running{false};
// A guest loop that never exits would hang the tool: cancel the vCPU after
// 20 s of one run (hv_vcpus_exit from another thread -> HV_EXIT_REASON_CANCELED).
void watchdog() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    static int ticks = 0;
    if (g_running.load()) {
      if (++ticks > 200) {
        hv_vcpu_t v = g_vcpu;
        hv_vcpus_exit(&v, 1);
        ticks = 0;
      }
    } else {
      ticks = 0;
    }
  }
}

// Run until an HVC (returns true) or a fault the caller does not expect.
// `mmio` handles the unmapped-IPA load by writing x1 and stepping the PC.
bool run_until_hvc(int max_exits, int *exits, bool mmio) {
  *exits = 0;
  for (;;) {
    g_running.store(true);
    const hv_return_t rr = hv_vcpu_run(g_vcpu);
    g_running.store(false);
    if (!check(rr, "run"))
      return false;
    (*exits)++;
    if (g_exit->reason == HV_EXIT_REASON_CANCELED) {
      uint64_t pc;
      hv_vcpu_get_reg(g_vcpu, HV_REG_PC, &pc);
      printf("hvprobe: the guest ran for 20 s without leaving (pc=%llx); "
             "cancelled\n",
             (unsigned long long)pc);
      return false;
    }
    if (g_exit->reason != HV_EXIT_REASON_EXCEPTION) {
      printf("hvprobe: exit reason %u\n", (unsigned)g_exit->reason);
      return false;
    }
    const unsigned ec = (unsigned)(g_exit->exception.syndrome >> 26) & 0x3f;
    if (ec == 0x16) // HVC64
      return true;
    if (ec == 0x24 && mmio) { // data abort, lower EL from EL2's view
      uint64_t pc;
      hv_vcpu_get_reg(g_vcpu, HV_REG_PC, &pc);
      hv_vcpu_set_reg(g_vcpu, HV_REG_X1, 0x1234);
      hv_vcpu_set_reg(g_vcpu, HV_REG_PC, pc + 4);
      if (*exits >= max_exits)
        return true;
      continue;
    }
    uint64_t pc;
    hv_vcpu_get_reg(g_vcpu, HV_REG_PC, &pc);
    printf("hvprobe: unexpected exception EC=0x%x syndrome=%llx va=%llx pa=%llx "
           "pc=%llx\n",
           ec, (unsigned long long)g_exit->exception.syndrome,
           (unsigned long long)g_exit->exception.virtual_address,
           (unsigned long long)g_exit->exception.physical_address,
           (unsigned long long)pc);
    return false;
  }
}

} // namespace

int main_hvprobe(int argc, char **argv) {
  (void)argc;
  (void)argv;
  setvbuf(stdout, nullptr, _IONBF, 0);
  printf("Alphabox hvprobe: Hypervisor.framework mechanics on this host\n");
  std::thread(watchdog).detach();
  if (!check(hv_vm_create(nullptr), "hv_vm_create"))
    return 1;
  g_mem = (uint8_t *)mmap(nullptr, kMemSize, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_mem == MAP_FAILED) {
    printf("hvprobe: mmap failed\n");
    return 1;
  }
  memset(g_mem, 0, kMemSize);
  if (!check(hv_vm_map(g_mem, kIpaBase, kMemSize,
                       HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC),
             "hv_vm_map"))
    return 1;
  if (!check(hv_vcpu_create(&g_vcpu, &g_exit, nullptr), "hv_vcpu_create"))
    return 1;

  // 1. What the CPU says about the MMU we would build the Alpha TB on.
  uint64_t mmfr0 = 0;
  if (check(hv_vcpu_get_sys_reg(g_vcpu, HV_SYS_REG_ID_AA64MMFR0_EL1, &mmfr0),
            "read ID_AA64MMFR0_EL1")) {
    const unsigned parange = mmfr0 & 0xf, asid = (mmfr0 >> 4) & 0xf;
    const unsigned tg16 = (mmfr0 >> 20) & 0xf, tg64 = (mmfr0 >> 24) & 0xf,
                   tg4 = (mmfr0 >> 28) & 0xf;
    static const char *const pa[] = {"32", "36", "40", "42", "44", "48", "52"};
    printf("  ID_AA64MMFR0_EL1 = %016llx: ASID %s bits, PA range %s bits, "
           "granules 4K:%s 16K:%s 64K:%s\n",
           (unsigned long long)mmfr0, asid == 2 ? "16" : "8",
           parange < 7 ? pa[parange] : "?", tg4 == 0 ? "yes" : "no",
           tg16 == 1 ? "yes" : "no", tg64 == 0 ? "yes" : "no");
  }

  int exits = 0;
  // 2. Exit round trip: HVC in a loop, N exits.
  place(kOffCodeA, gen_hvc_loop);
  if (!set_start(kIpaBase + kOffCodeA, false))
    return 1;
  const int kN = 20000;
  double t0 = now_ns();
  for (int i = 0; i < kN; i++)
    if (!run_until_hvc(1, &exits, false))
      return 1;
  double t1 = now_ns();
  printf("  exit round trip (HVC):            %7.0f ns  (%d exits)\n",
         (t1 - t0) / kN, kN);

  // 3. The MMIO case: a load from an IPA nothing is mapped at, emulated on
  // the host, PC stepped, resumed.
  place(kOffCodeB, gen_mmio_loop);
  if (!set_start(kIpaBase + kOffCodeB, false))
    return 1;
  t0 = now_ns();
  if (!run_until_hvc(kN, &exits, true))
    return 1;
  t1 = now_ns();
  printf("  exit round trip (MMIO load):      %7.0f ns  (%d exits)\n",
         (t1 - t0) / exits, exits);

  // 4. Raw speed for scale: 400M iterations of a 5-instruction loop.
  place(kOffCodeD, gen_alu_loop);
  if (!set_start(kIpaBase + kOffCodeD, false))
    return 1;
  t0 = now_ns();
  if (!run_until_hvc(1, &exits, false))
    return 1;
  t1 = now_ns();
  printf("  in-VM loop, 2 G instructions:     %7.0f ms  (%.0f MIPS)\n",
         (t1 - t0) / 1e6, 2000e6 / (t1 - t0) * 1e3);

  // 5. Stage-1 faults handled inside the VM. First with every page mapped
  // (the loop's own cost), then with the probe pages unmapped so each load
  // faults to the EL1 vector, which maps the page and returns.
  place(kOffVectors + 0x200, gen_fault_handler);
  const uint64_t tcr = 25 | (1u << 8) | (1u << 10) | (3u << 12) |
                       (1u << 23) | (2ull << 32); // T0SZ=25, WB, ISH, 4K, EPD1
  if (!check(hv_vcpu_set_sys_reg(g_vcpu, HV_SYS_REG_MAIR_EL1, 0xff), "MAIR") ||
      !check(hv_vcpu_set_sys_reg(g_vcpu, HV_SYS_REG_TCR_EL1, tcr), "TCR") ||
      !check(hv_vcpu_set_sys_reg(g_vcpu, HV_SYS_REG_TTBR0_EL1, kIpaBase + kOffL1),
             "TTBR0") ||
      !check(hv_vcpu_set_sys_reg(g_vcpu, HV_SYS_REG_VBAR_EL1,
                                 kIpaBase + kOffVectors),
             "VBAR"))
    return 1;
  double mapped_ns = 0, faulting_ns = 0;
  // Pass 0 warms the pages (the first guest touch of a page populates its
  // stage-2 mapping in the host kernel, which is not what is measured);
  // pass 1 is the loop's own cost with every page mapped; pass 2 faults on
  // every page.
  place(kOffCodeC, gen_touch_loop);
  for (int pass = 0; pass < 3; pass++) {
    build_tables(pass == 2);
    if (!set_start(kIpaBase + kOffCodeC, true))
      return 1;
    t0 = now_ns();
    if (!run_until_hvc(1, &exits, false))
      return 1;
    t1 = now_ns();
    if (pass == 1)
      mapped_ns = t1 - t0;
    if (pass == 2)
      faulting_ns = t1 - t0;
  }
  printf("  4096 page touches, all mapped:    %7.0f ns total\n", mapped_ns);
  printf("  4096 page touches, each faulting: %7.0f ns total -> %.0f ns per "
         "in-VM fault (map + eret), %d exits\n",
         faulting_ns, (faulting_ns - mapped_ns) / 4096, exits);

  hv_vcpu_destroy(g_vcpu);
  hv_vm_unmap(kIpaBase, kMemSize);
  hv_vm_destroy();
  munmap(g_mem, kMemSize);
  return 0;
}

#endif // ALPHABOX_HVF
