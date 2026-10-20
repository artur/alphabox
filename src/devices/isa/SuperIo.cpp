/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * Contains code from ES40-Emu:
 * Copyright (C) 2024-2026 gdwnldsKSC and the ES40-Emu contributors
 * Website: https://github.com/ES40-Emu/es40
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

/* The ALi M1543C's Super I/O configuration ports and the ISA Plug and Play
 * ports (see SuperIo.hpp).
 */

#include "SuperIo.hpp"

#include <cstdio>
#include <cstring>

void CSuperIo::reset() {
  memset(s.chip_regs, 0, sizeof(s.chip_regs));
  memset(s.ldn_regs, 0, sizeof(s.ldn_regs));

  s.config_mode = false;
  s.unlock_state = 0;
  s.index = 0;
  s.ldn = 0;

  s.chip_regs[0x20] = 0x43;
  s.chip_regs[0x21] = 0x15;
  s.chip_regs[0x22] = 0x00;
  s.chip_regs[0x23] = 0x00;
  s.chip_regs[0x26] = 0x80; // 0x80 = M1543_DEV_STATUS_KBC
  s.chip_regs[0x2d] = 0x20;
  s.chip_regs[0x2e] = 0x20;

  // The FDC logical device is chipset-integrated hardware: always active. CMOS
  // byte 0x10 describes the attached drives, not whether the controller exists.
  s.ldn_regs[0][0x30] = 0x01;
  s.ldn_regs[0][0x60] = 0x03;
  s.ldn_regs[0][0x61] = 0xf0;
  s.ldn_regs[0][0x70] = 0x06;
  s.ldn_regs[0][0x74] = 0x02;
  s.ldn_regs[0][0xf0] = 0x08;
  s.ldn_regs[0][0xf1] = 0x00;
  s.ldn_regs[0][0xf2] = 0xff;
  s.ldn_regs[0][0xf4] = 0x00;

  // LDN 3 — Parallel port.  Reset values mirror QEMU's M1543 emulation
  // (hw/isa/m1543.c) which is the known-good reference for booting
  // Windows NT 5.x on this chipset; the high bit in CFG0/CFG1 selects
  // the parallel-port mode bits the OS expects to round-trip.
  s.ldn_regs[3][0x30] = 0x00;
  s.ldn_regs[3][0x60] = 0x03;
  s.ldn_regs[3][0x61] = 0x78;
  s.ldn_regs[3][0x70] = 0x05;
  s.ldn_regs[3][0x74] = 0x04;
  s.ldn_regs[3][0xf0] = 0x8c;
  s.ldn_regs[3][0xf1] = 0x85;

  // LDN 4 — UART1 (COM1).
  s.ldn_regs[4][0x30] = 0x00;
  s.ldn_regs[4][0x60] = 0x03;
  s.ldn_regs[4][0x61] = 0xf8;
  s.ldn_regs[4][0x70] = 0x04;
  s.ldn_regs[4][0xf0] = 0x00;
  s.ldn_regs[4][0xf1] = 0x00;
  s.ldn_regs[4][0xf2] = 0x0c;

  // LDN 5 — UART2 (COM2).
  s.ldn_regs[5][0x30] = 0x00;
  s.ldn_regs[5][0x60] = 0x02;
  s.ldn_regs[5][0x61] = 0xf8;
  s.ldn_regs[5][0x70] = 0x03;
  s.ldn_regs[5][0xf0] = 0x00;
  s.ldn_regs[5][0xf1] = 0x00;
  s.ldn_regs[5][0xf2] = 0x0c;

  // LDN 7 — Keyboard controller.  CFG0 bit 6 (0x40) is the KBC speed/
  // translate enable bit.
  s.ldn_regs[7][0x30] = 0x01; // active
  s.ldn_regs[7][0x60] = 0x00; // base high (informational)
  s.ldn_regs[7][0x61] = 0x60; // base low — KBC at 0x60/0x64
  s.ldn_regs[7][0x70] = 0x01; // keyboard IRQ1
  s.ldn_regs[7][0x72] = 0x0c; // AUX IRQ12
  s.ldn_regs[7][0xf0] = 0x40; // KBC vendor config

  // LDN 8 — AUX (mouse).  M1543C exposes mouse via the LDN 7 KBC
  // (the IRQ12 wire is on KBD's reg 0x72), but NT may also probe LDN 8
  // as a separate "logical" mouse device — keep it advertised so the
  // PnP enumerator doesn't mark the port driver as "no mouse".
  s.ldn_regs[8][0x30] = 0x01;
  s.ldn_regs[8][0x70] = 0x0c;
  s.ldn_regs[8][0xf0] = 0x00;

  // LDN 0xC — Hotkey controller. Whistler's
  // PnP enumerator reads these to determine whether a hotkey logical
  // device exists.  Reporting consistent values keeps the enumeration
  // from looping on this LDN.
  s.ldn_regs[0xc][0xf0] = 0x35;
  s.ldn_regs[0xc][0xf1] = 0x14;
  s.ldn_regs[0xc][0xf2] = 0x11;
  s.ldn_regs[0xc][0xf3] = 0x71;
  s.ldn_regs[0xc][0xf4] = 0x42;
}

