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
 * Radeon: construction, power-on state, PCI header, BAR routing, the state
 * file.
 **/

#include "Radeon.hpp"
#include "System.hpp"

using namespace radeon;

CRadeon::CRadeon(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev)
    : CVGACard(cfg, c, pcibus, pcidev) {
  memset(m_regs, 0, sizeof(m_regs));
  memset(m_pll, 0, sizeof(m_pll));
  memset(&eng, 0, sizeof(eng));
}

/**
 * Stops the render thread while this object is still whole: the thread
 * calls this card's hooks (see CVGACard::~CVGACard).
 **/
CRadeon::~CRadeon() {
  stop_threads();
  delete[] vga.memory;
  vga.memory = nullptr;
}

void CRadeon::init() {
  // "memory": 32 or 64 MB (the card HP shipped has 64 MB of DDR SDRAM).
  const u64 mb = myCfg->get_num_value("memory", false, 64);
  if (mb != 32 && mb != 64)
    FAILURE_1(Configuration, "radeon: memory must be 32 or 64 (MB), not %llu",
              (unsigned long long)mb);
  m_vram_bytes = u32(mb) << 20;

  // "model": which board. The consoles tell the two Radeon 7500 boards HP
  // sold apart by their subsystem IDs alone (the ES40's and the Marvel's
  // SRM tables: 1002:013A "Radeon 7500 AGP", 1002:013B "Radeon 7500 PCI").
  const char *model = myCfg->get_text_value("model", "agp");
  u32 subsystem;
  if (strcmp(model, "agp") == 0)
    subsystem = 0x013a1002;
  else if (strcmp(model, "pci") == 0)
    subsystem = 0x013b1002;
  else
    FAILURE_1(Configuration, "radeon: model must be \"agp\" or \"pci\", not %s",
              model);
  const bool agp = strcmp(model, "agp") == 0;

  // PCI header, as the RV200 has it: the framebuffer aperture (BAR0, 128
  // MB prefetchable: two 64 MB apertures over the same memory), the
  // 256-byte I/O window onto the first registers (BAR1), the 64 KB
  // register aperture (BAR2), a 128 KB expansion ROM, interrupt pin INTA.
  // Capabilities: AGP 2.0 at 0x58 (1x/2x/4x, sideband, 48 requests) on the
  // AGP board, then power management at 0x50.
  u32 cfg_data[64] = {};
  u32 cfg_mask[64] = {};
  cfg_data[0x00 >> 2] = (u32(PCI_DEVICE_RV200_QW) << 16) | PCI_VENDOR_ATI;
  cfg_data[0x04 >> 2] = 0x02b00000; // caps, 66 MHz, fast b2b, medium DEVSEL
  cfg_data[0x08 >> 2] = 0x03000000; // VGA display controller, revision 0
  cfg_data[0x10 >> 2] = 0x00000008; // prefetchable 32-bit memory
  cfg_data[0x14 >> 2] = 0x00000001; // I/O
  cfg_data[0x18 >> 2] = 0x00000000; // 32-bit memory
  cfg_data[0x2c >> 2] = subsystem;
  cfg_data[0x34 >> 2] = agp ? 0x58 : 0x50;
  cfg_data[0x3c >> 2] = 0x080001ff; // INTA, min grant 8
  cfg_data[0x50 >> 2] = 0x06020001; // PM 1.1: D1, D2; no next
  if (agp) {
    cfg_data[0x58 >> 2] = 0x00205002; // AGP 2.0, next 0x50
    cfg_data[0x5c >> 2] = 0x2f000207; // RQ 48, SBA, 1x/2x/4x
  }
  cfg_mask[0x04 >> 2] = 0x0000ffff;
  cfg_mask[0x0c >> 2] = 0x0000ffff;
  cfg_mask[0x10 >> 2] = ~(FB_APERTURE_BYTES - 1);
  cfg_mask[0x14 >> 2] = ~(IO_BAR_BYTES - 1);
  cfg_mask[0x18 >> 2] = ~(REG_APERTURE_BYTES - 1);
  cfg_mask[0x30 >> 2] = ~(ROM_BAR_BYTES - 1) | PCI_ROM_ADDRESS_ENABLE;
  cfg_mask[0x3c >> 2] = 0x000000ff;
  cfg_mask[0x54 >> 2] = 0x00008103; // PMCSR: power state, PME
  if (agp)
    cfg_mask[0x60 >> 2] = 0xff000317; // AGP command: depth, SBA, enable, rate
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

  // Radeon power-on state: what the straps and the memory controller say
  // before the BIOS has run. The framebuffer at 0 in the memory
  // controller's space, its size in CONFIG_MEMSIZE (which the BIOS
  // rewrites), the two apertures 64 MB each, the command FIFO empty.
  memset(m_regs, 0, sizeof(m_regs));
  memset(m_pll, 0, sizeof(m_pll));
  R(CONFIG_MEMSIZE) = m_vram_bytes;
  R(CONFIG_APER_SIZE) = FB_APERTURE_HALF;
  R(CONFIG_REG_APER_SIZE) = REG_APERTURE_BYTES;
  R(MC_FB_LOCATION) = ((m_vram_bytes - 1) & 0xffff0000u);
  R(CRTC_EXT_CNTL) = CRTC_CRT_ON;
  R(DAC_CNTL) = 0xff000000u | 2; // DAC mask all, PS/2 range
  R(HOST_PATH_CNTL) = 1u << 23;  // HDP aperture control, as QEMU reads it
  m_pll[PLL_PPLL_CNTL] = PPLL_RESET;
  engine_reset();
  ddc_attach_monitor();

  load_option_rom("radeon7500.rom");

  state.last_bpp = 8;
  state.x_tilesize = X_TILESIZE;
  state.y_tilesize = Y_TILESIZE;
  state.vga_mem_updated = 1;

  timing.divisor = 1;
  timing.vrefresh_hz = 60.0;
  timing.refresh_interval_ms = 16;
  m_last_refresh_time = std::chrono::steady_clock::now();

  if (const char *t = getenv("ALPHABOX_TRACE_RADEON")) {
    m_trace_new = strcmp(t, "new") == 0;
    m_trace = !m_trace_new;
    // A per-access trace is capped: a driver polling a status register
    // writes millions of lines. ALPHABOX_TRACE_RADEON_MAX raises the cap.
    m_trace_budget = 200000;
    if (const char *m = getenv("ALPHABOX_TRACE_RADEON_MAX"))
      m_trace_budget = atol(m);
  }
  printf("%s: ATI Radeon 7500 (RV200) %s, %u MB, subsystem %04x:%04x\n",
         devid_string, agp ? "AGP" : "PCI", m_vram_bytes >> 20,
         subsystem & 0xffff, subsystem >> 16);
}

