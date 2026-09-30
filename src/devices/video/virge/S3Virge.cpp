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
 * S3 ViRGE: the chip table, construction, power-on state, the PCI header,
 * the S3 CRTC and sequencer extensions with their locks, the VGA ports,
 * the state file.
 **/

#include "S3Virge.hpp"
#include "System.hpp"

using namespace virge;

/* The family. Only the DX has been run against its BIOS and the Windows
 * 2000 driver; the other rows carry the identity each part reports and
 * the differences the data books and 86Box record, and are not verified. */
static const virge_chip_config virge_chips[] = {
    // The original ViRGE (86C325). 2-bit PLL R, 2 VCLKs per 16-bit pixel.
    {"virge", "ViRGE (86C325)", 0x5631, 0x00, 4u << 20, "86c325.bin", false,
     false, false},
    // ViRGE/VX (86C988): VRAM, up to 8 MB, a 32 MB BAR, one VCLK a pixel.
    {"vx", "ViRGE/VX (86C988)", 0x883d, 0x00, 4u << 20,
     "diamondstealth3000.vbi", true, false, false},
    // ViRGE/DX (86C375). The GX (86C385) is the same device ID with
    // revision 1 and SGRAM; the S3 reference BIOS 2.01.16 drives both.
    {"dx", "ViRGE/DX (86C375)", 0x8a01, 0x00, 4u << 20, "86c375_4.bin", false,
     true, false},
    {"gx", "ViRGE/GX (86C385)", 0x8a01, 0x01, 4u << 20, "86c375_4.bin", false,
     true, false},
    // ViRGE/GX2 (86C357).
    {"gx2", "ViRGE/GX2 (86C357)", 0x8a10, 0x00, 4u << 20,
     "DS3D4K v1.03 Brightness bug fix.bin", false, true, true},
};

const virge_chip_config *virge_chip_by_name(const char *name) {
  for (const auto &c : virge_chips)
    if (strcmp(c.name, name) == 0)
      return &c;
  return nullptr;
}

CS3Virge::CS3Virge(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev,
                   const virge_chip_config &chip)
    : CVGACard(cfg, c, pcibus, pcidev), m_chip(chip) {
  memset(&r, 0, sizeof(r));
}

/**
 * Stops the render thread while this object is still whole: the thread
 * calls this card's hooks (see CVGACard::~CVGACard).
 **/
CS3Virge::~CS3Virge() {
  stop_threads();
  delete[] vga.memory;
  vga.memory = nullptr;
}

