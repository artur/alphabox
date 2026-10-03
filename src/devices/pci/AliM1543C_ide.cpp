/* Alphabox Alpha Emulator
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

/**
 * \file
 * Contains the code for the emulated Ali M1543C IDE chipset part.
 **/
#include "AliM1543C_ide.hpp"
#include "StdAfx.hpp"

#include "AliM1543C.hpp"

u32 AliM1543C_ide_cfg_data[64] = {
    /*00*/ 0x522910b9, // CFID: vendor + device
    /*04*/ 0x02800000, // CFCS: command + status
    /*08*/ 0x0101fac1, // CFRV: class + revision
    /*0c*/ 0x00000000, // CFLT: latency timer + cache line size
    /*10*/ 0x000001f1, // BAR0:
    /*14*/ 0x000003f5, // BAR1:
    /*18*/ 0x00000171, // BAR2:
    /*1c*/ 0x00000375, // BAR3:
    /*20*/ 0x0000f001, // BAR4:
    /*24*/ 0x00000000, // BAR5:
    /*28*/ 0x00000000, // CCIC: CardBus
    /*2c*/ 0x00000000, // CSID: subsystem + vendor
    /*30*/ 0x00000000, // BAR6: expansion rom base
    /*34*/ 0x00000000, // CCAP: capabilities pointer
    /*38*/ 0x00000000,
    /*3c*/ 0x040201ff, // CFIT: interrupt configuration
    0,
    0,
    /*48*/ 0x4a000000, // UDMA test
    /*4c*/ 0x1aba0000, // reserved
    0,
    /*54*/ 0x44445555, // udma setting + fifo treshold
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    /*78*/ 0x00000021, // ide clock
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

u32 AliM1543C_ide_cfg_mask[64] = {
    /*00*/ 0x00000000, // CFID: vendor + device
    /*04*/ 0x00000105, // CFCS: command + status
    /*08*/ 0x00000000, // CFRV: class + revision
    /*0c*/ 0x0000ffff, // CFLT: latency timer + cache line size
    /*10*/ 0xfffffff8, // BAR0
    /*14*/ 0xfffffffc, // BAR1: CBMA
    /*18*/ 0xfffffff8, // BAR2:
    /*1c*/ 0xfffffffc, // BAR3:
    /*20*/ 0xfffffff0, // BAR4:
    /*24*/ 0x00000000, // BAR5:
    /*28*/ 0x00000000, // CCIC: CardBus
    /*2c*/ 0x00000000, // CSID: subsystem + vendor
    /*30*/ 0x00000000, // BAR6: expansion rom base
    /*34*/ 0x00000000, // CCAP: capabilities pointer
    /*38*/ 0x00000000,
    /*3c*/ 0x000000ff, // CFIT: interrupt configuration
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

CAliM1543C_ide *theIDE = 0;

CAliM1543C_ide::CAliM1543C_ide(CConfigurator *cfg, CSystem *c, int pcibus,
                               int pcidev)
    : CIdeController(cfg, c, pcibus, pcidev) {
  if (theIDE != 0)
    FAILURE(Configuration, "More than one IDE controller");
  theIDE = this;
}

void CAliM1543C_ide::add_functions() {
  add_function(0, AliM1543C_ide_cfg_data, AliM1543C_ide_cfg_mask);

  add_legacy_io(PRI_COMMAND, 0x1f0, 8);
  // In compatibility mode the control block is the
  // alternate-status/device-control port at 3F6h; 3F7h remains available to the
  // legacy floppy path.
  add_legacy_io(PRI_CONTROL, 0x3f6, 1);
  add_legacy_io(SEC_COMMAND, 0x170, 8);
  add_legacy_io(SEC_CONTROL, 0x376, 1);
  add_legacy_io(PRI_BUSMASTER, 0xf000, 8);
  add_legacy_io(SEC_BUSMASTER, 0xf008, 8);
}

// Compat mode:  ISA IRQ 14/15 via 8259 cascade -- no PCI INTx.
// Native mode:  shared PCI INTA -- no 8259 IRQ.
void CAliM1543C_ide::irq_raise(int index) {
  if (channel_is_native(index))
    do_pci_interrupt(0, true);
  else
    theAli->pic_interrupt(1, 6 + index);
}

void CAliM1543C_ide::irq_lower(int index) {
  if (channel_is_native(index)) {
    // PCI INTA is shared between both channels; only drop the line
    // when neither channel still has work pending.
    if (!irq_pending(0) && !irq_pending(1))
      do_pci_interrupt(0, false);
  } else {
    theAli->pic_deassert(1, 6 + index);
  }
}
