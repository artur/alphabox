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

/**
 * \file
 * `alphabox idetest`: the IDE controller driven through its registers.
 *
 * An ES40 with the ALi M1543C and its IDE function, a disk and a CD-ROM on
 * the primary channel and a very large sparse disk on the secondary one, and
 * no processor: the test writes and reads the task file, the control block
 * and the bus master registers the way a driver does (CSystem::WriteMem on
 * the Pchip's I/O space), with its PRD tables and buffers in guest memory
 * behind the Pchip's DMA window. Each check compares what the controller
 * answers with what ATA/ATAPI-6 and SFF-8038i say (the check names the
 * clause). A command that throws, hangs a channel or stops the machine
 * fails its check.
 *
 * usage: [IDETEST_ONLY=group,group] alphabox idetest <work directory> [-v]
 * The images are created in the work directory and removed at the end.
 **/

#include "AliM1543C_ide.hpp"
#include "Configurator.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include <chrono>
#include <cstdarg>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;
bool g_verbose = false;
std::string g_group;
std::string g_dir;

std::string path(const char *name) { return g_dir + "/" + name; }

void check(bool ok, const char *name, const char *fmt = nullptr, ...) {
  if (ok) {
    g_pass++;
    if (g_verbose)
      printf("  PASS %s: %s\n", g_group.c_str(), name);
    return;
  }
  g_fail++;
  printf("  FAIL %s: %s", g_group.c_str(), name);
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    printf(": ");
    vprintf(fmt, ap);
    va_end(ap);
  }
  printf("\n");
}

const u64 kIo = U64(0x801fc000000); // Pchip 0, PCI I/O space
const u32 kDiskSectors = 16384;     // 8 MB
const u32 kCdBlocks = 1000;

// Guest memory used by the DMA checks.
const u32 kPrd = 0x200000;
const u32 kSrc = 0x300000;
const u32 kDst = 0x400000;

bool make_image(const std::string &file, u64 bytes) {
  FILE *f = fopen(file.c_str(), "wb");
  if (!f)
    return false;
  bool ok = ftruncate(fileno(f), (off_t)bytes) == 0;
  fclose(f);
  return ok;
}

struct Rig {
  CSystem *sys = nullptr;

  void o8(u32 p, u32 v) { sys->WriteMem(kIo + p, 8, v, nullptr); }
  void o16(u32 p, u32 v) { sys->WriteMem(kIo + p, 16, v, nullptr); }
  void o32(u32 p, u32 v) { sys->WriteMem(kIo + p, 32, v, nullptr); }
  u32 i8(u32 p) { return (u32)sys->ReadMem(kIo + p, 8, nullptr) & 0xff; }
  u32 i16(u32 p) { return (u32)sys->ReadMem(kIo + p, 16, nullptr) & 0xffff; }
  u32 i32(u32 p) { return (u32)sys->ReadMem(kIo + p, 32, nullptr); }
  void mw8(u32 a, u8 v) { sys->WriteMem(a, 8, v, nullptr); }
  void mw32(u32 a, u32 v) { sys->WriteMem(a, 32, v, nullptr); }
  u8 mr8(u32 a) { return (u8)sys->ReadMem(a, 8, nullptr); }

  static u32 cmdbase(int ch) { return ch ? 0x170 : 0x1f0; }
  static u32 ctlport(int ch) { return ch ? 0x376 : 0x3f6; }
  static u32 bm(int ch) { return 0xf000 + 8 * ch; }

  u32 alt(int ch) { return i8(ctlport(ch)); }
  u32 status(int ch) { return i8(cmdbase(ch) + 7); }
  u32 error(int ch) { return i8(cmdbase(ch) + 1); }