void CS3Virge::init() {
  const u64 mb = myCfg->get_num_value("memory", false, m_chip.vram_bytes >> 20);
  const u64 max_mb = m_chip.vx ? 8 : 4;
  if (mb != 2 && mb != 4 && !(mb == 8 && max_mb == 8))
    FAILURE_2(Configuration, "virge: memory must be 2 or 4 (MB)%s, not %llu",
              max_mb == 8 ? " or 8" : "", (unsigned long long)mb);
  m_vram_bytes = u32(mb) << 20;

  // PCI header: a VGA-compatible display controller; BAR0 is the one
  // memory range, 64 MB (32 MB on the VX), not prefetchable -- its upper
  // part holds registers. The expansion ROM holds the 32 KB BIOS; the
  // console finds its copy at 0xc0000 like the other cards'. Interrupt
  // pin INTA: the vertical retrace and the engine's events, once CR32
  // lets them out. The subsystem IDs are S3's own, as on the reference
  // boards.
  const u32 bar_bytes = m_chip.vx ? BAR0_BYTES / 2 : BAR0_BYTES;
  u32 cfg_data[64] = {};
  u32 cfg_mask[64] = {};
  cfg_data[0x00 >> 2] = (u32(m_chip.pci_device_id) << 16) | PCI_VENDOR_S3;
  cfg_data[0x04 >> 2] = 0x02000000; // status: medium DEVSEL
  cfg_data[0x08 >> 2] = 0x03000000 | m_chip.revision;
  cfg_data[0x2c >> 2] = (u32(m_chip.pci_device_id) << 16) | PCI_VENDOR_S3;
  cfg_data[0x3c >> 2] = 0x040001ff; // INTA, min grant 4
  cfg_mask[0x04 >> 2] = 0x00000027; // I/O, memory, bus master, palette snoop
  cfg_mask[0x0c >> 2] = 0x0000ff00; // latency timer
  cfg_mask[0x10 >> 2] = ~(bar_bytes - 1);
  cfg_mask[0x30 >> 2] = ~(u32(sizeof(option_rom)) - 1) | PCI_ROM_ADDRESS_ENABLE;
  cfg_mask[0x3c >> 2] = 0x000000ff;
  add_function(0, cfg_data, cfg_mask);
  ResetPCI();

  memset((void *)&state, 0, sizeof(state));
  memset(&vga, 0, sizeof(vga));
  memset(&svga, 0, sizeof(svga));

  vga.svga_intf.vram_size = m_vram_bytes;
  vga.memory = new u8[m_vram_bytes];
  memset(vga.memory, 0, m_vram_bytes);

  add_vga_legacy_ranges();
  init_maps();

  // Standard VGA power-on state.
  vga.gc.bit_mask = 0xff;
  vga.gc.memory_map_sel = 3; // colour text
  vga.sequencer.data[0] = 0x03;
  vga.sequencer.data[4] = 0x06;
  vga.sequencer.char_sel.base[0] = 0x20000;
  vga.sequencer.char_sel.base[1] = 0x20000;
  vga.crtc.line_compare = 1023;
  vga.crtc.vert_disp_end = 399;
  vga.dac.mask = 0xff;
  vga.dac.dirty = 1;

  // ViRGE power-on state. CR36/CR37 are the configuration straps: PCI
  // bus, the memory type and size (CR36 bits 7..5: 000 4 MB, 100 2 MB on
  // the DX; 010 4 MB, 011 8 MB, 000 2 MB on the VX) and CR37's monitor
  // and clock straps as S3's reference boards set them.
  memset(&r, 0, sizeof(r));
  u8 *c = vga.crtc.data;
  c[CR_DEVICE_ID_HIGH] = u8(m_chip.pci_device_id >> 8);
  c[CR_DEVICE_ID_LOW] = u8(m_chip.pci_device_id);
  c[CR_REVISION] = m_chip.revision;
  c[CR_CHIP_ID] = 0xe1;
  if (m_chip.vx)
    c[CR_CONFIG_1] = mb == 2 ? 0x00 : mb == 4 ? 0x40 : 0x60;
  else
    c[CR_CONFIG_1] = 0x12 | (mb == 2 ? 0x80 : 0x00) |
                     (m_chip.revision ? 0x04 : 0x00); // GX: SGRAM
  c[CR_CONFIG_2] = 0xe1;
  c[CR_LAW_POS_HI] = 0x70;
  if (m_chip.dx)
    c[0x6c] = 0x01;
  if (m_chip.revision)
    c[CR_CONFIG_4] = 0x01;
  vga.sequencer.data[SR_MCLK_N] = 0x42; // ~50 MHz memory clock
  vga.sequencer.data[SR_MCLK_M] = 0x2e;
  vga.sequencer.data[SR_DCLK_N] = 0x00;
  vga.sequencer.data[SR_DCLK_M] = 0x00;
  r.port_3c3 = 0x01;
  M(SUBSYS_STATUS) = 0;
  engine_reset();
  ddc_attach_monitor();
  update_clock();

  load_option_rom(m_chip.default_rom);

  state.last_bpp = 8;
  state.x_tilesize = X_TILESIZE;
  state.y_tilesize = Y_TILESIZE;
  state.vga_mem_updated = 1;

  timing.divisor = 1;
  timing.vrefresh_hz = 60.0;
  timing.refresh_interval_ms = 16;
  m_last_refresh_time = std::chrono::steady_clock::now();

  // ALPHABOX_TRACE_VIRGE, a comma-separated list: "1" (or "all") every
  // port, register and window access; "cmd" the engine's commands, one
  // line each; "mode" each change of the extended display mode;
  // "file:<path>" every access while <path> exists (polled, so a trace
  // can be switched on and off around the step that matters).
  if (const char *t = getenv("ALPHABOX_TRACE_VIRGE")) {
    std::string s(t);
    size_t p = 0;
    while (p <= s.size()) {
      size_t q = s.find(',', p);
      if (q == std::string::npos)
        q = s.size();
      const std::string tok = s.substr(p, q - p);
      if (tok == "cmd")
        m_trace_cmd = true;
      else if (tok == "mode")
        m_trace_mode = true;
      else if (tok.compare(0, 5, "file:") == 0)
        m_trace_file = tok.substr(5);
      else if (!tok.empty())
        m_trace = true;
      p = q + 1;
    }
  }
  printf("%s: S3 %s, %u KB\n", devid_string, m_chip.part, m_vram_bytes / 1024);
}

