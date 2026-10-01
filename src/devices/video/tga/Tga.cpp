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
 * DECchip 21030 (TGA): the models, construction, the PCI header, the
 * memory-space decode (core space copies, alternate ROM, registers, frame
 * buffer), the register file, the device lifecycle and the state file.
 **/

#include "Tga.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "VGA.hpp"

#include <cstdarg>

using namespace tga;

CTga *theTGA = nullptr;

/* The boards. The ZLXp-E family carries the 21030 (Linux tgafb names them
 * by the alternate-ROM option type; NetBSD's tga_conf.c lists the frame
 * buffer options of chapter 8). The E1 is the 8-plane board, frame buffer
 * option T8-02: 2 MB of 256K x n VRAM, a Bt485. The E2 and E3 (24 planes,
 * a Bt463, T32-08 and T32-88) are not modelled yet.
 *
 * The PowerStorm 3D30 and 4D20 carry the TGA2 (PCI 1011:000d), the 21030's
 * successor: the same core registers and graphics modes, with the RAMDAC
 * and an ICS9110 clock synthesiser in an external-device window where the
 * 21030 has its alternate ROM. tga2.sys names the RAMDACs: a Bt485 on the
 * 3D30, an IBM RGB561 on the 4D20; the 3D30 has 2 MB of VRAM (8 planes),
 * the 4D20 16 MB (32-bpp pixels). Revision 0x22 is the third pass, the
 * last NetBSD's tga(4) knows. */
static const tga_model_config tga_models[] = {
    {"e1", "ZLXp-E1 (8-plane, Bt485)", 0x0004, 0x02, 0, false, 2u << 20,
     TgaRamdac::Bt485},
    {"3d30", "PowerStorm 3D30 (TGA2, 8-plane, Bt485)", 0x000d, 0x22, 0, false,
     2u << 20, TgaRamdac::Bt485},
    {"4d20", "PowerStorm 4D20 (TGA2, 32-plane, IBM RGB561)", 0x000d, 0x22, 0,
     true, 16u << 20, TgaRamdac::Rgb561},
};

const tga_model_config *tga_model_by_name(const char *name) {
  for (const auto &m : tga_models)
    if (strcmp(m.name, name) == 0)
      return &m;
  return nullptr;
}

CTga::CTga(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev,
           const tga_model_config &model)
    : CPCIDevice(cfg, c, pcibus, pcidev), m_model(model) {
  if (!theTGA)
    theTGA = this;
  memset(r, 0, sizeof(r));
  memset(&e, 0, sizeof(e));
  memset(&dac, 0, sizeof(dac));
}

CTga::~CTga() {
  stop_threads();
  if (theTGA == this)
    theTGA = nullptr;
}

void CTga::init() {
  // PCI header (Table 4-1, 4.2): Digital 21030, a display controller of
  // subclass "other" (not VGA compatible), step B. The command register
  // powers up as 00a0: bus stepping (hard-wired) and VGA palette snoop;
  // memory, bus master and snoop are writable, I/O space never answers.
  // Status: medium DEVSEL, fast back-to-back capable. BAR0 is the 128 MB
  // prefetchable memory space; the expansion ROM is 256 KB; INTA.
  // Device-specific: the VGA redirect register PVRR, 81000002 at reset.
  u32 cfg_data[64] = {};
  u32 cfg_mask[64] = {};
  cfg_data[0x00 >> 2] = (u32(m_model.device_id) << 16) | PCI_VENDOR_DEC;
  cfg_data[0x04 >> 2] = 0x028000a0;
  cfg_data[0x08 >> 2] = 0x03800000 | m_model.revision;
  cfg_data[0x10 >> 2] = 0x00000008;
  cfg_data[0x3c >> 2] = 0x00000100;
  cfg_data[0x40 >> 2] = 0x81000002;
  cfg_mask[0x04 >> 2] = 0x00000026;
  cfg_mask[0x0c >> 2] = 0x0000ff00;
  cfg_mask[0x10 >> 2] = ~(SPACE_BYTES - 1);
  cfg_mask[0x30 >> 2] = ~(ROM_BYTES - 1) | PCI_ROM_ADDRESS_ENABLE;
  cfg_mask[0x3c >> 2] = 0x000000ff;
  cfg_mask[0x40 >> 2] = 0x8f0000ff;
  add_function(0, cfg_data, cfg_mask);
  ResetPCI();

  m_vram.assign(m_model.vram_bytes, 0);
  m_vram_mask = m_model.vram_bytes - 1;

  // The EEPROM. An image is optional: what real boards keep there is
  // Alpha console code neither console here asks for (see docs).
  m_rom.assign(ROM_BYTES, 0xff);
  const char *rom = myCfg->get_text_value("rom", nullptr);
  if (rom && *rom) {
    FILE *f = fopen(rom, "rb");
    if (!f)
      FAILURE_1(FileNotFound, "tga rom file %s not found", rom);
    size_t n = fread(m_rom.data(), 1, m_rom.size(), f);
    fclose(f);
    printf("%s: option ROM %s, %zu bytes\n", devid_string, rom, n);
  }

  if (const char *t = getenv("ALPHABOX_TRACE_TGA")) {
    if (strncmp(t, "file:", 5) == 0)
      m_trace_file = t + 5;
    else if (strcmp(t, "first") == 0)
      m_trace_first = true;
    else
      m_trace = t[0] && t[0] != '0';
  }

  ramdac_reset();
  rgb561_reset();
  m_frame.clear();
  m_last_frame = std::chrono::steady_clock::now();
  printf("%s: Digital %s, %u KB\n", devid_string, m_model.part,
         m_model.vram_bytes / 1024);
}

