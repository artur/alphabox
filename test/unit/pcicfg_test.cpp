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

/* Unit test of the PCI configuration space every device inherits
 * (src/devices/pci/PCIDevice.cpp), driven through the configuration range as
 * the hose delivers cycles to it: register write masks, BAR and expansion ROM
 * sizing, the access sizes, accesses that run past a function's 256 bytes,
 * and the state file. Against the PCI Local Bus Specification 2.2 (6.1, PDF
 * p. 210; 6.2.5.1, PDF p. 221; 6.2.5.2, PDF p. 224). Built with every lane
 * as `pcicfg_test`; exit status 0 when every check passes.
 *
 * Assigning a BAR an address places a range through the chipset, which this
 * test does not have, and a ROM BAR write with the enable clear is reported
 * with the configuration node's name, which it does not have either: those
 * writes are left to the SRM records (docs/development.md, "Device access
 * records").
 */

#include "Configurator.hpp"
#include "PCIBridge.hpp"
#include "PCIDevice.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include <cstdio>
#include <cstring>
#include <new>

// ---- stubs for what PCIDevice.cpp needs from the rest of the machine ----

static int ranges_registered = 0;

CSystemComponent::CSystemComponent(CConfigurator *cfg, CSystem *system)
    : devid_string((char *)"pci0.1(test)"), cSystem(system), myCfg(cfg) {}
CSystemComponent::~CSystemComponent() {}
int CSystem::RegisterMemory(CSystemComponent *, int, u64, u64) {
  ranges_registered++;
  return 0;
}
char *CSystem::PtrToMem(u64) { return nullptr; }
u64 CSystem::ReadMem(u64, int, CSystemComponent *) { return 0; }
void CSystem::WriteMem(u64, int, u64, CSystemComponent *) {}
CSystem::CPCIDMAWriteGuard::CPCIDMAWriteGuard(CSystem *s, bool) : system(s) {}
CSystem::CPCIDMAWriteGuard::~CPCIDMAWriteGuard() {}
void CSystem::CPCIDMAWriteGuard::invalidate(u64, size_t) {}
void CPCIBridge::attach(CPCIDevice *) {}
int CPCIBridge::secondary_bus() const { return 0; }
void CPCIBridge::map_child_range(CPCIDevice *, int, bool, u32, u64) {}
namespace access_record {
extern const bool g_on = false;
bool selected(const char *) { return false; }
void note(const char *, const char *, u64, int, bool, u64) {}
} // namespace access_record

alignas(CConfigurator) static unsigned char cfg_storage[sizeof(CConfigurator)];
alignas(CSystem) static unsigned char sys_storage[sizeof(CSystem)];

// ---- the device ----