void CSuperIo::restored() {
  for (int ldn = 0; ldn < 16; ldn++)
    apply_ldn(ldn);
}

u8 CSuperIo::current_reg() const {
  if (s.index == 0x07)
    return s.ldn;

  if (s.index < 0x30)
    return s.chip_regs[s.index];

  return s.ldn_regs[s.ldn & 0x0f][s.index];
}

u8 CSuperIo::read(u32 address) {
  if (!s.config_mode)
    return 0xff;

  if (address == 0)
    return s.index;

  const u8 value = current_reg();
#ifdef DEBUG_SUPERIO
  static int superio_read_log_count = 0;
  printf("*** SUPERIO READ[%d]: port=%03x index=%02x ldn=%02x value=%02x\n",
         superio_read_log_count++, address + 0x370, s.index, s.ldn, value);
#endif
  return value;
}

void CSuperIo::write(u32 address, u8 data) {
#ifdef DEBUG_SUPERIO
  static int superio_write_log_count = 0;
  printf("*** SUPERIO WRITE[%d]: port=%03x data=%02x cfg=%d index=%02x "
         "ldn=%02x \n",
         superio_write_log_count++, address + 0x370, data,
         s.config_mode ? 1 : 0, s.index, s.ldn);
#endif

  if (address == 0) {
    if (!s.config_mode) {
      if ((s.unlock_state == 0 && data == 0x51) ||
          (s.unlock_state == 1 && data == 0x23)) {
        s.unlock_state++;
        if (s.unlock_state == 2) {
          s.config_mode = true;
          s.unlock_state = 0;
#ifdef DEBUG_SUPERIO
          printf("*** SUPERIO: entered configuration mode on port 370h\n");
#endif
        }
        return;
      }
      s.unlock_state = (data == 0x51) ? 1 : 0;
      return;
    }

    if (data == 0xbb) {
      s.config_mode = false;
      s.unlock_state = 0;
#ifdef DEBUG_SUPERIO
      printf("*** SUPERIO: exited configuration mode on port 370h\n");
#endif
      return;
    }

    s.index = data;
    return;
  }

  if (!s.config_mode)
    return;

  if (s.index == 0x02) {
    if (data & 0x01)
      reset();
    return;
  }

  if (s.index == 0x07) {
    s.ldn = data & 0x0f;
    return;
  }

  if (s.index == 0x20 || s.index == 0x21 || s.index == 0x22)
    return;

  if (s.index < 0x30) {
    s.chip_regs[s.index] = data;
    return;
  }

  s.ldn_regs[s.ldn & 0x0f][s.index] = data;

  if (s.index == 0x30 || s.index == 0x60 || s.index == 0x61) {
    apply_ldn(s.ldn & 0x0f);
  }
}

