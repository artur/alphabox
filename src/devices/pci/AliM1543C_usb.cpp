/* AXPbox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 */

#include "AliM1543C_usb.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

u32 usb_cfg_data[64] = {
    /*00*/ 0x523710b9, // CFID: vendor + device
    /*04*/ 0x02800000, // CFCS: command + status
    /*08*/ 0x0c031003, // CFRV: class + revision
    /*0c*/ 0x00000000, // CFLT: latency timer + cache line size
    /*10*/ 0x00000000, // BAR0:
    /*14*/ 0x00000000, // BAR1:
    /*18*/ 0x00000000, // BAR2:
    /*1c*/ 0x00000000, // BAR3:
    /*20*/ 0x00000000, // BAR4:
    /*24*/ 0x00000000, // BAR5:
    /*28*/ 0x00000000, // CCIC: CardBus
    /*2c*/ 0x00000000, // CSID: subsystem + vendor
    /*30*/ 0x00000000, // BAR6: expansion rom base
    /*34*/ 0x00000000, // CCAP: capabilities pointer
    /*38*/ 0x00000000,
    /*3c*/ 0x500001ff, // CFIT: interrupt configuration
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0};

u32 usb_cfg_mask[64] = {
    /*00*/ 0x00000000, // CFID: vendor + device
    /*04*/ 0x00000157, // CFCS: command + status
    /*08*/ 0x00000000, // CFRV: class + revision
    /*0c*/ 0x0000ffff, // CFLT: latency timer + cache line size
    /*10*/ 0xfffff000, // BAR0
    /*14*/ 0x00000000, // BAR1:
    /*18*/ 0x00000000, // BAR2:
    /*1c*/ 0x00000000, // BAR3:
    /*20*/ 0x00000000, // BAR4:
    /*24*/ 0x00000000, // BAR5:
    /*28*/ 0x00000000, // CCIC: CardBus
    /*2c*/ 0x00000000, // CSID: subsystem + vendor
    /*30*/ 0x00000000, // BAR6: expansion rom base
    /*34*/ 0x00000000, // CCAP: capabilities pointer
    /*38*/ 0x00000000,
    /*3c*/ 0x000000ff, // CFIT: interrupt configuration
    /*40*/ 0x04100000, // TM - test mode register
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0};

/**
 * Constructor.
 **/
CAliM1543C_usb::CAliM1543C_usb(CConfigurator *cfg, CSystem *c, int pcibus,
                               int pcidev)
    : CPCIDevice(cfg, c, pcibus, pcidev) {
  add_function(0, usb_cfg_data, usb_cfg_mask);

  ResetPCI();

  state.usb_data[0x34 / 4] = 0x2edf;
  state.usb_data[0x48 / 4] = 0x01000003;

  printf(
      "%s: $Id: AliM1543C_usb.cpp,v 1.6 2008/03/14 15:30:50 iamcamiel Exp $\n",
      devid_string);
}

CAliM1543C_usb::~CAliM1543C_usb() {}
u32 CAliM1543C_usb::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  u32 data = 0;
  switch (bar) {
  case 0:
    data = usb_hci_read(address, dsize);
    break;
  default:
    printf("%%USB-W-READBAR: Bad BAR %d selected.\n", bar);
  }

  return data;
}

void CAliM1543C_usb::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                                  u32 data) {
  switch (bar) {
  case 0:
    usb_hci_write(address, dsize, data);
    break;
  default:
    printf("%%USB-W-WRITEBAR: Bad BAR %d selected.\n", bar);
  }

  return;
}

// AXPBOX_USBTRACE=1: log each OHCI register write with the per-register read
// counts since the previous write, plus a read summary every 20000 reads
// (driver-polling diagnosis).
static const bool g_usbtrace = getenv("AXPBOX_USBTRACE") != nullptr;
static const auto g_usbtrace_t0 = std::chrono::steady_clock::now();
static u64 g_usb_reads[0x110 / 4 + 1];
static u64 g_usb_reads_total;

static void usbtrace_line(const char *what, u64 address, u64 data) {
  char buf[640];
  int len = snprintf(buf, sizeof(buf), "USBT %10.1f %s %03x=%08x reads:",
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - g_usbtrace_t0)
                         .count(),
                     what, (unsigned)address, (unsigned)data);
  for (int i = 0; i <= 0x110 / 4 && len < (int)sizeof(buf) - 24; i++)
    if (g_usb_reads[i]) {
      len += snprintf(buf + len, sizeof(buf) - len, " %03x:%llu", i * 4,
                      (unsigned long long)g_usb_reads[i]);
      g_usb_reads[i] = 0;
    }
  printf("%s\n", buf);
}

