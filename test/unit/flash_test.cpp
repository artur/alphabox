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

/* Unit test of the TIG-bus flash (src/platforms/es40/Flash.cpp), one or two
 * AM29F016 parts, driven through its bus range as the consoles do: every
 * byte on its own 64-byte slot, the upper 32-bit lane not wired. Checks the
 * command set against the AM29F016D data sheet (21444E5, Table 9, PDF
 * p. 21), the flash file the emulator
 * keeps, and replays what the ES40's SRM console did to it when setting a
 * console variable (test/unit/data/flash_srm_set.rec). Built with every lane
 * as `flash_test`; exit status 0 when every check passes.
 */

#include "AccessReplay.hpp"
#include "Configurator.hpp"
#include "Flash.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include <sys/stat.h>

// ---- stubs for what Flash.cpp needs from the rest of the machine ----

static const char *rom_file = "flash_test.rom";

CSystemComponent::CSystemComponent(CConfigurator *cfg, CSystem *system)
    : devid_string((char *)"sys0(test)"), cSystem(system), myCfg(cfg) {}
CSystemComponent::~CSystemComponent() {}
int CSystem::RegisterMemory(CSystemComponent *, int, u64, u64) { return 0; }
char *CConfigurator::get_text_value(const char *n, const char *def) {
  return (char *)(strcmp(n, "rom.flash") == 0 ? rom_file : def);
}
alignas(CConfigurator) static unsigned char cfg_storage[sizeof(CConfigurator)];
alignas(CSystem) static unsigned char sys_storage[sizeof(CSystem)];

// ---- the harness ----

static CFlash *fl;
static int checks = 0, failures = 0;

static void check(const char *what, unsigned got, unsigned want) {
  checks++;
  if (got == want)
    return;
  failures++;
  printf("FAIL %s: got %02x, want %02x\n", what, got, want);
}

static u8 rd(u32 byte) { return (u8)fl->ReadMem(0, (u64)byte << 6, 64); }
static void wr(u32 byte, u8 v) { fl->WriteMem(0, (u64)byte << 6, 64, v); }
/// The two unlock cycles and a command, in the part that holds `base`.
static void cmd(u32 base, u8 c) {
  wr(base + 0x5555, 0xaa);
  wr(base + 0x2aaa, 0x55);
  wr(base + 0x5555, c);
}
static void program(u32 byte, u8 v) {
  cmd(byte & ~0xffff, 0xa0);
  wr(byte, v);
}
static void erase_sector(u32 byte) {
  cmd(byte & ~0xffff, 0x80);
  wr((byte & ~0xffff) + 0x5555, 0xaa);
  wr((byte & ~0xffff) + 0x2aaa, 0x55);
  wr(byte, 0x30);
}

/// A part whose file does not exist yet: blank, and the file created.
static CFlash *fresh(int chips) {
  remove(rom_file);
  theSROM = nullptr;
  return new CFlash(reinterpret_cast<CConfigurator *>(cfg_storage),
                    reinterpret_cast<CSystem *>(sys_storage), chips);
}

static void done() {
  delete fl; // flushes what is dirty
  fl = nullptr;
}

static void test_autoselect() {
  fl = fresh(1);
  check("blank part reads ff", rd(0x1234), 0xff);
  cmd(0, 0x90);
  check("autoselect: manufacturer (AMD)", rd(0), 0x01);
  check("autoselect: device (AM29F016)", rd(1), 0xad);
  check("autoselect: sector 0 not protected", rd(2), 0x00);
  // The codes repeat in every sector: firmware reads them at the sector's
  // own base.
  check("autoselect: manufacturer at sector 5", rd(0x50000), 0x01);
  check("autoselect: device at sector 5", rd(0x50001), 0xad);
  wr(0, 0xf0);
  check("reset returns to the array", rd(0), 0xff);
  // The consoles reset with the full unlock sequence as well.
  cmd(0, 0x90);
  cmd(0, 0xf0);
  check("unlocked reset returns to the array", rd(1), 0xff);
  done();
}