void CSuperIo::apply_ldn(int ldn) {
  const u8 activate = s.ldn_regs[ldn][0x30] & 0x01;
  const u16 base = (u16)((s.ldn_regs[ldn][0x60] << 8) | s.ldn_regs[ldn][0x61]);
  const u8 irq1 = s.ldn_regs[ldn][0x70];
  const u8 irq2 = s.ldn_regs[ldn][0x72];

  switch (ldn) {
  case 0: // FDC.  CFloppyController binds 0x3f0-0x3f7 statically;
          // honoring an OS-driven relocation here would require a hook into
          // that class.  For now just acknowledge the activation so PnP
          // enumeration sees a coherent picture.
#ifdef DEBUG_SUPERIO
    printf("%s: FDC %s (base=%#06x irq=%u)\n", "ali",
           activate ? "activated" : "deactivated", base, irq1);
#endif
    break;

  case 3: // LPT — only LDN with dynamic binding implemented.
    if (activate && base != 0) {
      map(3, base & 0xfffc);
#ifdef DEBUG_SUPERIO
      printf("%s: LPT activated at %#06x\n", "ali", base & 0xfffc);
#endif
    }
#ifdef DEBUG_SUPERIO
    else {
      printf("%s: LPT deactivated (activate=%u base=%#06x)\n", "ali", activate,
             base);
    }
#endif
    break;

  case 4: // UART1 (COM1).  CSerial binds its ports per-instance from
          // the configurator.
  case 5: // UART2 (COM2).
#ifdef DEBUG_SUPERIO
    printf("%s: COM%d %s (base=%#06x irq=%u)\n", "ali", ldn - 3,
           activate ? "activated" : "deactivated", base, irq1);
#endif
    break;

  case 7: // Keyboard controller.  CKeyboard binds 0x60/0x64 statically;
          // the LDN advertises the same so the OS sees a self-consistent
          // SuperIO view.
#ifdef DEBUG_SUPERIO
    printf("%s: KBC %s (base=%#06x kbd_irq=%u aux_irq=%u)\n", "ali",
           activate ? "activated" : "deactivated", base, irq1, irq2);
#endif
    (void)irq2;
    break;

  case 8: // AUX (mouse) — IRQ wire lives on the KBC; nothing to bind.
#ifdef DEBUG_SUPERIO
    printf("%s: AUX %s (irq=%u)\n", "ali",
           activate ? "activated" : "deactivated", irq1);
#endif
    break;

  default:
#ifdef DEBUG_SUPERIO
    printf("%s: LDN %#x apply (activate=%u base=%#06x)\n", "ali", ldn, activate,
           base);
#endif
    break;
  }
}

/**
 * \brief ISA Plug-and-Play protocol handlers.
 *
 * The OS drives ISA PnP card discovery via three I/O ports:
 *   - 0x279 (ADDRESS, write-only): selects the next register to access
 *     and is also the channel for the 32-byte LFSR initiation key that
 *     transitions cards from Wait-For-Key into Sleep state.
 *   - 0xA79 (WRITE_DATA, write-only): writes the value of the register
 *     previously selected via 0x279.
 *   - READ_DATA (read-only): an OS-selectable address in 0x203-0x3FF,
 *     programmed by writing PnP register 0x00.  Used for serial
 *     isolation reads and per-card resource descriptor reads.
 *
 * The es40 platform exposes no ISA PnP cards.  The handlers below run
 * the protocol state machine just enough to swallow the OS's enumeration
 * cleanly and report "no cards found": isolation reads return 0xff so
 * the OS sees a failed alternating 0x55/0xAA pattern and gives up after
 * the standard timeout, instead of generating a flood of "unknown IO
 * port" warnings every boot.
 *
 * Reference: Plug and Play ISA Specification, version 1.0a (1994),
 * Microsoft / Intel, section 4 (auto-configuration ports).
 **/

const u8 CIsaPnp::init_key[32] = {
    0x6A, 0xB5, 0xDA, 0xED, 0xF6, 0xFB, 0x7D, 0xBE, 0xDF, 0x6F, 0x37,
    0x1B, 0x0D, 0x86, 0xC3, 0x61, 0xB0, 0x58, 0x2C, 0x16, 0x8B, 0x45,
    0xA2, 0xD1, 0xE8, 0x74, 0x3A, 0x9D, 0xCE, 0xE7, 0x73, 0x39};

void CIsaPnp::reset() {
  s.state = PNP_WAIT_FOR_KEY;
  s.key_pos = 0;
  s.reg = 0;
  s.rd_port = 0;
  s.wake_csn = 0;
}