u64 CAliM1543C_usb::usb_hci_read(u64 address, int dsize) {
  u64 data = 0;
  if (g_usbtrace && address < 0x110) {
    g_usb_reads[address / 4]++;
    if (++g_usb_reads_total % 20000 == 0)
      usbtrace_line("R", address, state.usb_data[address / 4]);
  }
  if (dsize != 32)
    printf("%%USB-W-HCIREAD: Non dword read, returning 32 bits anyway.\n");
  switch (address) {
  case 0: // HcRevision
    data = 0x00000110;
    break;

  case 0x0c: // HcInterruptStatus: SF is set every frame while operational
    data = state.usb_data[address / 4];
    if (ohci_operational())
      data |= OHCI_INT_SF;
    break;

  case 0x14: // HcInterruptDisable reads back the enable mask
    data = state.usb_data[0x10 / 4];
    break;

  case 0x30: // HcDoneHead: nothing is ever scheduled, so nothing completes
    data = 0;
    break;

  case 0x38: // HcFrameRemaining
    data = state.usb_data[0x34 / 4] & 0x3fff;
    break;

  case 0x3c: // HcFmNumber: advances once per (wall-clock) ms while operational
    data = ohci_operational()
               ? (u32)(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count() &
                       0xffff)
               : state.usb_data[address / 4];
    break;

  case 4:     // HcControl
  case 8:     // HcCommandStatus
  case 0x10:  // HcInterruptEnable
  case 0x18:  // HcHCCA
  case 0x1c:  // HcPeriodCurrentED
  case 0x20:  // HcControlHeadED
  case 0x24:  // HcControlCurrentED
  case 0x28:  // HcBulkHeadED
  case 0x2c:  // HcBulkCurrentED
  case 0x34:  // HcFmInterval
  case 0x40:  // HcPeriodicStart
  case 0x44:  // HcLSThreshold
  case 0x48:  // HcRhDescriptorA
  case 0x4c:  // HcRhDescriptorB
  case 0x50:  // HcRhStatus
  case 0x54:  // HcRhPortStatus1 (power bit only: no device is ever connected)
  case 0x58:  // HcRhPortStatus2
  case 0x5c:  // HcRhPortStatus3
  case 0x100: // HceControlRegister
  case 0x104: // HceInputRegister
  case 0x108: // HceOutputRegister
  case 0x10c: // HceStatusRegister
    data = state.usb_data[address / 4];
    break;

  default:
    printf("%%USB-W-HCIREAD: Reading from unknown address %x.  Ignoring.\n",
           (int)address);
  }

  return data;
}

// HcControl HCFS == UsbOperational (OHCI 1.0a 7.1.2).
bool CAliM1543C_usb::ohci_operational() const {
  return ((state.usb_data[4 / 4] >> 6) & 3) == 2;
}

// Level-sensitive INTA: an enabled status bit with MasterInterruptEnable set.
// SF is synthesized on read only and never drives the line (no frame ticks).
void CAliM1543C_usb::ohci_update_irq() {
  const u32 enable = state.usb_data[0x10 / 4];
  const bool level =
      (enable & OHCI_INT_MIE) &&
      (state.usb_data[0x0c / 4] & enable & ~OHCI_INT_SF & 0x4000007f);
  do_pci_interrupt(0, level);
}

