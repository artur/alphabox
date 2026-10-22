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

/* Unit test of the PCI-PCI bridge (src/devices/pci/bridge/), a DECchip 21152
 * in hose 0 slot 3 with devices behind it, configured the way the ES40's SRM
 * console configures one (the order and values of its configuration writes,
 * from an ALPHABOX_RECORD of `show config`), and driven through its windows.
 * Against the PCI-to-PCI Bridge Architecture Specification 1.1 (type 1
 * header 3.2, PDF p. 25; Table 3-9 bridge control, PDF p. 49; master abort
 * 6.3.1, PDF p. 101; Table 9-1 interrupt binding, PDF p. 114; secondary
 * reset 11.1.1, PDF p. 119) and the Intel 21152 data sheet (278060-002).
 * Built with every lane as `pcibridge_test`; exit status 0 when every check
 * passes.
 */

#include "Configurator.hpp"
#include "PCIBridge.hpp"
#include "PCIDevice.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

// ---- stubs for what the PCI layer needs from the rest of the machine ----

/// The ranges registered with the system: (component, index) -> where.
struct placed {
  u64 base, length;
};
static std::map<std::pair<const CSystemComponent *, int>, placed> ranges;

CSystemComponent::CSystemComponent(CConfigurator *cfg, CSystem *system)
    : devid_string((char *)"pci(test)"), cSystem(system), myCfg(cfg) {}
CSystemComponent::~CSystemComponent() {}
int CSystem::RegisterMemory(CSystemComponent *c, int index, u64 base,
                            u64 length) {
  ranges[{c, index}] = {base, length};
  return 0;
}
char *CSystem::PtrToMem(u64) { return nullptr; }
u64 CSystem::ReadMem(u64, int, CSystemComponent *) { return 0; }
void CSystem::WriteMem(u64, int, u64, CSystemComponent *) {}
void CSystem::trace_unknown(const char *, u64, int, bool, u64,
                            CSystemComponent *) {}
CSystem::CPCIDMAWriteGuard::CPCIDMAWriteGuard(CSystem *s, bool) : system(s) {}
CSystem::CPCIDMAWriteGuard::~CPCIDMAWriteGuard() {}
void CSystem::CPCIDMAWriteGuard::invalidate(u64, size_t) {}
namespace access_record {
extern const bool g_on = false;
bool selected(const char *) { return false; }
void note(const char *, const char *, u64, int, bool, u64) {}
} // namespace access_record

/// The Tsunami's hose spaces, which is all the PCI layer asks a chipset.
class CTestChipset : public CChipset {
public:
  CTestChipset() : CChipset(nullptr) {}
  chipset_kind kind() const override { return CHIPSET_TSUNAMI; }
  const char *name() const override { return "test"; }
  u64 phys_mask() const override { return ~U64(0); }
  u64 read_io(u64, u64, int, CSystemComponent *) override { return 0; }
  void write_io(u64, u64, int, u64, CSystemComponent *) override {}
  void interrupt(int, bool) override {}
  void interval_tick() override {}
  void ack_interval_timer(int) override {}
  void ack_ipi(int) override {}
  u64 pci_space_base(int hose, pci_space space) const override {
    const u64 stride = U64(0x0000000200000000) * hose;
    switch (space) {
    case PCI_SPACE_IO:
      return U64(0x00000801fc000000) + stride;
    case PCI_SPACE_CONFIG:
      return U64(0x00000801fe000000) + stride;
    default:
      return U64(0x0000080000000000) + stride;
    }
  }
  u64 pci_phys(int, u32 a) override { return a; }
  void reset() override {}
  void save_state(FILE *) override {}
  bool restore_state(FILE *) override { return true; }
};

/// The ES40's wiring: input (slot + 1) * 4 + hose * 16 + INTx.
static int test_pci_interrupt(int hose, int slot, int intx, int) {
  return ((slot + 1) * 4 + (hose & 3) * 0x10 + (intx & 3)) & 0x3f;
}