  /// Wait for BSY to drop; false if the channel stays busy.
  bool wait_ready(int ch, int ms = 5000) {
    for (int t = 0; t < ms * 5; t++) {
      if (!(alt(ch) & 0x80))
        return true;
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return false;
  }

  void select(int ch, int drv, bool lba = true) {
    o8(cmdbase(ch) + 6, 0xa0 | (drv << 4) | (lba ? 0x40 : 0));
  }

  /// Write the task file and the command; the command is not waited for.
  void issue(int ch, int drv, u8 cmd, u32 count, u32 lba, u32 feat = 0) {
    const u32 b = cmdbase(ch);
    o8(b + 6, 0xe0 | (drv << 4) | ((lba >> 24) & 0xf));
    o8(b + 1, feat);
    o8(b + 2, count);
    o8(b + 3, lba & 0xff);
    o8(b + 4, (lba >> 8) & 0xff);
    o8(b + 5, (lba >> 16) & 0xff);
    o8(b + 7, cmd);
  }

  /// A CHS command on drive `drv`.
  void issue_chs(int ch, int drv, u8 cmd, u32 count, u32 cyl, u32 head,
                 u32 sector) {
    const u32 b = cmdbase(ch);
    o8(b + 6, 0xa0 | (drv << 4) | (head & 0xf));
    o8(b + 2, count);
    o8(b + 3, sector);
    o8(b + 4, cyl & 0xff);
    o8(b + 5, (cyl >> 8) & 0xff);
    o8(b + 7, cmd);
  }

  bool run(int ch, int drv, u8 cmd, u32 count, u32 lba, u32 feat = 0) {
    issue(ch, drv, cmd, count, lba, feat);
    return wait_ready(ch);
  }

  bool failed(int ch) { return status(ch) & 0x01; }

  std::vector<u16> read_words(int ch, int n) {
    std::vector<u16> v;
    for (int i = 0; i < n; i++)
      v.push_back((u16)i16(cmdbase(ch)));
    return v;
  }
  void write_words(int ch, int n, u16 pattern) {
    for (int i = 0; i < n; i++)
      o16(cmdbase(ch), pattern);
  }

  /// Read the whole 256-word response of IDENTIFY (DEVICE or PACKET).
  std::vector<u16> identify(int ch, int drv, bool packet) {
    run(ch, drv, packet ? 0xa1 : 0xec, 0, 0);
    if (!(status(ch) & 0x08))
      return {};
    return read_words(ch, 256);
  }

  /// A PRD table of `entries` regions covering `total` bytes at `base`.
  void prd_table(u32 table, u32 base, u32 total, u32 entries, u8 eot_byte) {
    u32 each = (total / entries) & ~1u, off = 0;
    for (u32 e = 0; e < entries; e++) {
      u32 len = (e == entries - 1) ? total - off : each;
      mw32(table + 8 * e, base + off);
      mw32(table + 8 * e + 4, len & 0xffff);
      mw8(table + 8 * e + 7, e == entries - 1 ? eot_byte : 0);
      off += len;
    }
  }

  /// A bus master DMA command: PRD table pointer (written with its two
  /// reserved bits set), direction, the ATA command, Start, then wait for the
  /// interrupt. Returns the bus master status after Stop; `ok` is false when
  /// the channel never finished.
  u32 dma(int ch, int drv, bool to_memory, u32 count, u32 lba, u32 table,
          bool *ok) {
    o32(bm(ch) + 4, table | 3);
    o8(bm(ch) + 0, to_memory ? 0x08 : 0x00);
    o8(bm(ch) + 2, 0x06);
    issue(ch, drv, to_memory ? 0xc8 : 0xca, count, lba);
    o8(bm(ch) + 0, (to_memory ? 0x08 : 0x00) | 1);
    *ok = wait_ready(ch);
    for (int t = 0; t < 2000 && !(i8(bm(ch) + 2) & 4); t++)
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    u32 st = i8(bm(ch) + 2);
    o8(bm(ch) + 0, to_memory ? 0x08 : 0x00);
    o8(bm(ch) + 2, 0x06);
    status(ch); // acknowledge INTRQ
    return st;
  }
};

// ---------------------------------------------------------------------------
//  Checks
// ---------------------------------------------------------------------------

// ATA/ATAPI-6 8.11, pp.105-106; 9.12, pp.365-366.
void t_diagnostic(Rig &r) {
  g_group = "execute-device-diagnostic";
  r.select(0, 1);
  r.issue(0, 1, 0x90, 0, 0); // whichever device is selected, both run it
  bool ok = r.wait_ready(0);
  check(ok, "the command completes");
  r.select(0, 0);
  check(r.error(0) == 0x01, "device 0 reports 01h", "error %02x", r.error(0));
  check(r.i8(0x1f2) == 1 && r.i8(0x1f3) == 1 && r.i8(0x1f4) == 0 &&
            r.i8(0x1f5) == 0,
        "device 0 leaves the ATA signature");
  check((r.status(0) & 0xd1) == 0x50, "an ATA device sets DRDY, no ERR",
        "status %02x", r.status(0));
  r.select(0, 1);
  check(r.error(0) == 0x01, "device 1 reports 01h", "error %02x", r.error(0));
  check(r.i8(0x1f4) == 0x14 && r.i8(0x1f5) == 0xeb,
        "the PACKET device leaves its signature");
  check(!(r.status(0) & 0x40) && !(r.status(0) & 0x01),
        "a PACKET device does not set DRDY");
  r.select(0, 0);
}

// ATA/ATAPI-6 8.34.6 (p.200): ABRT "if this command is not supported".
void t_unknown(Rig &r) {
  g_group = "unknown-command";
  bool ok = r.run(0, 0, 0xb0, 0, 0); // SMART, not implemented
  check(ok, "the command ends");
  check(r.failed(0) && (r.error(0) & 0x04), "ERR with ABRT", "st %02x err %02x",
        r.status(0), r.error(0));
  ok = r.run(0, 0, 0xec, 0, 0);
  check(ok && (r.status(0) & 0x08), "the channel still answers IDENTIFY");
  r.read_words(0, 256);
  r.wait_ready(0);
  ok = r.run(1, 0, 0x25, 1, 0); // READ DMA EXT: no 48-bit feature set
  check(ok && r.failed(1) && (r.error(1) & 0x04),
        "READ DMA EXT on the other channel is aborted too");
  r.status(1);
}

// ATA/ATAPI-6 8.34.6 (p.200): IDNF for an address outside the range.
void t_range(Rig &r) {
  g_group = "range";
  r.run(0, 0, 0x30, 1, 10);
  r.write_words(0, 256, 0xaaaa);
  r.wait_ready(0);
  r.run(0, 0, 0x20, 1, 10);
  r.read_words(0, 256);
  r.wait_ready(0);
  // The image's file position is now after sector 10. A write at the first
  // sector past the end must not land there.
  r.run(0, 0, 0x30, 1, kDiskSectors);
  r.write_words(0, 256, 0xbbbb);
  r.wait_ready(0);
  check(r.failed(0) && (r.error(0) & 0x10), "WRITE SECTORS past the end: IDNF",
        "st %02x err %02x", r.status(0), r.error(0));
  r.run(0, 0, 0x20, 1, 11);
  auto w = r.read_words(0, 256);
  r.wait_ready(0);
  bool zero = true;
  for (u16 x : w)
    zero &= x == 0;
  check(zero, "the sector after the last read is unchanged");

  r.run(0, 0, 0x20, 1, kDiskSectors);
  check(r.failed(0) && (r.error(0) & 0x10), "READ SECTORS past the end: IDNF",
        "st %02x err %02x", r.status(0), r.error(0));
  r.run(0, 0, 0x20, 1, kDiskSectors - 1);
  check(!r.failed(0) && (r.status(0) & 0x08), "the last sector reads");
  r.read_words(0, 256);
  r.wait_ready(0);

  r.run(0, 0, 0x20, 2, kDiskSectors - 1);
  r.read_words(0, 256);
  r.wait_ready(0);
  check(r.failed(0) && (r.error(0) & 0x10), "a read straddling the end: IDNF",
        "st %02x err %02x", r.status(0), r.error(0));

  r.run(0, 0, 0x40, 4, kDiskSectors - 2); // READ VERIFY
  check(r.failed(0) && (r.error(0) & 0x10), "READ VERIFY past the end: IDNF",
        "st %02x err %02x", r.status(0), r.error(0));

  // CHS addresses the geometry does not have (8.34.6, p.200).
  r.issue_chs(0, 0, 0x20, 1, 0, 0, 0);
  bool ok = r.wait_ready(0);
  check(ok && r.failed(0) && (r.error(0) & 0x10), "CHS sector 0: IDNF",
        "st %02x err %02x", r.status(0), r.error(0));
  r.issue_chs(0, 0, 0x30, 1, 0, 0, 200);
  ok = r.wait_ready(0);
  r.write_words(0, 256, 0xcccc);
  r.wait_ready(0);
  check(ok && r.failed(0) && (r.error(0) & 0x10), "CHS sector 200: IDNF",
        "st %02x err %02x", r.status(0), r.error(0));
  r.issue_chs(0, 0, 0x20, 1, 0, 0, 1);
  ok = r.wait_ready(0);
  check(ok && !r.failed(0), "CHS 0/0/1 reads");
  r.read_words(0, 256);
  r.wait_ready(0);
}

// ATA/ATAPI-6 8.49 (pp.243-244), 8.30.
void t_multiple(Rig &r) {
  g_group = "multiple";
  r.run(0, 0, 0xc6, 0, 0);
  check(!r.failed(0), "SET MULTIPLE MODE 0 is accepted (8.49.4)");
  bool ok = r.run(0, 0, 0xc4, 4, 0);
  check(ok && r.failed(0) && (r.error(0) & 0x04),
        "READ MULTIPLE is then aborted (8.49.4)", "st %02x err %02x",
        r.status(0), r.error(0));
  ok = r.run(0, 0, 0xc5, 4, 0);
  check(ok && r.failed(0) && (r.error(0) & 0x04),
        "WRITE MULTIPLE is then aborted (8.49.4)");
  r.run(0, 0, 0xc6, 129, 0);
  check(r.failed(0) && (r.error(0) & 0x04),
        "SET MULTIPLE MODE above word 47 is aborted (8.49.6)");
  r.run(0, 0, 0xc6, 3, 0);
  check(!r.failed(0), "SET MULTIPLE MODE 3 is accepted");
  r.run(0, 0, 0xc4, 6, 0);
  bool drq = r.status(0) & 0x08;
  auto a = r.read_words(0, 768);
  r.wait_ready(0);
  check(drq && (r.status(0) & 0x08), "READ MULTIPLE: a second block follows");
  auto b = r.read_words(0, 768);
  r.wait_ready(0);
  check(!(r.status(0) & 0x08) && !r.failed(0), "READ MULTIPLE ends cleanly");
  r.run(0, 0, 0xc6, 1, 0);
  r.run(0, 0, 0xc4, 2, kDiskSectors - 1);
  r.read_words(0, 256);
  r.wait_ready(0);
  check(r.failed(0) && (r.error(0) & 0x10), "READ MULTIPLE past the end: IDNF",
        "st %02x err %02x", r.status(0), r.error(0));
}

// ATA/ATAPI-6 8.34.5.2, 9.12 (p.366), 8.23.6 (p.163).
void t_packet(Rig &r) {
  g_group = "packet";
  bool ok = r.run(0, 1, 0x20, 1, 0);
  check(ok && r.failed(0) && (r.error(0) & 0x04),
        "READ SECTORS to a PACKET device is aborted", "st %02x err %02x",
        r.status(0), r.error(0));
  check(r.i8(0x1f4) == 0x14 && r.i8(0x1f5) == 0xeb,
        "and leaves the PACKET signature");
  ok = r.run(0, 1, 0xa0, 0, 0, 0x02); // OVL
  check(ok && r.failed(0) && (r.error(0) & 0x04),
        "PACKET with OVL is aborted, not waited on", "st %02x err %02x",
        r.status(0), r.error(0));

  // MODE SELECT(10) with a 65535-byte parameter list: data-out in blocks of at
  // most 65534 bytes (the controller's buffer is 128 KB).
  r.issue(0, 1, 0xa0, 0, 0, 0);
  r.wait_ready(0);
  ok = r.status(0) & 0x08;
  const u8 cdb[12] = {0x55, 0x10, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0};
  for (int i = 0; i < 6; i++)
    r.o16(0x1f0, cdb[2 * i] | (cdb[2 * i + 1] << 8));
  r.wait_ready(0);
  u32 first = r.i8(0x1f4) | (r.i8(0x1f5) << 8);
  check(ok && (r.status(0) & 0x08) && first == 65534,
        "MODE SELECT data-out: first block is 65534 bytes", "count %u", first);
  r.write_words(0, 32767, 0);
  r.wait_ready(0);
  u32 second = r.i8(0x1f4) | (r.i8(0x1f5) << 8);
  check((r.status(0) & 0x08) && second == 1,
        "and the rest follows as a second block", "count %u st %02x", second,
        r.status(0));
  r.write_words(0, 1, 0);
  ok = r.wait_ready(0);
  check(ok && !(r.status(0) & 0x08), "the command completes");
  r.status(0);
}

// ATA/ATAPI-6 8.12 (pp.106-108), 8.15 words 60-61, 83, 86 (pp.116-118), 6.2.1.
void t_identify(Rig &r) {
  g_group = "identify";
  auto w = r.identify(0, 0, false);
  check(w.size() == 256, "IDENTIFY DEVICE answers");
  if (w.size() == 256) {
    check(w[60] == (kDiskSectors & 0xffff) && w[61] == 0,
          "words 60-61: capacity");
    check(w[83] & 0x1000, "word 83 bit 12: FLUSH CACHE supported");
    check(w[86] & 0x1000, "word 86 bit 12: FLUSH CACHE enabled");
  }
  r.wait_ready(0);
  r.run(0, 0, 0xe7, 0, 0);
  check(!r.failed(0) && (r.status(0) & 0x40), "FLUSH CACHE completes");
  w = r.identify(0, 1, true);
  check(w.size() == 256 && !(w[83] & 0x1000),
        "a PACKET device does not claim FLUSH CACHE");
  r.wait_ready(0);
  w = r.identify(1, 0, false);
  check(w.size() == 256 && w[60] == 0xffff && w[61] == 0x0fff,
        "words 60-61 are clamped to 0FFFFFFFh (6.2.1, p.22)", "%04x %04x",
        w.size() == 256 ? w[60] : 0, w.size() == 256 ? w[61] : 0);
  r.wait_ready(1);
}

// SFF-8038i 2.0 (p.3), 2.3 (p.4), 1.1-1.2 (p.2).
void t_busmaster(Rig &r) {
  g_group = "busmaster";
  const u32 b = Rig::bm(0);
  r.o32(b + 4, kPrd | 3);
  r.o8(b + 0, 0x08);
  u32 st = r.i8(b + 2);
  check(r.i16(b + 0) == 0x08, "a 16-bit read of the command register");
  check(r.i16(b + 2) == st, "a 16-bit read of the status register", "%04x",
        r.i16(b + 2));
  check(r.i32(b + 4) == kPrd, "the PRD pointer reads back with bits 1:0 clear",
        "%08x", r.i32(b + 4));
  check(r.i16(b + 4) == (kPrd & 0xffff), "a 16-bit read of the pointer");
  r.o8(b + 0, 0);

  // 128 PRD entries of 256 bytes and 40 of 820: the walk has no cap of 34.
  const u32 total = 64 * 512;
  for (u32 i = 0; i < total; i += 8)
    r.sys->WriteMem(kSrc + i, 64, U64(0x0101010101010101) * ((i / 8) * 7 + 3),
                    nullptr);
  r.prd_table(kPrd, kSrc, total, 40, 0xc0); // EOT with reserved bits set
  bool ok;
  st = r.dma(0, 0, false, 64, 100, kPrd, &ok);
  check(ok, "WRITE DMA through 40 entries completes");
  check((st & 0x05) == 0x04, "Interrupt set, Active clear", "bm status %02x",
        st);
  for (u32 i = 0; i < total; i += 8)
    r.sys->WriteMem(kDst + i, 64, 0, nullptr);
  r.prd_table(kPrd, kDst, total, 128, 0x80);
  st = r.dma(0, 0, true, 64, 100, kPrd, &ok);
  check(ok, "READ DMA through 128 entries completes (no walk limit)");
  check((st & 0x07) == 0x04, "Interrupt set, no Error", "bm status %02x", st);
  bool same = true;
  for (u32 i = 0; i < total; i += 8)
    same &= r.sys->ReadMem(kSrc + i, 64, nullptr) ==
            r.sys->ReadMem(kDst + i, 64, nullptr);
  check(same, "the data comes back intact");

  // A table of 8192 two-byte entries without an end bit covers half of a
  // 64-sector transfer: the controller stops with Error set and raises no
  // interrupt (SFF-8038i 4.0, p.6), and the channel is not wedged.
  for (u32 e = 0; e < 8192; e++) {
    r.mw32(0x500000 + 8 * e, kDst);
    r.mw32(0x500000 + 8 * e + 4, 2);
  }
  r.o32(b + 4, 0x500000);
  r.o8(b + 0, 0x08);
  r.o8(b + 2, 0x06);
  r.issue(0, 0, 0xc8, 64, 100);
  r.o8(b + 0, 0x09);
  ok = r.wait_ready(0, 2000);
  st = r.i8(b + 2);
  check(ok, "a table without an end bit does not hang the channel");
  check((st & 0x06) == 0x02, "Error set, no Interrupt", "bm status %02x", st);
  r.o8(b + 0, 0x08);
  r.o8(b + 2, 0x06);
  r.status(0);
}

// ATA/ATAPI-6 9.2 (pp.315-317): SRST ends the command in progress; a
// command parked for the bus master must not wedge the channel.
void t_reset_in_dma(Rig &r) {
  g_group = "srst-in-dma";
  const u32 b = Rig::bm(0);
  r.prd_table(kPrd, kDst, 512, 1, 0x80);
  r.o32(b + 4, kPrd);
  r.o8(b + 0, 0x08);
  r.issue(0, 0, 0xc8, 1, 0); // READ DMA, and no Start
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  r.o8(r.ctlport(0), 0x04);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  r.o8(r.ctlport(0), 0x00);
  bool ok = r.wait_ready(0, 3000);
  check(ok, "SRST completes");
  r.mw32(kDst, 0x55555555);
  r.issue(0, 0, 0xec, 0, 0);
  ok = r.wait_ready(0, 3000);
  check(ok && (r.status(0) & 0x08), "IDENTIFY runs after the reset", "st %02x",
        r.status(0));
  r.read_words(0, 256);
  r.wait_ready(0);
  // The abandoned command must not run when the next one starts the engine.
  r.o8(b + 0, 0x09);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  r.o8(b + 0, 0x08);
  check(r.sys->ReadMem(kDst, 32, nullptr) == 0x55555555,
        "no data moves for the abandoned command");
  r.o8(b + 2, 0x06);
  bool ok2;
  r.prd_table(kPrd, kDst, 512, 1, 0x80);
  u32 st = r.dma(0, 0, true, 1, 100, kPrd, &ok2);
  check(ok2 && (st & 0x07) == 0x04, "the next DMA command works",
        "bm status %02x", st);

  // A new command also releases a parked one.
  r.o32(b + 4, kPrd);
  r.o8(b + 0, 0x08);
  r.issue(0, 0, 0xc8, 1, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  r.issue(0, 0, 0xec, 0, 0);
  ok = r.wait_ready(0, 3000);
  check(ok && (r.status(0) & 0x08), "a new command releases the parked one",
        "st %02x", r.status(0));
  r.read_words(0, 256);
  r.wait_ready(0);
  r.status(0);
}

} // namespace

int main_idetest(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    printf("usage: alphabox idetest <work directory> [-v]\n");
    return 2;
  }
  g_dir = argv[1];
  g_verbose = argc > 2 && strcmp(argv[2], "-v") == 0;
  mkdir(g_dir.c_str(), 0755);