void CAliM1543C_usb::usb_hci_write(u64 address, int dsize, u64 data) {
  if (g_usbtrace)
    usbtrace_line("W", address, data);
  if (dsize != 32)
    printf("%%USB-W-HCIWRITE: Non dword write, writing 32 bits anyway.\n");
  // OHCI 1.0a chapter 7 semantics for a controller with no attached devices:
  // the reset / ownership-change / frame-counter handshakes a host controller
  // driver polls must complete, or the driver busy-waits out its timeouts.
  switch (address) {
  case 4: // HcControl
    state.usb_data[address / 4] = (u32)data & 0x7ff;
    break;

  case 8: // HcCommandStatus: write 1 to set
    if (data & OHCI_CMD_HCR) {
      // Software reset: registers to their defaults (IR and RWC survive),
      // functional state UsbSuspend, and HCR clears when the reset is done --
      // immediately here.
      const u32 keep = state.usb_data[4 / 4] & 0x300;
      for (u32 off = 0x08; off <= 0x44; off += 4)
        state.usb_data[off / 4] = 0;
      state.usb_data[0x34 / 4] = 0x2edf;
      state.usb_data[0x44 / 4] = 0x0628;
      state.usb_data[4 / 4] = keep | 0xc0;
    }
    if (data & OHCI_CMD_OCR) {
      // Ownership change: no SMM firmware owns the controller, so the handoff
      // completes at once -- InterruptRouting clears and OC is reported.
      state.usb_data[4 / 4] &= ~(u32)0x100;
      state.usb_data[0x0c / 4] |= OHCI_INT_OC;
    }
    state.usb_data[8 / 4] |= (u32)data & 0x06; // CLF/BLF; HCR/OCR self-clear
    break;

  case 0x0c: // HcInterruptStatus: write 1 to clear
    state.usb_data[address / 4] &= ~(u32)data;
    break;

  case 0x10: // HcInterruptEnable: write 1 to set
    state.usb_data[0x10 / 4] |= (u32)data & 0xc000007f;
    break;

  case 0x14: // HcInterruptDisable: write 1 to clear the enable bit
    state.usb_data[0x10 / 4] &= ~((u32)data & 0xc000007f);
    break;

  case 0x18: // HcHCCA: the controller needs a 512-byte-aligned block
    state.usb_data[address / 4] = (u32)data & 0xfffffe00;
    break;

  case 0x1c: // HcPeriodCurrentED
  case 0x20: // HcControlHeadED
  case 0x24: // HcControlCurrentED
  case 0x28: // HcBulkHeadED
  case 0x2c: // HcBulkCurrentED
    state.usb_data[address / 4] = (u32)data & ~(u32)0xf;
    break;

  case 0x30: // HcDoneHead, HcFrameRemaining, HcFmNumber: read-only
  case 0x38:
  case 0x3c:
    break;

  case 0x50: // HcRhStatus: only DRWE is stored; power/OCIC writes are no-ops
    state.usb_data[address / 4] = (u32)data & 0x8000;
    break;

  case 0x54: // HcRhPortStatus: SetPortPower / ClearPortPower; change bits
  case 0x58: // write-1-to-clear (none are ever set: nothing connects)
  case 0x5c:
    if (data & 0x100)
      state.usb_data[address / 4] |= 0x100;
    if (data & 0x200)
      state.usb_data[address / 4] &= ~(u32)0x100;
    break;

  case 0x34:  // HcFmInterval
  case 0x40:  // HcPeriodicStart
  case 0x44:  // HcLSThreshold
  case 0x48:  // HcRhDescriptorA
  case 0x4c:  // HcRhDescriptorB
  case 0x100: // HceControlRegister
  case 0x104: // HceInputRegister
  case 0x108: // HceOutputRegister
  case 0x10c: // HceStatusRegister
    state.usb_data[address / 4] = (u32)data;
    break;

  default:
    printf("%%USB-W-HCIWRITE: Writing to unknown address %x.  Ignoring.\n",
           (int)address);
  }
  ohci_update_irq();
}

static u32 usb_magic1 = 0x9000432B;
static u32 usb_magic2 = 0xB2340009;

/**
 * Save state to a Virtual Machine State file.
 **/
int CAliM1543C_usb::SaveState(FILE *f) {
  long ss = sizeof(state);
  int res;

  if ((res = CPCIDevice::SaveState(f)))
    return res;

  fwrite(&usb_magic1, sizeof(u32), 1, f);
  fwrite(&ss, sizeof(long), 1, f);
  fwrite(&state, sizeof(state), 1, f);
  fwrite(&usb_magic2, sizeof(u32), 1, f);
  printf("%s: %d bytes saved.\n", devid_string, (int)ss);
  return 0;
}

/**
 * Restore state from a Virtual Machine State file.
 **/
int CAliM1543C_usb::RestoreState(FILE *f) {
  long ss;
  u32 m1;
  u32 m2;
  int res;
  size_t r;

  if ((res = CPCIDevice::RestoreState(f)))
    return res;

  r = fread(&m1, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m1 != usb_magic1) {
    printf("%s: MAGIC 1 does not match!\n", devid_string);
    return -1;
  }

  r = fread(&ss, sizeof(long), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (ss != sizeof(state)) {
    printf("%s: STRUCT SIZE does not match!\n", devid_string);
    return -1;
  }

  r = fread(&state, sizeof(state), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  r = fread(&m2, sizeof(u32), 1, f);
  if (r != 1) {
    printf("%s: unexpected end of file!\n", devid_string);
    return -1;
  }

  if (m2 != usb_magic2) {
    printf("%s: MAGIC 2 does not match!\n", devid_string);
    return -1;
  }

  printf("%s: %d bytes restored.\n", devid_string, (int)ss);
  return 0;
}