void CS3Virge::unimplemented(const std::string &what) {
  if (m_unimplemented_seen.insert(what).second)
    printf("%s: not implemented: %s\n", devid_string, what.c_str());
}

u32 CS3Virge::config_read_custom(int func, u32 address, int dsize, u32 data) {
  if (m_trace)
    printf("%s: config read  %02x/%d = %08x\n", devid_string, address, dsize,
           data);
  return data;
}

void CS3Virge::config_write_custom(int func, u32 address, int dsize,
                                   u32 old_data, u32 new_data, u32 data) {
  if (m_trace)
    printf("%s: config write %02x/%d = %08x (now %08x)\n", devid_string,
           address, dsize, data, new_data);
  // COMMAND's memory enable and BAR0 decide where, and whether, the
  // framebuffer is.
  if (address <= 0x05 || (address >= 0x10 && address <= 0x13))
    refresh_direct_lfb();
}

u32 CS3Virge::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  u32 v;
  switch (bar) {
  case 0:
    v = bar0_read(address, dsize);
    break;
  case 6:
    v = rom_read(address & 0x7fff, dsize);
    break;
  default:
    return 0;
  }
  if (m_trace)
    printf("%s: bar%d read  %07x/%d = %0*x\n", devid_string, bar, address,
           dsize / 8, dsize / 4, v);
  return v;
}

void CS3Virge::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data) {
  if (m_trace)
    printf("%s: bar%d write %07x/%d = %0*x\n", devid_string, bar, address,
           dsize / 8, dsize / 4, data);
  if (bar == 0)
    bar0_write(address, dsize, data);
}

/**
 * The VGA ports. The CRTC's index is eight bits here (the core keeps
 * seven), and the S3 extensions -- CRTC indices from 0x2d, sequencer
 * indices from 8 -- are this card's, behind their locks.
 **/
u8 CS3Virge::io_read_b(u32 address) {
  u8 v;
  switch (address) {
  case 0x3b5:
  case 0x3d5:
    v = vga.crtc.index >= CR_DEVICE_ID_HIGH ? crtc_ext_read(vga.crtc.index)
                                            : CVGACard::io_read_b(address);
    break;
  case 0x3c5:
    v = vga.sequencer.index >= SR_UNLOCK ? sr(vga.sequencer.index)
                                         : CVGACard::io_read_b(address);
    break;
  case 0x3c3:
    v = r.port_3c3;
    break;
  case 0x3cb:
  case 0x3cd:
    v = 0;
    break;
  default:
    v = CVGACard::io_read_b(address);
    break;
  }
  if (m_trace && address != 0x3da && address != 0x3ba)
    printf("%s: port read  %03x = %02x\n", devid_string, address, v);
  return v;
}