// The system and the configuration node are raw storage, as in the other
// unit tests: no constructor of theirs runs. The PCI layer reads three
// pointers from them inline (the chipset, the board row, the node's name),
// so the test finds where each lives by writing a marker into the storage
// and seeing which offset the public accessor returns, and puts its own
// there.
alignas(CConfigurator) static unsigned char cfg_storage[sizeof(CConfigurator)];
alignas(CSystem) static unsigned char sys_storage[sizeof(CSystem)];
static CConfigurator *const cfg =
    reinterpret_cast<CConfigurator *>(cfg_storage);
static CSystem *const sys = reinterpret_cast<CSystem *>(sys_storage);

template <class Get>
static void plant(unsigned char *storage, size_t size, Get get,
                  const void *value) {
  for (size_t off = 0; off + sizeof(void *) <= size; off += alignof(void *)) {
    const void *marker = reinterpret_cast<const void *>(0x5a5a0000 + off);
    memcpy(storage + off, &marker, sizeof(marker));
    if (get() == marker) {
      memcpy(storage + off, &value, sizeof(value));
      return;
    }
    memset(storage + off, 0, sizeof(marker));
  }
  printf("pcibridge_test: cannot find the member to plant\n");
  exit(2);
}

static CTestChipset chipset;
static platform_config board{};
static char node_name[] = "pci";
static char node_value[] = "test";

static void setup() {
  board.pci_interrupt = test_pci_interrupt;
  plant(
      sys_storage, sizeof(sys_storage),
      [] { return static_cast<const void *>(sys->chipset()); }, &chipset);
  plant(
      sys_storage, sizeof(sys_storage),
      [] { return static_cast<const void *>(&sys->platform()); }, &board);
  plant(
      cfg_storage, sizeof(cfg_storage),
      [] { return static_cast<const void *>(cfg->get_myName()); }, node_name);
  plant(
      cfg_storage, sizeof(cfg_storage),
      [] { return static_cast<const void *>(cfg->get_myValue()); }, node_value);
}

// ---- the devices ----

/// A bridge, at the root of hose 0 or behind another bridge.
class CTestBridge : public CPCIBridge {
public:
  CTestBridge(CTestBridge *up, int dev, const char *chip)
      : CPCIBridge(cfg, sys, 0, dev, *CPCIBridge::find_chip(chip)) {
    if (up) {
      myBridge = up;
      up->attach(this);
    }
    init();
  }
  void ResetPCI() override {
    resets++;
    CPCIBridge::ResetPCI();
  }
  int resets = 0;
};

/// A device with a 256-byte I/O BAR and a 4 KB memory BAR, INTA.
class CTestDev : public CPCIDevice {
public:
  CTestDev(CTestBridge *up, int dev) : CPCIDevice(cfg, sys, 0, dev) {
    myBridge = up;
    up->attach(this);
    u32 d[64] = {}, m[64] = {};
    d[0x00 >> 2] = 0x00011011;
    d[0x08 >> 2] = 0x02000000;
    d[0x10 >> 2] = 0x00000001;
    d[0x3c >> 2] = 0x000001ff;
    m[0x04 >> 2] = 0x00000157;
    m[0x10 >> 2] = 0xffffff00;
    m[0x14 >> 2] = 0xfffff000;
    m[0x3c >> 2] = 0x000000ff;
    add_function(0, d, m);
    ResetPCI();
  }
  void ResetPCI() override {
    resets++;
    CPCIDevice::ResetPCI();
  }
  u32 ReadMem_Bar(int func, int bar, u32 address, int dsize) override {
    return 0xc0de0000 | (u32)bar << 12 | address;
  }
  void WriteMem_Bar(int, int bar, u32 address, int, u32 data) override {
    last_write = (u32)bar << 28 | address << 16 | (data & 0xffff);
  }
  int input() const { return interrupt_input(0); }
  int resets = 0;
  u32 last_write = 0;
};

// ---- the harness ----

static int checks = 0, failures = 0;