void CIsaPnp::restored() {
  if (valid_read_port(s.rd_port))
    map(s.rd_port);
}

void CIsaPnp::addr_write(u8 data) {
#ifdef DEBUG_ISAPNP
  printf("%%PNP-I-ADDR: write 0x%02x to 0x279 (state=%d, key_pos=%d)\n", data,
         s.state, s.key_pos);
#endif

  if (s.state == PNP_WAIT_FOR_KEY) {
    // Per spec 4.3.2: the LFSR is reset by writing 0x00 to ADDRESS
    // twice in succession, after which the 32-byte initiation key
    // must arrive byte-by-byte without interruption.  Any byte that
    // fails to match the expected position resets our match counter.
    if (data == 0x00 && s.key_pos < 2) {
      s.key_pos = 0;
      return;
    }

    if (data == init_key[s.key_pos]) {
      s.key_pos++;
      if (s.key_pos >= 32) {
#ifdef DEBUG_ISAPNP
        printf("%%PNP-I-KEY: initiation key accepted, entering SLEEP\n");
#endif
        s.state = PNP_SLEEP;
        s.key_pos = 0;
      }
    } else {
      s.key_pos = 0;
    }
    return;
  }

  // Past the key — every ADDRESS write selects the register that the
  // next 0xA79 / READ_DATA access will operate on.
  s.reg = data;
}

void CIsaPnp::data_write(u8 data) {
#ifdef DEBUG_ISAPNP
  printf("%%PNP-I-DATA: write 0x%02x to reg 0x%02x (state=%d)\n", data, s.reg,
         s.state);
#endif

  if (s.state == PNP_WAIT_FOR_KEY)
    return;

  switch (s.reg) {
  case 0x00: // Set RD_DATA Port — actual port = (data << 2) | 0x0003
  {
    u16 new_port = ((u16)data << 2) | 0x0003;
    // The READ_DATA port is relocatable within 0x203-0x3FF (ISA PnP 1.0a,
    // section 3.1, Table 1). No card here answers anywhere else: placing
    // the hook lower would shadow the DMA controller, the PICs and the
    // timer.
    if (!valid_read_port(new_port))
      break;
    s.rd_port = new_port;
    // Re-register our READ_DATA hook at the OS-chosen port.  The
    // system memory map updates id 52 in place, so successive
    // programmings just relocate the same registration.
    map(new_port);
#ifdef DEBUG_ISAPNP
    printf("%%PNP-I-RDPORT: READ_DATA port set to 0x%03x\n", new_port);
#endif
    break;
  }

  case 0x02: // Config Control
    // bit 0: Reset (drives RESET_DRV — all cards back to Wait-For-Key)
    // bit 1: Wait For Key (return to Wait-For-Key)
    // bit 2: Reset CSN (clear all CSNs to zero)
    if (data & 0x03) {
      s.state = PNP_WAIT_FOR_KEY;
      s.key_pos = 0;
    }
    break;

  case 0x03: // Wake[CSN]
    s.wake_csn = data;
    // CSN==0 with a card in Sleep starts isolation; non-zero wakes
    // the card with that CSN into Config.  We have no cards either
    // way, but tracking the state lets future reads of register 0x05
    // (Status) return something coherent if asked.
    if (s.state == PNP_SLEEP)
      s.state = (data == 0) ? PNP_ISOLATION : PNP_CONFIG;
    break;

  case 0x06: // Set CSN — assign CSN to the card that won isolation.
    // No card won, no-op.
    break;

  default:
    // Logical-device select (0x07), card-level resource registers
    // (0x20-0x2F) and per-LDN resource registers (0x30-0xFF) are
    // per-card.  No cards, nothing to record.
    break;
  }
}

u8 CIsaPnp::data_read() {
  // With no PnP cards driving the bus, isolation reads see floating
  // pull-ups (0xff).  The OS interprets a {0xff, 0xff} pair as a "0"
  // bit; after 72 such reads it computes a checksum that won't match,
  // concludes no card responded, and ends the isolation sequence.
  return 0xff;
}