void CS3Virge::io_write_b(u32 address, u8 data) {
  if (m_trace)
    printf("%s: port write %03x = %02x\n", devid_string, address, data);
  switch (address) {
  case 0x3b4:
  case 0x3d4:
    vga.crtc.index = data;
    return;
  case 0x3b5:
  case 0x3d5:
    if (vga.crtc.index >= CR_DEVICE_ID_HIGH) {
      crtc_ext_write(vga.crtc.index, data);
      return;
    }
    break;
  case 0x3c5: {
    const u8 i = vga.sequencer.index;
    if (i < SR_UNLOCK)
      break;
    // SR08 unlocks the rest with 0x06; the PLL load register strobes.
    if (i != SR_UNLOCK && sr(SR_UNLOCK) != 0x06)
      return;
    vga.sequencer.data[i] = data;
    if (i == SR_CLKSYN_2 || i == SR_DCLK_N || i == SR_DCLK_M)
      update_clock();
    return;
  }
  case 0x3c3:
    r.port_3c3 = data;
    return;
  case 0x3cb:
  case 0x3cd:
    return;
  }
  CVGACard::io_write_b(address, data);
}

/**
 * An S3 CRTC register. CR2D-CR30 identify the chip and are read-only;
 * CR31-CR3F answer only after CR38 = 0x48 and CR40 up after CR39 =
 * 0xa0..0xbf (the configuration straps CR36, CR37, CR68 and CR6F need
 * 0xa5). The chip ID register reads 0xff while both are locked.
 **/
u8 CS3Virge::crtc_ext_read(u8 index) {
  switch (index) {
  case CR_CHIP_ID:
    return (cr_unlocked_s3() || cr_unlocked_ext()) ? cr(CR_CHIP_ID) : 0xff;
  case CR_MEM_CONFIG:
    return cr(index);
  case CR_CRT_LOCK:
    return (cr(index) & 0xf0) | (r.bank & 0x0f);
  case CR_CURSOR_MODE:
    r.cursor_fg_pos = r.cursor_bg_pos = 0; // resets the colour stacks
    return cr(index);
  case CR_CURSOR_FG:
    return r.cursor_fg[r.cursor_fg_pos++ & 3];
  case CR_CURSOR_BG:
    return r.cursor_bg[r.cursor_bg_pos++ & 3];
  case CR_EXT_SYS_CTL_2:
    return (cr(index) & 0xf0) | ((r.bank >> 2) & 0x0c) | (cr(index) & 0x03);
  case CR_LAW_POS_HI:
  case CR_LAW_POS_LO: {
    // The PCI BAR is where the window is; these read it back.
    const u32 bar0 = config_read(0, 0x10, 32);
    return index == CR_LAW_POS_HI ? u8(bar0 >> 24) : u8(bar0 >> 16);
  }
  case CR_GENERAL_OUT: {
    // Bits 3..0 read the clock select in use.
    u8 v = cr(index) & 0xf0;
    if (((vga.miscellaneous_output >> 2) & 3) == 3)
      v |= cr(0x42) & 0x0f;
    else
      v |= (vga.miscellaneous_output >> 2) & 3;
    return v;
  }
  case CR_EXT_H_OVF:
  case CR_EXT_V_OVF:
    return cr(index);
  case CR_EXT_SYS_CTL_4:
    return r.bank;
  }
  return cr(index);
}

