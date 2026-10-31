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
 * The native EV8's direct start (DirectBoot.hpp). The boot block's place and
 * format are firmware/ev8/layout.h's (BOOT_BLOCK, BB_*), repeated here: the
 * console's image carries the block's magic and version, the emulator fills
 * the rest [decision, S12b]. An image without the magic (a test's own) is
 * left as it is.
 **/
#include "StdAfx.hpp"

#include "AlphaCPU.hpp"
#include "DirectBoot.hpp"
#include "Marvel.hpp"
#include "System.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

constexpr u64 kBlock = 0x1fe000;                // layout.h BOOT_BLOCK
constexpr u64 kMagic = U64(0x00544f4f42385645); // "EV8BOOT\0"
constexpr u64 kVersion = 1;
constexpr u64 kFirmwareTop = 0x200000; // the console's cluster (N28)
// Linux's zero page, KERNEL_START_PHYS + 0xA000 (setup.h), which the
// console writes: no segment may cover it.
constexpr u64 kZeroPage = 0x100a000;
constexpr size_t kCmdline = 256; // COMMAND_LINE_SIZE (uapi setup.h)
constexpr u16 kEmAlpha = 0x9026;

// The block's quadwords (layout.h BB_*).
enum : u64 {
  BB_MAGIC = 0x00,
  BB_VERSION = 0x08,
  BB_FILLED = 0x10,
  BB_CYCLE_HZ = 0x18,
  BB_CHIPS = 0x20,
  BB_TPUS = 0x28,
  BB_MEM_PER_PID = 0x30,
  BB_IO7_MASK = 0x38,
  BB_ENTRY = 0x40,
  BB_KERNEL_LOW = 0x48,
  BB_KERNEL_HIGH = 0x50,
  BB_INITRD_PA = 0x58,
  BB_INITRD_SIZE = 0x60,
  BB_FLAGS = 0x68, // <0> auto_action boot
  BB_SYSNAME = 0x70,
  BB_CMDLINE = 0x100,
};

u64 get64(const u8 *p) {
  u64 v = 0;
  for (int i = 7; i >= 0; i--)
    v = v << 8 | p[i];
  return v;
}
void put64(u8 *p, u64 v) {
  for (int i = 0; i < 8; i++, v >>= 8)
    p[i] = (u8)v;
}
u32 get16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }

bool read_file(const std::string &path, std::vector<u8> &out) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    return false;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? (size_t)n : 0);
  const bool ok = n >= 0 && fread(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}

/// A program header's load address as a physical address: Linux's vmlinux
/// gives KSEG addresses (vmlinux.lds places no AT(), so p_paddr is p_vaddr,
/// 0xfffffc0001010000 [inference]), which SPE43 maps to PA<40:0> with
/// PA<47:41> copies of VA<40> (IDS pp. 688-689): memory needs VA<40> clear.
/// A plain physical address is taken as it is. ~0 for anything else.
u64 load_pa(u64 a) {
  if ((a >> 41) == U64(0x7ffffe))
    return (a >> 40 & 1) ? ~U64(0) : a & ((U64(1) << 41) - 1);
  return a < (U64(1) << 48) ? a : ~U64(0);
}

struct loaded {
  u64 entry = 0, low = ~U64(0), high = 0;
};

/// The kernel's PT_LOAD segments into PID 0's memory, above the console's
/// cluster; the entry as the KSEG address of its physical address.
loaded load_elf(CSystem *sys, const std::string &path, u64 mem) {
  std::vector<u8> f;
  if (!read_file(path, f))
    FAILURE_1(Configuration, "kernel = \"%s\": cannot read it", path.c_str());
  const char *what = nullptr;
  if (f.size() < 64 || memcmp(f.data(), "\177ELF", 4) != 0)
    what = "not an ELF file";
  else if (f[4] != 2 || f[5] != 1)
    what = "not a 64-bit little-endian ELF";
  else if (get16(&f[18]) != kEmAlpha)
    what = "not an Alpha ELF (e_machine 0x9026)";
  if (what)
    FAILURE_2(Configuration, "kernel = \"%s\": %s", path.c_str(), what);
  const u64 phoff = get64(&f[32]), ent = get64(&f[24]), size = f.size();
  const u32 phsize = get16(&f[54]), phnum = get16(&f[56]);
  loaded k;
  // Every range below is checked as "start > limit - length", the length
  // checked against the limit first, so that no sum of the file's values
  // can wrap past 2^64 (i x phsize is below 2^32).
  for (u32 i = 0; i < phnum; i++) {
    if (phsize < 56 || phoff > size || size - phoff < 56 ||
        (u64)i * phsize > size - phoff - 56)
      FAILURE_1(Configuration,
                "kernel = \"%s\": the program headers are not in the file",
                path.c_str());
    const u8 *p = f.data() + phoff + (u64)i * phsize;
    const u64 off = get64(p + 8), va = get64(p + 16), filesz = get64(p + 32),
              memsz = get64(p + 40);
    if ((p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24) != 1 || !memsz)
      continue; // PT_LOAD only
    const u64 paddr = get64(p + 24), pa = load_pa(paddr);
    if (filesz > memsz || filesz > size || off > size - filesz)
      FAILURE_1(Configuration,
                "kernel = \"%s\": a segment's bytes are not in the file",
                path.c_str());
    if (pa == ~U64(0) || pa < kFirmwareTop || memsz > mem || pa > mem - memsz ||
        (pa < kZeroPage + 0x2000 &&
         (pa >= kZeroPage || memsz > kZeroPage - pa)))
      FAILURE_3(Configuration,
                "kernel = \"%s\": a segment at %" PRIx64 " (%" PRIx64
                " bytes) is not in PID 0's memory between the console's 2 MB "
                "and its end, clear of Linux's zero page",
                path.c_str(), paddr, memsz);
    u8 *to = (u8 *)sys->PtrToMem(pa);
    memcpy(to, f.data() + off, filesz);
    memset(to + filesz, 0, memsz - filesz);
    k.low = std::min(k.low, pa);
    k.high = std::max(k.high, pa + memsz);
    // e_entry is virtual (vmlinux), or physical where the linker gave the
    // segments physical addresses of their own (nada's -B v=p).
    if (ent >= va && ent - va < memsz)
      k.entry = U64(0xfffffc0000000000) | (pa + (ent - va));
    else if (paddr != va && ent >= paddr && ent - paddr < memsz)
      k.entry = U64(0xfffffc0000000000) | (pa + (ent - paddr));
  }
  if (!k.entry)
    FAILURE_1(Configuration, "kernel = \"%s\": its entry is in no segment",
              path.c_str());
  return k;
}

} // namespace