static void test_program() {
  fl = fresh(1);
  program(0x1234, 0x5a);
  check("program", rd(0x1234), 0x5a);
  program(0x1234, 0xff);
  check("program cannot turn a 0 into a 1", rd(0x1234), 0x5a);
  program(0x1234, 0x0f);
  check("program clears only bits written 0", rd(0x1234), 0x0a);
  check("program leaves its neighbour", rd(0x1235), 0xff);
  // The data sheet's own unlock addresses: A10-A0 are decoded, the rest
  // are don't care (Table 9 note 4).
  wr(0x555, 0xaa);
  wr(0x2aa, 0x55);
  wr(0x555, 0xa0);
  wr(0x3000, 0x12);
  check("program unlocked at 555/2AA", rd(0x3000), 0x12);
  wr(0x7d555, 0xaa); // A20-A11 set: still the unlock cycle
  wr(0x7a2aa, 0x55);
  wr(0x7d555, 0xa0);
  wr(0x3001, 0x34);
  check("unlock ignores A20-A11", rd(0x3001), 0x34);
  // A bad third cycle drops the sequence; the next write is not data.
  wr(0x5555, 0xaa);
  wr(0x2aaa, 0x55);
  wr(0x5555, 0x77);
  wr(0x1234, 0x00);
  check("unknown command programs nothing", rd(0x1234), 0x0a);
  wr(0x5555, 0xaa);
  wr(0x5555, 0x55); // second cycle at the wrong address
  wr(0x5555, 0xa0);
  wr(0x1234, 0x00);
  check("misaddressed unlock programs nothing", rd(0x1234), 0x0a);
  done();
}

static void test_erase() {
  fl = fresh(1);
  program(0x1234, 0x00);
  program(0x10005, 0x00);
  erase_sector(0x0100);
  check("sector erase reports done (DQ7 = 1)", rd(0x0100), 0x80);
  check("second status read", rd(0x0100), 0x80);
  check("sector erase clears its sector", rd(0x1234), 0xff);
  check("sector erase leaves the next sector", rd(0x10005), 0x00);
  cmd(0, 0x80);
  wr(0x5555, 0xaa);
  wr(0x2aaa, 0x55);
  wr(0x5555, 0x10);
  rd(0); // status
  rd(0);
  check("chip erase clears every sector", rd(0x10005), 0xff);
  done();
}

static void test_bus() {
  fl = fresh(1);
  check("upper lane reads 0", (u8)fl->ReadMem(0, ((u64)0x1234 << 6) | 4, 64),
        0x00);
  cmd(0, 0xa0);
  fl->WriteMem(0, ((u64)0x2000 << 6) | 4, 64, 0x00); // not wired
  check("upper-lane write programs nothing", rd(0x2000), 0xff);
  wr(0x2000, 0x11); // still the program's data cycle
  check("upper-lane write leaves the command", rd(0x2000), 0x11);
  check("past the part reads ff", rd(0x200000), 0xff);
  done();

  // Two parts (the ES45): each has its own command state.
  fl = fresh(2);
  cmd(0x200000, 0x90);
  check("second part autoselect", rd(0x200001), 0xad);
  check("first part still reads its array", rd(1), 0xff);
  program(0x1, 0x42);
  check("first part programs while the second is in autoselect", rd(1), 0x42);
  wr(0x200000, 0xf0);
  erase_sector(0x210000);
  rd(0x210000);
  rd(0x210000);
  check("second part's erase leaves the first", rd(1), 0x42);
  done();
}

static void test_file() {
  fl = fresh(1);
  program(0x1ff000, 0x3c);
  fl->FlushIfDirty();
  struct stat st;
  check("flash file is the part's size",
        stat(rom_file, &st) == 0 && st.st_size == 2 * 1024 * 1024, 1);
  check("no temporary file left behind",
        stat((std::string(rom_file) + ".tmp").c_str(), &st) != 0, 1);
  done();
  theSROM = nullptr;
  fl = new CFlash(reinterpret_cast<CConfigurator *>(cfg_storage),
                  reinterpret_cast<CSystem *>(sys_storage), 1);
  check("the file is read back at the next start", rd(0x1ff000), 0x3c);
  done();
}

static void test_replay(const char *path) {
  const std::vector<RecordedAccess> rec = load_record(path, "flash");
  check("record has the console's accesses", rec.size() > 800, 1);
  fl = fresh(1);
  int bad = 0;
  for (const RecordedAccess &r : rec) {
    if (r.write) {
      fl->WriteMem(0, r.address, r.bits, r.value);
      continue;
    }
    const u64 got = fl->ReadMem(0, r.address, r.bits);
    checks++;
    if (got == r.value)
      continue;
    failures++;
    if (bad++ < 10)
      printf("FAIL replay line %d: read %" PRIx64 " gave %" PRIx64
             ", the console was told %" PRIx64 "\n",
             r.line, r.address, got, r.value);
  }
  done();
}

int main(int argc, char **argv) {
  test_autoselect();
  test_program();
  test_erase();
  test_bus();
  test_file();
  test_replay(argc > 1 ? argv[1] : TEST_DATA_DIR "/flash_srm_set.rec");
  remove(rom_file);
  printf("flash_test: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