/**
 * Chip reset (4.1): most registers clear; the exceptions are the pixel
 * mask (ffffffff), the raster op (copy) and the data register (ffffffff).
 * The plane mask and block colours live in the VRAMs and are undefined;
 * all ones and zeros here.
 **/
void CTga::ResetPCI() {
  CPCIDevice::ResetPCI();
  memset(r, 0, sizeof(r));
  memset(&e, 0, sizeof(e));
  r[GOPR] = 0x00000003;
  r[GDAR] = 0xffffffff;
  r[GPMR] = 0xffffffff;
  e.gpxr = 0xffffffff;
  {
    std::lock_guard<std::mutex> lock(m_irq_lock);
    update_irq();
  }
  m_generation++;
}

void CTga::trace(const char *fmt, ...) {
  if (!m_trace)
    return;
  va_list ap;
  va_start(ap, fmt);
  printf("%s: ", devid_string);
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
}

void CTga::first_use(const char *fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (m_unimplemented_seen.insert(std::string("first:") + buf).second)
    printf("%s: first use: %s\n", devid_string, buf);
}

void CTga::unimplemented(const std::string &what) {
  if (m_unimplemented_seen.insert(what).second)
    printf("%s: not implemented: %s\n", devid_string, what.c_str());
}

u32 CTga::config_read_custom(int func, u32 address, int dsize, u32 data) {
  trace("config read  %02x/%d = %08x", address, dsize, data);
  return data;
}

void CTga::config_write_custom(int func, u32 address, int dsize, u32 old_data,
                               u32 new_data, u32 data) {
  trace("config write %02x/%d = %08x (now %08x)", address, dsize, data,
        new_data);
}

/**
 * The core space size, from GDER's address mask (4.4.28, Table 2-1): the
 * PCI address bits <24:22> that are ignored, so that the 128 MB memory
 * space holds 32, 16, 8 or 4 copies of it.
 **/
u32 CTga::core_bytes() const {
  // The TGA2's frame buffer is the upper half of a core twice its size:
  // tga2.sys maps it at the memory size from the start of the BAR (4 MB
  // cores on the 3D30, 32 MB on the 4D20, as NetBSD's tga_conf.c has it).
  if (m_model.tga2())
    return std::max<u32>(4u << 20, 2 * m_model.vram_bytes);
  switch ((r[GDER] & GDER_ADDR_MASK) >> 2) {
  case 0:
    return 4u << 20;
  case 1:
    return 8u << 20;
  case 3:
    return 16u << 20;
  default: // 7 (unused codes reserved)
    return 32u << 20;
  }
}

u32 CTga::vram32(u32 a) const {
  a &= m_vram_mask & ~3u;
  u32 v;
  memcpy(&v, &m_vram[a], 4);
  return v;
}

void CTga::vram32_set(u32 a, u32 v) {
  a &= m_vram_mask & ~3u;
  memcpy(&m_vram[a], &v, 4);
}

/**
 * BAR0, the memory space, and BAR6, the expansion ROM. The registers take
 * Dwords only and ignore the byte enables (5.2.1); the frame buffer takes
 * any width, which simple mode honours as a byte mask.
 **/