void direct_boot_load(CSystem *sys, const CMarvel &mv,
                      const direct_boot_config &boot) {
  u8 *b = (u8 *)sys->PtrToMem(kBlock);
  if (get64(b + BB_MAGIC) != kMagic || get64(b + BB_VERSION) != kVersion) {
    if (!boot.kernel.empty() || !boot.initrd.empty() || !boot.cmdline.empty())
      FAILURE(Configuration,
              "kernel, initrd, cmdline: the boot image has no console to take "
              "them (firmware/ev8/build.py writes one)");
    return;
  }
  const u64 mem = mv.memory_per_pid();
  const int chips = sys->processor_chips();
  u64 io7s = 0;
  for (int pid = 0; pid < chips && pid < 64; pid++)
    if (mv.io7((u32)pid))
      io7s |= U64(1) << pid;
  put64(b + BB_CYCLE_HZ, sys->get_cpu(0)->get_speed());
  put64(b + BB_CHIPS, (u64)chips);
  put64(b + BB_TPUS, (u64)mv.topology().tpus());
  put64(b + BB_MEM_PER_PID, mem);
  put64(b + BB_IO7_MASK, io7s);
  put64(b + BB_FLAGS, boot.auto_boot ? 1 : 0);
  memset(b + BB_SYSNAME, 0, BB_CMDLINE - BB_SYSNAME);
  strncpy((char *)b + BB_SYSNAME, sys->platform().description,
          BB_CMDLINE - BB_SYSNAME - 1);
  if (boot.cmdline.size() >= kCmdline)
    FAILURE_1(Configuration, "cmdline: %d characters at most",
              (int)kCmdline - 1);
  memset(b + BB_CMDLINE, 0, kCmdline);
  memcpy(b + BB_CMDLINE, boot.cmdline.data(), boot.cmdline.size());
  loaded k;
  k.low = k.high = 0;
  if (!boot.kernel.empty())
    k = load_elf(sys, boot.kernel, mem);
  else if (!boot.initrd.empty())
    FAILURE(Configuration, "initrd: there is no kernel to take it");
  put64(b + BB_ENTRY, k.entry);
  put64(b + BB_KERNEL_LOW, k.low);
  put64(b + BB_KERNEL_HIGH, k.high);
  // The initrd at the top of PID 0's memory, page aligned [decision]: clear
  // of the kernel and of everything Linux allocates after its end.
  u64 rd = 0, rd_size = 0;
  if (!boot.initrd.empty()) {
    std::vector<u8> r;
    if (!read_file(boot.initrd, r))
      FAILURE_1(Configuration, "initrd = \"%s\": cannot read it",
                boot.initrd.c_str());
    rd_size = r.size();
    if (!rd_size || rd_size > mem || ((mem - rd_size) & ~U64(0x1fff)) < k.high)
      FAILURE_1(Configuration,
                "initrd = \"%s\": no room between the kernel and the end of "
                "PID 0's memory",
                boot.initrd.c_str());
    rd = (mem - rd_size) & ~U64(0x1fff);
    memcpy(sys->PtrToMem(rd), r.data(), r.size());
  }
  put64(b + BB_INITRD_PA, rd);
  put64(b + BB_INITRD_SIZE, rd_size);
  put64(b + BB_FILLED, 1);
  sys->code_pages()->note_write_all();
  if (!boot.kernel.empty())
    printf("%%MVL-I-KERNEL: %s at %" PRIx64 "-%" PRIx64 ", entry %" PRIx64
           "%s%s\n",
           boot.kernel.c_str(), k.low, k.high, k.entry, rd ? ", initrd " : "",
           rd ? boot.initrd.c_str() : "");
}