void CS3Virge::crtc_ext_write(u8 index, u8 data) {
  if (index <= CR_CHIP_ID)
    return; // identification: read-only
  if (index < 0x40) {
    if (index != CR_LOCK_1 && index != CR_LOCK_2 && !cr_unlocked_s3())
      return;
    if (index == CR_CONFIG_1 || index == CR_CONFIG_2) {
      if (cr(CR_LOCK_2) != 0xa5)
        return;
    }
  } else if (!cr_unlocked_ext()) {
    return;
  } else if ((index == CR_CONFIG_3 || index == CR_CONFIG_4) &&
             cr(CR_LOCK_2) != 0xa5) {
    return;
  }

  const u8 old = cr(index);
  vga.crtc.data[index] = data;
  switch (index) {
  case CR_CRT_LOCK:
    r.bank = (r.bank & 0x70) | (data & 0x0f);
    break;
  case CR_EXT_SYS_CTL_2:
    r.bank = (r.bank & 0x4f) | ((data & 0x0c) << 2);
    break;
  case CR_EXT_SYS_CTL_4:
    r.bank = data & 0x7f;
    break;
  case CR_BKWD_1: {
    std::lock_guard<std::mutex> lock(m_int_lock);
    update_int_line();
    break;
  }
  case CR_CURSOR_FG:
    r.cursor_fg[r.cursor_fg_pos++ & 3] = data;
    break;
  case CR_CURSOR_BG:
    r.cursor_bg[r.cursor_bg_pos++ & 3] = data;
    break;
  case CR_EXT_MEM_CTL_1:
  case CR_LAW_CTL:
    refresh_direct_lfb();
    break;
  case CR_LAW_POS_HI:
  case CR_LAW_POS_LO:
    // Firmware for a VL-bus part would place the window here; on PCI the
    // BAR does, and these only read it back.
    break;
  case CR_EXT_MISC_1:
    if ((data & CR66_ENGINE_RESET) && !(old & CR66_ENGINE_RESET))
      engine_reset();
    break;
  case CR_GENERAL_OUT:
    break;
  }
  if (old != data)
    state.vga_mem_updated = 1;
}

/**
 * The display's horizontal and vertical overflow bits (CR5D, CR5E),
 * for the VGA path's screen size.
 **/
void CS3Virge::apply_extended_timing(int &h, int &v) {
  if (cr(CR_EXT_H_OVF) & 0x02)
    h += 256 * (seq_dotperchar() ? 8 : 9) / timing.divisor;
  if (cr(CR_EXT_V_OVF) & 0x02)
    v += 1024;
}

/// CR33 bit 6 write-protects the palette and overscan registers.
bool CS3Virge::atc_palette_locked() const {
  return (cr(CR_BKWD_2) & 0x40) != 0;
}

void CS3Virge::crtc_map(address_map &map) {}

void CS3Virge::sequencer_map(address_map &map) {}

/**
 * State file: the chip's registers beyond the VGA core (whose CRTC and
 * sequencer data arrays hold the S3 extensions). The memory is the VGA
 * core's.
 **/
static constexpr u32 kVirgeMagic = 0x33445653; // 'SVD3'

int CS3Virge::save_card_state(FILE *f) {
  fwrite(&kVirgeMagic, sizeof(u32), 1, f);
  long sz = sizeof(r);
  fwrite(&sz, sizeof(long), 1, f);
  fwrite(&r, sizeof(r), 1, f);
  fwrite(&kVirgeMagic, sizeof(u32), 1, f);
  return 0;
}

int CS3Virge::restore_card_state(FILE *f) {
  u32 m;
  long sz;
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kVirgeMagic) {
    printf("%s: ViRGE MAGIC does not match!\n", devid_string);
    return -1;
  }
  if (fread(&sz, sizeof(long), 1, f) != 1 || sz != (long)sizeof(r) ||
      fread(&r, sizeof(r), 1, f) != 1) {
    printf("%s: ViRGE register block does not match!\n", devid_string);
    return -1;
  }
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kVirgeMagic) {
    printf("%s: ViRGE end MAGIC does not match!\n", devid_string);
    return -1;
  }
  return 0;
}

void CS3Virge::post_restore() {
  m_int_asserted = false;
  {
    std::lock_guard<std::mutex> lock(m_int_lock);
    update_int_line();
  }
  refresh_direct_lfb();
}