u32 CTga::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  if (bar == 6) {
    // The expansion ROM space is byte readable (4.2.6).
    u32 v = 0;
    for (int i = 0; i < dsize / 8; i++)
      v |= u32(m_rom[(address + i) & (ROM_BYTES - 1)]) << (8 * i);
    trace("rom read  %05x/%d = %x", address, dsize, v);
    return v;
  }
  if (bar != 0)
    return 0;
  const u32 lane = address & 3;
  u32 v = space_read(address & ~3u);
  if (dsize < 32)
    v = (v >> (8 * lane)) & ((1u << dsize) - 1);
  return v;
}

void CTga::WriteMem_Bar(int func, int bar, u32 address, int dsize, u32 data) {
  if (bar != 0)
    return;
  const u32 lane = address & 3;
  u32 bytemask = 0xf;
  if (dsize < 32) {
    bytemask = ((1u << (dsize / 8)) - 1) << lane;
    data <<= 8 * lane;
  }
  space_write(address & ~3u, data, bytemask);
}

u32 CTga::space_read(u32 address) {
  const u32 core = core_bytes();
  const u32 off = address & (core - 1);
  u32 v;
  if (off < CORE_REGS && m_model.tga2()) {
    v = extdev_read(off);
  } else if (off < CORE_REGS) {
    v = altrom_read(off);
    trace("altrom read %06x = %08x", off, v);
  } else if (off < CORE_REGS + REGS_BYTES && tga2_upper(off)) {
    v = 0; // the rectangle ports: write-only
    trace("reg read  %03x = %08x (rectangle port)", off & 0x3ff, v);
  } else if (off < CORE_REGS + REGS_BYTES) {
    const unsigned reg = (off >> 2) & (NUM_REGS - 1);
    v = reg_read(reg);
    if (reg != SCSR && reg != SISR && !(m_model.tga2() && reg == EPDR))
      trace("reg read  %03x = %08x", reg * 4, v);
  } else {
    v = vram32(off & (core / 2 - 1));
  }
  return v;
}

void CTga::space_write(u32 address, u32 data, u32 bytemask) {
  const u32 core = core_bytes();
  const u32 off = address & (core - 1);
  if (off < CORE_REGS && m_model.tga2()) {
    extdev_write(off, data);
  } else if (off < CORE_REGS) {
    // Writes to alternate ROM space reach the address and continue
    // registers (2.2.3.2): in its first 512 KB, even Dwords are GADR and
    // odd ones GCTR; every other write is GCTR.
    const bool gadr = off < (512u << 10) && (off & 4) == 0;
    trace("altrom write %06x = %08x (%s)", off, data, gadr ? "GADR" : "GCTR");
    reg_write(gadr ? GADR : GCTR, data);
  } else if (off < CORE_REGS + REGS_BYTES && tga2_upper(off)) {
    trace("reg write %03x = %08x (rectangle port)", off & 0x3ff, data);
    if (m_trace_first)
      first_use("register %03x written", (off & 0x300) == 0x200
                                             ? 0x200 // one line for the ports
                                             : off & 0x3ff);
    tga2_port_write(off & 0x3ff, data);
  } else if (off < CORE_REGS + REGS_BYTES) {
    const unsigned reg = (off >> 2) & (NUM_REGS - 1);
    trace("reg write %03x = %08x", reg * 4, data);
    if (m_trace_first)
      first_use("register %03x written", reg * 4);
    reg_write(reg, data);
  } else {
    // Frame buffer space: its upper half, the smaller cores' reserved
    // stretch below aliasing it (Figures 2-2, 2-3).
    fb_write(off & (core / 2 - 1), data, bytemask, false);
  }
}

/**
 * Alternate ROM space (2.2.3): one EEPROM byte per Dword, in its low byte;
 * the three upper bytes are hard-wired on Digital's options with the
 * option ID. Linux tgafb reads the frame buffer type from bits <15:12>
 * of the first Dword: 0 for an 8-plane board.
 **/
u32 CTga::altrom_read(u32 offset) {
  const u32 byte = m_rom[(offset >> 2) & (ROM_BYTES - 1)];
  return (m_model.option_type << 12) | byte;
}

/**
 * The TGA2's external-device window (first megabyte of core space): the
 * RAMDAC from 0x80000, its register select in address bits <11:8> and its
 * data in the low byte (tga2.sys and NetBSD put the window at 0x8e000);
 * the ICS9110 clock synthesiser's serial port from 0x60000, a data bit in
 * <0> and the load strobe in <1> as NetBSD's tga2_ics9110_wr shifts its 24
 * bits. Anything else in the window reads as zero.
 **/