  if (!make_image(path("ide0.img"), (u64)kDiskSectors * 512) ||
      !make_image(path("ide.iso"), (u64)kCdBlocks * 2048) ||
      !make_image(path("ide-big.img"), (u64)200 << 30)) {
    printf("idetest: cannot create the images in %s\n", g_dir.c_str());
    return 2;
  }

  const std::string systext =
      "sys0 = tsunami { memory.bits = 26; pci0.7 = ali { } pci0.15 = ali_ide { "
      "disk0.0 = file { file = \"" +
      path("ide0.img") + "\"; } disk0.1 = file { file = \"" + path("ide.iso") +
      "\"; cdrom = true; read_only = true; } disk1.0 = file { file = \"" +
      path("ide-big.img") + "\"; } } }";
  char *roottext = strdup(systext.c_str());
  new CConfigurator(nullptr, strdup("idetest"), strdup("idetest"), roottext,
                    strlen(roottext));
  Rig r;
  r.sys = theSystem;
  CIdeController *ide = nullptr;
  for (int i = 0; i < r.sys->component_count(); i++)
    if (CIdeController *c = dynamic_cast<CIdeController *>(r.sys->component(i)))
      ide = c;
  if (!ide) {
    printf("idetest: no IDE controller in the test machine\n");
    return 2;
  }
  // DMA window 0 of the Pchip: PCI 0-1 GB direct-mapped onto memory.
  r.sys->WriteMem(U64(0x80180000100), 64, 0x3ff00000, nullptr);
  r.sys->WriteMem(U64(0x80180000200), 64, 0, nullptr);
  r.sys->WriteMem(U64(0x80180000000), 64, 1, nullptr);
  // I/O space and bus mastering on, as the console leaves them.
  r.sys->WriteMem(U64(0x801fe000000) + (15 << 11) + 4, 32, 0x7, nullptr);
  ide->start_threads();

  printf("idetest: checks\n");
  // IDETEST_ONLY=name,name runs only the groups named (a group that stops
  // the machine would fail the ones after it).
  const char *only = getenv("IDETEST_ONLY");
  auto want = [&](const char *name) {
    return !only || strstr(only, name) != nullptr;
  };
  if (want("diagnostic"))
    t_diagnostic(r);
  if (want("unknown"))
    t_unknown(r);
  if (want("range"))
    t_range(r);
  if (want("multiple"))
    t_multiple(r);
  if (want("packet"))
    t_packet(r);
  if (want("identify"))
    t_identify(r);
  if (want("busmaster"))
    t_busmaster(r);
  if (want("srst"))
    t_reset_in_dma(r);

  printf("idetest: %d passed, %d failed\n", g_pass, g_fail);
  printf("%s\n", g_fail ? "FAIL" : "PASS");
  remove(path("ide0.img").c_str());
  remove(path("ide.iso").c_str());
  remove(path("ide-big.img").c_str());
  fflush(stdout);
  // The machine is not torn down: its components' destructors expect a
  // running system. The process ends here.
  _exit(g_fail ? 1 : 0);
}