/// A two-function device in the shape of the Symbios parts: an I/O BAR, a
/// memory BAR, a 64 KB expansion ROM, an interrupt pin per function.
class CTestDev : public CPCIDevice {
public:
  CTestDev()
      : CPCIDevice(reinterpret_cast<CConfigurator *>(cfg_storage),
                   reinterpret_cast<CSystem *>(sys_storage), 0, 1) {}
  void build(int functions) {
    for (int f = 0; f < functions; f++) {
      u32 d[64] = {}, m[64] = {};
      d[0x00 >> 2] = 0x000f1000;
      d[0x04 >> 2] = 0x02000000;
      d[0x08 >> 2] = 0x01000002;
      d[0x0c >> 2] = functions > 1 ? 0x00800000 : 0;
      d[0x10 >> 2] = 0x00000001; // I/O
      d[0x3c >> 2] = 0x401100ff | (u32(f + 1) << 8);
      m[0x04 >> 2] = 0x00000157;
      m[0x0c >> 2] = 0x0000ffff;
      m[0x10 >> 2] = 0xffffff00;
      m[0x14 >> 2] = 0xfffff000;
      m[0x30 >> 2] = ~u32(0x10000 - 1) | PCI_ROM_ADDRESS_ENABLE;
      m[0x3c >> 2] = 0x000000ff;
      add_function(f, d, m);
      memcpy(pci_state.config_data[f], d, sizeof(d));
      memcpy(pci_state.config_mask[f], m, sizeof(m));
    }
  }
  u32 raw(int func, int dword) const {
    return pci_state.config_data[func][dword];
  }
  void poison_absent(int func) {
    memset(pci_state.config_data[func], 0xaa, 256);
    memset(pci_state.config_mask[func], 0xff, 256);
  }
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

/// A configuration cycle as the hose delivers it to function `f`.
static u64 rd(CTestDev &d, int f, u32 a, int bits) {
  return d.ReadMem(PCI_RANGE_BASE + f * 8 + 7, a, bits);
}
static void wr(CTestDev &d, int f, u32 a, int bits, u64 v) {
  d.WriteMem(PCI_RANGE_BASE + f * 8 + 7, a, bits, v);
}

/// The header's read-only and read-write fields (PCI 2.2 6.1, 6.2.2).
static void test_masks() {
  CTestDev d;
  d.build(1);
  wr(d, 0, 0x00, 32, 0xffffffff);
  CHECK_EQ(rd(d, 0, 0x00, 32), 0x000f1000); // vendor and device ID: RO
  wr(d, 0, 0x04, 16, 0xffff);
  CHECK_EQ(rd(d, 0, 0x04, 16), 0x0157); // the enables the part has
  CHECK_EQ(rd(d, 0, 0x06, 16), 0x0200); // status untouched
  wr(d, 0, 0x04, 8, 0x00);
  CHECK_EQ(rd(d, 0, 0x04, 32), 0x02000100); // a byte write: low byte only
  wr(d, 0, 0x08, 32, 0);
  CHECK_EQ(rd(d, 0, 0x08, 32), 0x01000002); // class and revision: RO
  wr(d, 0, 0x0c, 32, 0xffffffff);
  CHECK_EQ(rd(d, 0, 0x0c, 32), 0x0000ffff); // header type RO, latency RW
  wr(d, 0, 0x3c, 32, 0xffffff0b);
  CHECK_EQ(rd(d, 0, 0x3c, 8), 0x0b); // interrupt line: RW
  CHECK_EQ(rd(d, 0, 0x3d, 8), 0x01); // interrupt pin: RO, INTA
  CHECK_EQ(rd(d, 0, 0x3e, 16), 0x4011);
  CHECK_EQ(rd(d, 0, 0x02, 16), 0x000f); // a word inside a doubleword
  CHECK_EQ(rd(d, 0, 0x00, 64), U64(0x02000100000f1000)); // two cycles
}

/// Writing all ones to a BAR reads back the size; the type bits stay
/// (PCI 2.2 6.2.5.1, PDF p. 221). The expansion ROM BAR the same, its
/// enable bit as written (6.2.5.2, PDF p. 224). A probe places nothing.
static void test_sizing() {
  CTestDev d;
  d.build(1);
  ranges_registered = 0;
  wr(d, 0, 0x10, 32, 0xffffffff);
  CHECK_EQ(rd(d, 0, 0x10, 32), 0xffffff01); // 256 bytes of I/O
  wr(d, 0, 0x14, 32, 0xffffffff);
  CHECK_EQ(rd(d, 0, 0x14, 32), 0xfffff000); // 4 KB of memory, 32-bit
  wr(d, 0, 0x18, 32, 0xffffffff);
  CHECK_EQ(rd(d, 0, 0x18, 32), 0); // not implemented: reads zero
  wr(d, 0, 0x30, 32, 0xffffffff);
  CHECK_EQ(rd(d, 0, 0x30, 32), 0xffff0001); // 64 KB, the enable as written
  wr(d, 0, 0x10, 32, 0);
  wr(d, 0, 0x14, 32, 0);
  wr(d, 0, 0x30, 32, 0);
  CHECK_EQ(rd(d, 0, 0x30, 32), 0);
  CHECK_EQ(ranges_registered, 0);
}

/// An access that runs past the function's 256 bytes. Before the bound, a
/// longword at FDh read three bytes of the next function's vendor ID, and
/// a quadword at FCh its whole first doubleword; a write there wrote them.
static void test_bounds() {
  CTestDev d;
  d.build(2);
  wr(d, 0, 0xfc, 32, 0x12345678); // reserved space: nothing writable
  CHECK_EQ(rd(d, 0, 0xfc, 32), 0);
  CHECK_EQ(rd(d, 0, 0xff, 8), 0);
  CHECK_EQ(rd(d, 0, 0xfd, 32), 0xffffffff);
  CHECK_EQ(rd(d, 0, 0xff, 16), 0xffff);
  CHECK_EQ(rd(d, 0, 0xfc, 64), U64(0xffffffff00000000));
  wr(d, 0, 0xfd, 32, 0xffffffff);
  wr(d, 0, 0xfc, 64, ~U64(0));
  CHECK_EQ(d.raw(1, 0), 0x000f1000); // the next function is untouched
  CHECK_EQ(rd(d, 1, 0x00, 32), 0x000f1000);
  CHECK_EQ(rd(d, 1, 0x3d, 8), 0x02); // its own pin, INTB
  CHECK_EQ(rd(d, 0, 0x0e, 8), 0x80); // multi-function
}

/// The functions a device does not have hold zeros, whatever the memory
/// held before, and a state file that says otherwise for them places
/// nothing on restore.
static void test_absent_functions() {
  alignas(CTestDev) static unsigned char buf[sizeof(CTestDev)];
  memset(buf, 0xaa, sizeof(buf));
  CTestDev *d = new (buf) CTestDev();
  d->build(1);
  CHECK_EQ(d->raw(3, 0x10 >> 2), 0);
  CHECK_EQ(d->raw(7, 63), 0);

  // A state file whose absent functions hold BAR-like values, as an
  // uninitialised one could: restoring it must not place them.
  d->poison_absent(5);
  FILE *f = tmpfile();
  CHECK_EQ(d->SaveState(f), 0);
  rewind(f);
  ranges_registered = 0;
  CHECK_EQ(d->RestoreState(f), 0);
  CHECK_EQ(ranges_registered, 0);
  fclose(f);
  d->~CTestDev();
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0); // a failure that crashes still shows
  test_masks();
  test_sizing();
  test_bounds();
  test_absent_functions();
  printf("pcicfg_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