u32 CTga::extdev_read(u32 offset) {
  u32 v = 0;
  if ((offset & ~(TGA2_EXT_WINDOW - 1)) == TGA2_EXT_RAMDAC) {
    const unsigned rs = (offset >> 8) & 0xf;
    if (m_model.ramdac == TgaRamdac::Rgb561)
      v = rgb561_read(rs & 3);
    else
      v = ramdac_read(rs);
  } else if (m_trace_first) {
    first_use("external-device read %06x", offset);
  }
  trace("extdev read  %06x = %08x", offset, v);
  return v;
}

void CTga::extdev_write(u32 offset, u32 data) {
  trace("extdev write %06x = %08x", offset, data);
  switch (offset & ~(TGA2_EXT_WINDOW - 1)) {
  case TGA2_EXT_RAMDAC: {
    const unsigned rs = (offset >> 8) & 0xf;
    if (m_model.ramdac == TgaRamdac::Rgb561)
      rgb561_write(rs & 3, u8(data));
    else
      ramdac_write(rs, u8(data));
    return;
  }
  case TGA2_EXT_CLOCK:
    // Kept for the trace; the screen is drawn at a fixed rate.
    e.icsbits = (e.icsbits >> 1) | ((data & 1) << 31);
    e.icscount++;
    if (data & 2) {
      trace("ICS9110 loaded (%u bits: %06x)", e.icscount, e.icsbits >> 8);
      e.icscount = 0;
    }
    return;
  default:
    if (m_trace_first)
      first_use("external-device write %06x", offset);
    return;
  }
}

u32 CTga::reg_read(unsigned reg) {
  switch (reg) {
  case GCBR0 + 0:
  case GCBR0 + 1:
  case GCBR0 + 2:
  case GCBR0 + 3:
  case GCBR0 + 4:
  case GCBR0 + 5:
  case GCBR0 + 6:
  case GCBR0 + 7: {
    // Direct reads of copy buffer entries 0-3 (Figure 4-24).
    const unsigned n = reg - GCBR0;
    return u32(e.copybuf[n >> 1] >> (32 * (n & 1)));
  }
  case GSNR0 + 0:
  case GSNR0 + 1:
  case GSNR0 + 2:
  case GSNR0 + 3:
  case GSNR0 + 4:
  case GSNR0 + 5:
  case GSNR0 + 6:
  case GSNR0 + 7: {
    // ... and entries 4-7 (Table 4-29).
    const unsigned n = reg - GSNR0;
    return u32(e.copybuf[4 + (n >> 1)] >> (32 * (n & 1)));
  }
  case GPXR_S:
  case GPXR_P:
    return e.gpxr;
  case GMOR:
    return (r[GMOR] & GMOR_WRITABLE) | (e.copy_drain ? GMOR_CD : 0) |
           (e.bres_new ? GMOR_BA : 0) | (e.addr_new ? GMOR_AA : 0) |
           (e.gpxr_persistent ? GMOR_GS : 0);
  case GREV:
    // Reserved in the 21030 manual; NetBSD's tga(4) reads the chip
    // revision in its low byte here (1-4 on a TGA, 0x2x on a TGA2) and
    // panics on anything else, so a real TGA answers with it. A TGA2
    // also reports its board: the monitor-ID straps in <19:16>, inverted
    // (NetBSD indexes its monitor table with ~GREV<19:16>; 0xf picks the
    // first, 1280x1024 at 72 Hz) and the frame buffer size in <22:21>
    // (tga2.sys: 0 16 MB, 1 8 MB, 2 4 MB, 3 2 MB).
    if (m_model.tga2()) {
      u32 size = 0;
      for (u32 mb = 16u << 20; mb > m_model.vram_bytes && size < 3; mb >>= 1)
        size++;
      return (pci_state.config_data[0][2] & 0xff) | (0xfu << 16) | (size << 21);
    }
    return pci_state.config_data[0][2] & 0xff;
  case GDER:
    // The TGA2's deep bit is a strap: the board's, whatever is written.
    if (m_model.tga2())
      return (r[GDER] & ~GDER_DEEP) | (m_model.deep ? GDER_DEEP : 0);
    return r[GDER];
  case GCTR:
    return 0; // Z address increments: no Z-buffered lines yet
  case SCSR:
    return 0; // never busy: every operation completes on its write
  case EPDR:
    // The TGA2 has no RAMDAC port here: tga2.sys's interrupt handler
    // reads the interrupt status from this address (and writes it back
    // to SISR to clear it).
    if (m_model.tga2()) {
      std::lock_guard<std::mutex> lock(m_irq_lock);
      return r[SISR];
    }
    return epdr_read();
  case SISR: {
    std::lock_guard<std::mutex> lock(m_irq_lock);
    return r[SISR];
  }
  case VVBR:
  case GPMR:
  case GSWR:
    // Write-only; reading them back is what NetBSD's TGAREGRWB does for
    // a barrier, and returning the last value harms nothing.
    return r[reg];
  default:
    return r[reg];
  }
}