u32 CRadeon::config_read_custom(int func, u32 address, int dsize, u32 data) {
  return data;
}

void CRadeon::config_write_custom(int func, u32 address, int dsize,
                                  u32 old_data, u32 new_data, u32 data) {
  if (m_trace && m_trace_budget > 0) {
    m_trace_budget--;
    printf("%s: config write %02x/%d = %08x (now %08x)\n", devid_string,
           address, dsize, data, new_data);
  }
  if (address <= 0x05 || (address >= 0x10 && address <= 0x13))
    refresh_direct_aperture();
}

/**
 * The VGA ports the base does not claim: 0x3c3 (the video subsystem
 * enable, which the Radeon keeps as a plain register) and the ATI ports
 * 0x3cb/0x3cd, dead here.
 **/
u8 CRadeon::io_read_b(u32 address) {
  switch (address) {
  case 0x3c3:
    return 1;
  case 0x3cb:
  case 0x3cd:
    return 0;
  }
  return CVGACard::io_read_b(address);
}

void CRadeon::io_write_b(u32 address, u8 data) {
  switch (address) {
  case 0x3c3:
  case 0x3cb:
  case 0x3cd:
    return;
  }
  CVGACard::io_write_b(address, data);
}

/**
 * BAR0: the framebuffer; BAR1: the I/O window onto the first 256 bytes of
 * the register aperture; BAR2: the register aperture; BAR6: the ROM.
 **/
u32 CRadeon::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  switch (bar) {
  case 0:
    m_trace_path = "fb";
    return fb_read(address, dsize);
  case 1:
    m_trace_path = "io";
    return reg_read(address & (IO_BAR_BYTES - 1), dsize / 8);
  case 2:
    m_trace_path = "mmio";
    return reg_read(address & (REG_APERTURE_BYTES - 1), dsize / 8);
  case 6:
    return rom_read(address, dsize);
  }
  return 0;
}

void CRadeon::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                           u32 data) {
  switch (bar) {
  case 0:
    m_trace_path = "fb";
    fb_write(address, dsize, data);
    return;
  case 1:
    m_trace_path = "io";
    reg_write(address & (IO_BAR_BYTES - 1), dsize / 8, data);
    return;
  case 2:
    m_trace_path = "mmio";
    reg_write(address & (REG_APERTURE_BYTES - 1), dsize / 8, data);
    return;
  }
}

/**
 * State file: the register file and the PLLs. The engine's decoded state
 * is rebuilt from the registers; a host-data transfer in flight is not
 * resumed.
 **/
static constexpr u32 kRadeonMagic = 0x37354152; // 'RA57'

int CRadeon::save_card_state(FILE *f) {
  fwrite(&kRadeonMagic, sizeof(u32), 1, f);
  long sz = sizeof(m_regs);
  fwrite(&sz, sizeof(long), 1, f);
  fwrite(m_regs, sizeof(m_regs), 1, f);
  sz = sizeof(m_pll);
  fwrite(&sz, sizeof(long), 1, f);
  fwrite(m_pll, sizeof(m_pll), 1, f);
  fwrite(&kRadeonMagic, sizeof(u32), 1, f);
  return 0;
}

int CRadeon::restore_card_state(FILE *f) {
  u32 m;
  long sz;
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kRadeonMagic) {
    printf("%s: Radeon MAGIC does not match!\n", devid_string);
    return -1;
  }
  if (fread(&sz, sizeof(long), 1, f) != 1 || sz != (long)sizeof(m_regs) ||
      fread(m_regs, sizeof(m_regs), 1, f) != 1) {
    printf("%s: Radeon register file does not match!\n", devid_string);
    return -1;
  }
  if (fread(&sz, sizeof(long), 1, f) != 1 || sz != (long)sizeof(m_pll) ||
      fread(m_pll, sizeof(m_pll), 1, f) != 1) {
    printf("%s: Radeon PLL block does not match!\n", devid_string);
    return -1;
  }
  if (fread(&m, sizeof(u32), 1, f) != 1 || m != kRadeonMagic) {
    printf("%s: Radeon end MAGIC does not match!\n", devid_string);
    return -1;
  }
  return 0;
}

void CRadeon::post_restore() {
  engine_reset();
  refresh_direct_aperture();
}
