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
 * The CP microcode images (RadeonMicrocode.hpp).
 *
 * Every distinct image of the R100-family CP microcode the open sources
 * carried, and the later generations' for comparison: the DRI CVS /
 * libdrm history of radeon_cp.c and radeon_microcode.h (2001-2009),
 * Linux's drivers/char/drm (to 2.6.25) and drivers/gpu/drm/radeon (2.6.26
 * on), the BSD copies, and linux-firmware's radeon/*_cp.bin (one revision
 * each, 2009). docs/radeon-microcode.md lists the sources. There are ten
 * images, one per row below; the R100's never changed.
 *
 * What a row says about packets comes from the image's dispatch table:
 * entries 4.. of every image hold one byte per type-3 opcode from 0x10
 * (NOP), the micro-engine address of its handler; opcodes without a
 * handler of their own point at NOP's, which skips the body [inference:
 * the table lines up with the documented opcodes in every image, BITBLT
 * and BITBLT_MULTI share a handler, as do the three 3D_DRAW_*, and the
 * handlers write the registers their packets document]. So:
 *   - the R100 image (b86caa2a, what OpenVMS's DECwindows server loads)
 *     has no INDX_BUFFER (its entry is NOP's) and its table ends at 0x33:
 *     the _2 draws and 3D_CLEAR_HIZ are past it;
 *   - the R200 image has those, and no 3D_RNDR_GEN_INDX_PRIM (0x23);
 *   - the 2008 images add 0x2b and 0x2d; the R300 ones PRED_EXEC,
 *     WAIT_SEMAPHORE and more (R5xx Acceleration 6.2.1).
 * The micro-engine word is wider in each generation (the register field
 * grows: 11, 12, 13 bits); entries 2 and 3 of every image are jumps,
 * whose opcode bit shows the width. An image of another width cannot run
 * on the part's micro-engine as written; what the chip would do with it
 * is not known, so the model keeps parsing packets and says so.
 **/

#include "RadeonMicrocode.hpp"
#include "RadeonChip.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace radeon {

namespace {
// R100 family: Linux r100_cp_init_microcode (R100, RV100, RV200, RS100,
// RS200 load R100_cp; R200, RV250, RV280, RS300 load R200_cp; ...).
const MicrocodeImage kImages[] = {
    {0xb86caa2a, 0x92b7f8d8, "R100_cp",
     "R100 (R100, RV100, RV200, RS100, RS200)",
     "ATI; DRI CVS ati-5-0-0 merge 2001-01-05 (radeon_cp_microcode), "
     "unchanged in libdrm, Linux, the BSDs and linux-firmware "
     "radeon/R100_cp.bin",
     12, 0x33, 0x000000079f787f3full},
    {0x7f059413, 0x8297f6b1, "R200_cp (2002)",
     "R200 (R200, RV250, RV280, RS300)",
     "ATI; DRI CVS r200-0-1-branch merge 2002-08-26, replaced 2008-03-19", 12,
     0x37, 0x000000ff97507f3full},
    {0xf71981e4, 0xf8f7c7c3, "R200_cp", "R200 (R200, RV250, RV280, RS300)",
     "AMD production microcode, libdrm 2008-03-19; linux-firmware "
     "radeon/R200_cp.bin",
     12, 0x37, 0x000000ffbf507f3full},
    {0x236491cd, 0xf483f895, "R300_cp (2004)",
     "R300 (R300, R350, RV350, RV380, RS400, RS480)",
     "DRI CVS 2004-10-23 (r300 microcode patch), replaced 2008-03-19", 13, 0x3b,
     0x000007fc97507f3full},
    {0x1f7252ce, 0x63b4be38, "R300_cp",
     "R300 (R300, R350, RV350, RV380, RS400, RS480)",
     "AMD production microcode, libdrm 2008-03-19; linux-firmware "
     "radeon/R300_cp.bin",
     13, 0x3b, 0x000007fcbf457f3full},
    {0x8402ea16, 0xe73cc904, "R420_cp", "R420 (R420, R423, RV410)",
     "AMD production microcode, libdrm 2008-03-19; linux-firmware "
     "radeon/R420_cp.bin",
     13, 0x3b, 0x000007fcbf4f7f37ull},
    {0xb9f09374, 0xabea771a, "R520_cp",
     "R520 (R520, RV515, RV530, RV560, RV570, R580)",
     "AMD production microcode, libdrm 2008-03-19; linux-firmware "
     "radeon/R520_cp.bin",
     13, 0x3b, 0x000007fdbf4f7f37ull},
    {0xcaaae480, 0xe5f4096a, "RS600_cp", "RS600",
     "AMD production microcode, libdrm 2008-03-19; linux-firmware "
     "radeon/RS600_cp.bin",
     13, 0x3b, 0x000007fdbf457f37ull},
    {0x2c1c2fae, 0xff756426, "RS690_cp", "RS690 (RS690, RS740)",
     "AMD production microcode, libdrm 2008-03-19; linux-firmware "
     "radeon/RS690_cp.bin",
     13, 0x3b, 0x00000678bf457f37ull},
    // Not the R100 family: the Rage 128's CCE, the same RAM and ports.
    {0x70c3e844, 0xfb2e59a2, "R128_cce", "Rage 128 (not a Radeon)",
     "ATI; Linux drivers/char/drm/r128_cce.c (in 2.6.12 and 2.6.25 alike)", 11,
     0x33, 0x00000000707fffffull},
};

u32 crc32_entries(const u32 (*me)[2], u32 n) {
  u32 crc = 0xffffffffu;
  for (u32 i = 0; i < n; i++)
    for (int k = 0; k < 2; k++)
      for (int b = 0; b < 32; b += 8) {
        crc ^= (me[i][k] >> b) & 0xff;
        for (int j = 0; j < 8; j++)
          crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
      }
  return ~crc;
}

// The part's micro-engine: the width of the image its family loads.
u8 chip_reg_bits(const ChipInfo &chip) {
  for (const MicrocodeImage &m : kImages)
    if (strncmp(m.family, chip.cp_microcode, strlen(chip.cp_microcode)) == 0)
      return m.reg_bits;
  return 0;
}
} // namespace

const MicrocodeImage *find_microcode(u32 crc) {
  for (const MicrocodeImage &m : kImages)
    if (m.crc == crc)
      return &m;
  return nullptr;
}

MicrocodeId identify_microcode(const u32 (*me)[2], u32 entries,
                               const ChipInfo &chip) {
  MicrocodeId id;
  id.crc = crc32_entries(me, entries);
  id.known = find_microcode(id.crc);
  // Entries 2 and 3 are jumps in every image: the jump's opcode is bit
  // (reg_bits + 22) of the 40-bit word, i.e. DATAH 0x02 / 0x04 / 0x08.
  if (entries > 3 && me[2][0] == me[3][0] && !(me[2][1] >> 8) &&
      !(me[3][1] >> 8)) {
    for (u8 rb = 11; rb <= 13; rb++)
      if (me[2][0] == 1u << (rb - 10))
        id.reg_bits = rb;
  }
  if (id.known)
    id.reg_bits = id.known->reg_bits;
  const u8 mine = chip_reg_bits(chip);
  id.wrong_width = id.reg_bits && mine && id.reg_bits != mine;
  return id;
}

void log_microcode(const MicrocodeId &id, const ChipInfo &chip, u32 entries,
                   const char *devid) {
  printf("%s: CP microcode loaded, %u entries, CRC-32 %08x: ", devid, entries,
         id.crc);
  if (!id.known)
    printf("unknown microcode");
  else
    printf("%s, for the %s", id.known->name, id.known->family);
  if (id.wrong_width) {
    printf("\n%s: %%RADEON-W-UCODE, microcode for a micro-engine with a "
           "%u-bit register field; the %s's has %u: the chip cannot run it "
           "as written, and what it would do is not known. The CP parses "
           "packets as the %s microcode does.\n",
           devid, id.reg_bits, chip.name, chip_reg_bits(chip),
           chip.cp_microcode);
    return;
  }
  if (id.known && strncmp(id.known->family, chip.cp_microcode,
                          strlen(chip.cp_microcode)) != 0) {
    printf("\n%s: %%RADEON-W-UCODE, not the microcode a driver loads on the "
           "%s (%s_cp); it has the same word width, so the micro-engine "
           "runs it and the CP parses its packet set.\n",
           devid, chip.name, chip.cp_microcode);
    return;
  }
  if (!id.known)
    printf(" (accepted; the CP parses packets as the %s microcode does)",
           chip.cp_microcode);
  printf("\n");
}

int microcode_packet(const MicrocodeId &id, u8 op) {
  const u8 base = op & 0x7f;
  if (!id.known || id.wrong_width || base < 0x10 || base > id.known->table_last)
    return -1;
  return (id.known->handled >> (base - 0x10)) & 1;
}

void dump_microcode(const u32 (*me)[2], u32 entries, const char *devid) {
  const char *path = getenv("ALPHABOX_RADEON_ME_DUMP");
  if (!path || !*path)
    return;
  FILE *f = fopen(path, "wb");
  if (!f) {
    printf("%s: ALPHABOX_RADEON_ME_DUMP: cannot write %s\n", devid, path);
    return;
  }
  for (u32 i = 0; i < entries; i++)
    for (int k = 0; k < 2; k++) {
      const u32 v = me[i][k];
      const unsigned char b[4] = {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v)};
      fwrite(b, 1, 4, f);
    }
  fclose(f);
  printf("%s: CP microcode written to %s\n", devid, path);
}

void selftest_microcode(
    const ChipInfo &chip,
    const std::function<void(const std::string &, bool, const std::string &)>
        &report) {
  char d[160];
  // the table itself: CRCs unique, NOP handled, the R100 the part loads
  {
    bool ok = true;
    for (const MicrocodeImage &m : kImages)
      ok = ok && find_microcode(m.crc) == &m && (m.handled & 1) &&
           m.table_last >= 0x33 && m.reg_bits >= 11 && m.reg_bits <= 13;
    const MicrocodeImage *r100 = find_microcode(0xb86caa2a);
    ok = ok && r100 && strcmp(r100->name, "R100_cp") == 0 &&
         chip_reg_bits(chip) == 12;
    report("microcode: the known-image table, DECwindows' b86caa2a = R100_cp",
           ok, "");
  }
  // synthetic images, identified by width and CRC (no ATI bytes here)
  {
    static u32 img[256][2];
    auto fill = [&](u32 h) {
      for (u32 i = 0; i < 256; i++) {
        img[i][0] = 0;
        img[i][1] = 0x5a000000u | i;
      }
      img[2][0] = img[3][0] = h;
      img[2][1] = 0x40;
      img[3][1] = 0x44;
    };
    fill(0x04); // R100-width jumps
    const MicrocodeId a = identify_microcode(img, 256, chip);
    fill(0x08); // R300-width
    const MicrocodeId b = identify_microcode(img, 256, chip);
    fill(0x02); // Rage 128-width
    const MicrocodeId c = identify_microcode(img, 256, chip);
    img[2][1] = 0x12345678; // not jumps
    const MicrocodeId e = identify_microcode(img, 256, chip);
    const bool ok =
        !a.known && a.reg_bits == 12 && !a.wrong_width && !b.known &&
        b.reg_bits == 13 && b.wrong_width && !c.known && c.reg_bits == 11 &&
        c.wrong_width && !e.known && e.reg_bits == 0 && !e.wrong_width &&
        microcode_packet(a, 0x91) == -1 && microcode_packet(b, 0x34) == -1;
    snprintf(d, sizeof(d), "(widths %u %u %u %u, wrong %d %d %d %d)",
             a.reg_bits, b.reg_bits, c.reg_bits, e.reg_bits, a.wrong_width,
             b.wrong_width, c.wrong_width, e.wrong_width);
    report("microcode: unknown images, their word width", ok, ok ? "" : d);
  }
  // the packet sets of the R100 and R200 images
  {
    MicrocodeId r100, r200;
    r100.known = find_microcode(0xb86caa2a);
    r200.known = find_microcode(0xf71981e4);
    r100.reg_bits = r200.reg_bits = 12;
    struct {
      const MicrocodeId *id;
      u8 op;
      int want;
    } const t[] = {
        {&r100, 0x10, 1},  {&r100, 0x91, 1},  {&r100, 0x9b, 1},
        {&r100, 0x9c, 1},  {&r100, 0x23, 1},  {&r100, 0x28, 1},
        {&r100, 0x2c, 1},  {&r100, 0x32, 1},  {&r100, 0x33, 0},
        {&r100, 0x16, 0},  {&r100, 0x34, -1}, {&r100, 0x37, -1},
        {&r100, 0x05, -1}, {&r200, 0x34, 1},  {&r200, 0x37, 1},
        {&r200, 0x33, 1},  {&r200, 0x23, 0},  {&r200, 0x91, 1},
    };
    bool ok = r100.known && r200.known;
    d[0] = 0;
    for (const auto &x : t)
      if (ok && microcode_packet(*x.id, x.op) != x.want) {
        snprintf(d, sizeof(d), "(%s op %02x: %d, expected %d)",
                 x.id->known->name, x.op, microcode_packet(*x.id, x.op),
                 x.want);
        ok = false;
      }
    report("microcode: packet sets (R100: no INDX_BUFFER, _2; R200: no 0x23)",
           ok, d);
  }
}

} // namespace radeon