void CTga::reg_write(unsigned reg, u32 v) {
  switch (reg) {
  case GCBR0 + 0:
  case GCBR0 + 2:
  case GCBR0 + 4:
  case GCBR0 + 6:
    // An even copy buffer register holds the low Dword of the next entry
    // until an odd one completes it (4.4.6).
    e.cbr_low = v;
    e.copy_drain = false;
    return;
  case GCBR0 + 1:
  case GCBR0 + 3:
  case GCBR0 + 5:
  case GCBR0 + 7:
    e.copybuf[e.cbr_fill & 7] = (u64(v) << 32) | e.cbr_low;
    e.cbr_fill++;
    e.copy_drain = false;
    return;
  case GPXR_S:
    e.gpxr = v;
    e.gpxr_persistent = false;
    return;
  case GPXR_P:
    e.gpxr = v;
    e.gpxr_persistent = true;
    return;
  case GMOR:
    r[GMOR] = v & GMOR_WRITABLE;
    return;
  case GPSR:
    // Writing the pixel shift resets the copy direction flag (4.4.5).
    r[GPSR] = v & 0xf;
    e.copy_drain = false;
    e.cbr_fill = 0;
    return;
  case GADR:
  case GADR_ALIAS:
    r[GADR] = v;
    e.addr_new = true;
    return;
  case GB3R:
    r[GB3R] = v;
    e.bres_new = true;
    return;
  case GCTR:
    r[GCTR] = v;
    fb_write(0, v, 0xf, true);
    return;
  case GDER:
    r[GDER] = v & 0x00015fff;
    m_generation++;
    return;
  case CCBR:
  case VHCR:
  case VVCR:
  case CXYR:
  case VSAR:
    r[reg] = v;
    m_generation++;
    return;
  case VVBR:
    r[VVBR] = v & 0x1ff;
    m_generation++;
    return;
  case VVVR:
    // The TGA2 adds the monitor's power state in <5:4> (tga2.sys writes
    // 01, 11, 21, 31 for D0 to D3): anything but on blanks the screen.
    r[VVVR] = v & (m_model.tga2() ? 0x3f : 0x7);
    m_generation++;
    return;
  case SISR: {
    // Enables are read-write; pending bits clear when written with one.
    std::lock_guard<std::mutex> lock(m_irq_lock);
    r[SISR] = (v & SISR_ENABLES) | (r[SISR] & SISR_PENDING & ~v);
    update_irq();
    return;
  }
  case GDBR:
  case GZBR:
    // The DMA base and Z base registers are one (4.4.7).
    r[GDBR] = r[GZBR] = v;
    return;
  case GSWR:
    // A write is slope register 7's (4.3.2).
    r[GSWR] = v;
    line_setup(7, v);
    line_draw(r[GDAR] & 0xffff, true);
    return;
  case EPSR:
    r[EPSR] = v & 0x1f;
    return;
  case GSNR0 + 0:
  case GSNR0 + 1:
  case GSNR0 + 2:
  case GSNR0 + 3:
  case GSNR0 + 4:
  case GSNR0 + 5:
  case GSNR0 + 6:
  case GSNR0 + 7:
    r[reg] = v;
    line_setup(reg - GSNR0, v);
    return;
  case GSLR0 + 0:
  case GSLR0 + 1:
  case GSLR0 + 2:
  case GSLR0 + 3:
  case GSLR0 + 4:
  case GSLR0 + 5:
  case GSLR0 + 6:
  case GSLR0 + 7:
    r[reg] = v;
    line_setup(reg - GSLR0, v);
    line_draw(r[GDAR] & 0xffff, true);
    return;
  case GCSR:
  case GCSR + 2:
  case GCSR + 4:
  case GCSR + 6:
    copy64_load(v & 0xffffff);
    return;
  case GCDR:
  case GCDR + 2:
  case GCDR + 4:
  case GCDR + 6:
    copy64_store(v & 0xffffff);
    return;
  case 0x68:
  case 0x6a:
  case 0x6c:
  case 0x6e:
  case 0x69:
  case 0x6b:
  case 0x6d:
  case 0x6f:
    // Reserved on the 21030. The TGA2 copies 128 bytes through them, in
    // source and destination pairs as GCSR and GCDR copy 64: tga2.dll
    // moves the interior of a screen-to-screen copy with them, 128 bytes a
    // pair, the edges in copy mode, through the byte shifter and in the
    // direction the pixel shift gives (copy_dir).
    if (!m_model.tga2()) {
      r[reg] = v;
      return;
    }
    if (reg & 1)
      copy64_store(v & 0xffffff, 16);
    else
      copy64_load(v & 0xffffff, 16);
    return;
  case ERWR:
    // The EEPROM takes writes only with GDER's ROM write enable.
    if (r[GDER] & 0x1000)
      m_rom[(v >> 8) & (ROM_BYTES - 1)] = u8(v);
    return;
  case ECGR:
    // The ICS1562 clock generator's serial port: 56 bits, the last with
    // the hold bit. Kept for the trace; the screen is drawn at a fixed
    // rate whatever the dot clock.
    e.icsbits = (e.icsbits >> 1) | ((v & 1) << 31);
    e.icscount++;
    if (v & 2) {
      trace("clock generator loaded (%u bits)", e.icscount);
      e.icscount = 0;
    }
    return;
  case EPDR:
    if (!m_model.tga2())
      epdr_write(v);
    return;
  case SCSR:
    return; // a barrier: nothing is ever outstanding
  default:
    r[reg] = v;
    return;
  }
}