#define CHECK_EQ(got, want)                                                    \
  do {                                                                         \
    const unsigned long long g_ = (unsigned long long)(got),                   \
                             w_ = (unsigned long long)(want);                  \
    checks++;                                                                  \
    if (g_ != w_) {                                                            \
      failures++;                                                              \
      printf("FAIL %s:%d: %s = 0x%llx, want 0x%llx\n", __FILE__, __LINE__,     \
             #got, g_, w_);                                                    \
    }                                                                          \
  } while (0)

static const int kConfig = PCI_RANGE_BASE + 7; // function 0's configuration
// The bridge's windows, as PCIBridge.cpp numbers them.
static const int kWinIo = 0x1000, kWinMemory = 0x1001, kWinVgaMemory = 0x1003,
                 kWinVgaColour = 0x1005;

static u64 cfg_rd(CPCIDevice *d, u32 a, int bits) {
  return d->ReadMem(kConfig, a, bits);
}
static void cfg_wr(CPCIDevice *d, u32 a, int bits, u64 v) {
  d->WriteMem(kConfig, a, bits, v);
}
static placed where(const CSystemComponent *c, int index) {
  auto i = ranges.find({c, index});
  return i == ranges.end() ? placed{~U64(0), ~U64(0)} : i->second;
}

/// What SRM writes to a bridge it numbers (the ES40 console, ppb3 record):
/// the command register off, the bus numbers, the windows, the bridge
/// control, the command register on.
static void srm_configure(CPCIDevice *b, int primary, int secondary,
                          int subordinate, u8 io_base, u8 io_limit,
                          u16 mem_base, u16 mem_limit) {
  cfg_wr(b, 0x04, 16, 0);
  cfg_wr(b, 0x18, 8, primary);
  cfg_wr(b, 0x19, 8, secondary);
  cfg_wr(b, 0x1a, 8, subordinate);
  cfg_wr(b, 0x1e, 16, 0xffff); // secondary status: clear what is set
  cfg_wr(b, 0x1c, 8, io_base);
  cfg_wr(b, 0x1d, 8, io_limit);
  cfg_wr(b, 0x30, 16, 0); // I/O base and limit, upper 16 bits
  cfg_wr(b, 0x32, 16, 0);
  cfg_wr(b, 0x20, 16, mem_base);
  cfg_wr(b, 0x22, 16, mem_limit);
  cfg_wr(b, 0x24, 16, 0xffff); // the prefetchable window closed
  cfg_wr(b, 0x3e, 16, 0x0003);
  cfg_wr(b, 0x0c, 16, 0xff10);
  cfg_wr(b, 0x04, 16, 0x0007);
}

/// The type 1 header of a 21152 (3.2, PDF p. 25; 21152 data sheet 13.1).
static void test_header() {
  CTestBridge b(nullptr, 3, "dec21152");
  CHECK_EQ(cfg_rd(&b, 0x00, 32), 0x00241011);
  CHECK_EQ(cfg_rd(&b, 0x08, 32) >> 8, 0x060400); // PCI-PCI bridge
  CHECK_EQ(cfg_rd(&b, 0x0e, 8), 0x01);           // type 1, one function
  CHECK_EQ(cfg_rd(&b, 0x18, 32), 0);             // bus numbers after reset
  cfg_wr(&b, 0x18, 32, 0xff050401);
  CHECK_EQ(cfg_rd(&b, 0x18, 32), 0xff050401);
  CHECK_EQ(b.secondary_bus(), 4);
  cfg_wr(&b, 0x1c, 16, 0xffff);
  CHECK_EQ(cfg_rd(&b, 0x1c, 16), 0xf1f1); // 32-bit I/O, bits 3:0 RO
  cfg_wr(&b, 0x20, 32, 0xffffffff);
  CHECK_EQ(cfg_rd(&b, 0x20, 32), 0xfff0fff0);
  cfg_wr(&b, 0x10, 32, 0xffffffff);
  CHECK_EQ(cfg_rd(&b, 0x10, 32), 0); // no BARs
  cfg_wr(&b, 0x38, 32, 0xfffffffe);
  CHECK_EQ(cfg_rd(&b, 0x38, 32), 0); // no expansion ROM
  CHECK_EQ(cfg_rd(&b, 0x3d, 8), 0);  // no interrupt pin
  cfg_wr(&b, 0x3c, 32, 0xffffffff);
  CHECK_EQ(cfg_rd(&b, 0x3e, 16), 0x0fff); // bridge control <11:0>

  // Status: medium DEVSEL, fast back-to-back, and (revision 03h, the
  // 21152-AB) the capability list: power management at DCh, PM 1.0, no
  // D1/D2, no PME#, so PME_En reads 0 (13.1.4, 13.1.27-13.1.33).
  CHECK_EQ(cfg_rd(&b, 0x06, 16), 0x0290);
  CHECK_EQ(cfg_rd(&b, 0x1e, 16), 0x0280); // the secondary side the same
  CHECK_EQ(cfg_rd(&b, 0x34, 8), 0xdc);
  CHECK_EQ(cfg_rd(&b, 0xdc, 32), 0x00010001); // PM, end of list, PMC
  cfg_wr(&b, 0xe0, 16, 0xffff);
  CHECK_EQ(cfg_rd(&b, 0xe0, 16), 0x0003); // the power state alone
  cfg_wr(&b, 0xe0, 16, 0);

  // A 21050 has no capabilities, and its status says so (PCI 2.2 6.2.3).
  CTestBridge old(nullptr, 4, "dec21050");
  CHECK_EQ(cfg_rd(&old, 0x06, 16), 0x0280);
  CHECK_EQ(cfg_rd(&old, 0x34, 8), 0);
  CHECK_EQ(cfg_rd(&old, 0x1c, 8), 0x00); // 16-bit I/O only
}

/// Behind the bridge nothing answers configuration cycles until the bridge
/// has a secondary bus number; then a device answers at that bus's address
/// (type 1 cycles, 3.2.2.3.1 of PCI 2.2, as the Tsunami lays them out).
static void test_config_mapping() {
  ranges.clear();
  CTestBridge b(nullptr, 3, "dec21152");
  CTestDev d(&b, 2);
  CHECK_EQ(where(&d, kConfig).length, 0);
  cfg_wr(&b, 0x19, 8, 2);
  CHECK_EQ(where(&d, kConfig).base, U64(0x801fe000000) + (2 << 16) + (2 << 11));
  CHECK_EQ(where(&d, kConfig).length, 0x100);
  cfg_wr(&b, 0x19, 8, 0);
  CHECK_EQ(where(&d, kConfig).length, 0);
}

/// The windows the console opens, what reaches a device behind them, and
/// what a master abort reads (6.3.1, PDF p. 101).
static void test_windows() {
  ranges.clear();
  CTestBridge b(nullptr, 3, "dec21152");
  CTestDev d(&b, 2);
  // I/O 1000-1fff, memory 01000000-012fffff, as SRM lays out ppb3.
  srm_configure(&b, 0, 2, 2, 0x10, 0x10, 0x0100, 0x0120);
  CHECK_EQ(where(&b, kWinIo).base, U64(0x801fc001000));
  CHECK_EQ(where(&b, kWinIo).length, 0x1000);
  CHECK_EQ(where(&b, kWinMemory).base, U64(0x80001000000));
  CHECK_EQ(where(&b, kWinMemory).length, 0x300000);

  // The device's BARs, inside the windows.
  cfg_wr(&d, 0x10, 32, 0x00001001);
  cfg_wr(&d, 0x14, 32, 0x01000000);
  cfg_wr(&d, 0x04, 16, 0x0007);
  CHECK_EQ(b.ReadMem(kWinIo, 0x010, 32), 0xc0de0010);
  CHECK_EQ(b.ReadMem(kWinMemory, 0x0020, 32), 0xc0de1020);
  b.WriteMem(kWinIo, 0x004, 32, 0x1234);
  CHECK_EQ(d.last_write, 0x00041234);

  // Inside a window, outside any BAR: a master abort on the secondary.
  CHECK_EQ(b.ReadMem(kWinIo, 0x800, 32), 0xffffffff);
  CHECK_EQ(b.ReadMem(kWinIo, 0x800, 8), 0xff);
  CHECK_EQ(b.ReadMem(kWinIo, 0x800, 16), 0xffff);
  CHECK_EQ(b.ReadMem(kWinMemory, 0x100000, 32), 0); // memory: as the hose

  // ISA enable: the top 768 bytes of each kilobyte stay on the primary
  // side (Table 3-9 bit 2), so a BAR there is out of reach.
  cfg_wr(&d, 0x10, 32, 0x00001101);
  CHECK_EQ(b.ReadMem(kWinIo, 0x110, 32), 0xc0de0010);
  cfg_wr(&b, 0x3e, 16, 0x0007);
  CHECK_EQ(b.ReadMem(kWinIo, 0x110, 32), 0xffffffff);
  cfg_wr(&b, 0x3e, 16, 0x0003);

  // The command register's enables open and close the windows.
  cfg_wr(&b, 0x04, 16, 0x0006);
  CHECK_EQ(where(&b, kWinIo).length, 0);
  CHECK_EQ(where(&b, kWinMemory).length, 0x300000);
  cfg_wr(&b, 0x04, 16, 0x0007);
  CHECK_EQ(where(&b, kWinIo).length, 0x1000);

  // VGA enable: the legacy graphics addresses, whatever the windows say.
  cfg_wr(&b, 0x3e, 16, 0x000b);
  CHECK_EQ(where(&b, kWinVgaMemory).base, U64(0x800000a0000));
  CHECK_EQ(where(&b, kWinVgaMemory).length, 0x20000);
  CHECK_EQ(where(&b, kWinVgaColour).base, U64(0x801fc0003c0));
  CHECK_EQ(where(&b, kWinVgaColour).length, 0x20);
  cfg_wr(&b, 0x3e, 16, 0x0003);
  CHECK_EQ(where(&b, kWinVgaMemory).length, 0);

  // A window whose base is above its limit is closed.
  cfg_wr(&b, 0x1c, 8, 0x20);
  CHECK_EQ(where(&b, kWinIo).length, 0);
}

/// The interrupt binding behind a bridge (Table 9-1, PDF p. 114): device
/// d's INTA reaches the primary pin INT((d) mod 4), so devices 1 and 5
/// share one, and at the slot the ES40 wiring takes it to its input.
static void test_swizzle() {
  CTestBridge b(nullptr, 3, "dec21152");
  CTestDev d0(&b, 0), d1(&b, 1), d2(&b, 2), d5(&b, 5);
  CHECK_EQ(d0.input(), 16); // slot 3: (3 + 1) * 4, INTA
  CHECK_EQ(d1.input(), 17); // INTB
  CHECK_EQ(d2.input(), 18); // INTC
  CHECK_EQ(d5.input(), 17); // INTB again: shares d1's input
  // A bridge behind the bridge rotates again, by its own device number.
  CTestBridge inner(&b, 1, "dec21152");
  CTestDev g(&inner, 2);
  CHECK_EQ(g.input(), 16 + ((0 + 2 + 1) & 3));
}

/// The secondary bus reset (Table 3-9 bit 6, 11.1.1 PDF p. 119) resets the
/// devices on the secondary bus and, through a bridge among them, the
/// buses below it, whose numbers clear with the bridge's registers.
static void test_secondary_reset() {
  ranges.clear();
  CTestBridge b(nullptr, 3, "dec21152");
  CTestBridge inner(&b, 1, "dec21152");
  CTestDev d(&b, 2);
  CTestDev g(&inner, 3);
  srm_configure(&b, 0, 2, 3, 0x10, 0x10, 0x0100, 0x0120);
  srm_configure(&inner, 2, 3, 3, 0x10, 0x10, 0x0100, 0x0110);
  CHECK_EQ(where(&g, kConfig).length, 0x100); // bus 3 numbered
  const int d0 = d.resets, i0 = inner.resets, g0 = g.resets;
  cfg_wr(&b, 0x3e, 16, 0x0043);
  CHECK_EQ(d.resets - d0, 1);
  CHECK_EQ(inner.resets - i0, 1);
  CHECK_EQ(g.resets - g0, 1); // the bus behind the inner bridge too
  CHECK_EQ(inner.secondary_bus(), 0);
  CHECK_EQ(where(&g, kConfig).length, 0); // until it is numbered again
  CHECK_EQ(cfg_rd(&b, 0x19, 8), 2);       // the bridge itself keeps its numbers
  // Held in reset, the bit set again resets nothing more; cleared, nothing.
  cfg_wr(&b, 0x3e, 16, 0x0043);
  cfg_wr(&b, 0x3e, 16, 0x0003);
  CHECK_EQ(d.resets - d0, 1);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0); // a failure that crashes still shows
  setup();
  test_header();
  test_config_mapping();
  test_windows();
  test_swizzle();
  test_secondary_reset();
  printf("pcibridge_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