/**
 * State file: the registers, the engine's working state, the RAMDAC, the
 * VRAM and the EEPROM.
 **/
static constexpr u32 kTgaMagic1 = 0x31414754; // 'TGA1'
static constexpr u32 kTgaMagic2 = 0x32414754; // 'TGA2'

int CTga::SaveState(FILE *f) {
  int res;
  if ((res = CPCIDevice::SaveState(f)))
    return res;
  fwrite(&kTgaMagic1, sizeof(u32), 1, f);
  long sz = sizeof(r) + sizeof(e) + sizeof(dac) + sizeof(ibm);
  fwrite(&sz, sizeof(long), 1, f);
  fwrite(r, sizeof(r), 1, f);
  fwrite(&e, sizeof(e), 1, f);
  fwrite(&dac, sizeof(dac), 1, f);
  fwrite(&ibm, sizeof(ibm), 1, f);
  const u64 vram = m_vram.size();
  fwrite(&vram, sizeof(u64), 1, f);
  fwrite(m_vram.data(), 1, m_vram.size(), f);
  fwrite(m_rom.data(), 1, m_rom.size(), f);
  fwrite(&kTgaMagic2, sizeof(u32), 1, f);
  printf("%s: %ld bytes saved (+ VRAM %llu).\n", devid_string, sz,
         (unsigned long long)vram);
  return 0;
}

int CTga::RestoreState(FILE *f) {
  int res;
  if ((res = CPCIDevice::RestoreState(f)))
    return res;
  u32 m;
  long sz;
  u64 vram;
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kTgaMagic1) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }
  if (fread(&sz, sizeof(long), 1, f) != 1 ||
      sz != long(sizeof(r) + sizeof(e) + sizeof(dac) + sizeof(ibm))) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }
  if (fread(r, sizeof(r), 1, f) != 1 || fread(&e, sizeof(e), 1, f) != 1 ||
      fread(&dac, sizeof(dac), 1, f) != 1 ||
      fread(&ibm, sizeof(ibm), 1, f) != 1 ||
      fread(&vram, sizeof(u64), 1, f) != 1 || vram != m_vram.size() ||
      fread(m_vram.data(), 1, m_vram.size(), f) != m_vram.size() ||
      fread(m_rom.data(), 1, m_rom.size(), f) != m_rom.size()) {
    printf("%s: unexpected end of file or VRAM size mismatch!\n", devid_string);
    return -1;
  }
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kTgaMagic2) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }
  {
    std::lock_guard<std::mutex> lock(m_irq_lock);
    m_irq_asserted = false;
    update_irq();
  }
  m_last_w = m_last_h = 0;
  m_generation++;
  printf("%s: %ld bytes restored (+ VRAM %llu).\n", devid_string, sz,
         (unsigned long long)vram);
  return 0;
}
