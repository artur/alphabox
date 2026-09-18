/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/alphabox
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
 * Symbios 53C8xx family: Interrupt raising, stacking and the IRQ line.
 **/
#include "Disk.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "Sym53C8xx.hpp"
#include "System.hpp"

#include "Sym53C8xxRegs.hpp"

/**
 * Set an interrupt bit.
 *
 * This function checks if any other interrupt bits are active, if so,
 * the interrupt bit is set in the stacked interrupt registers. Otherwise
 * it goes straight to the respective interrupt register.
 *
 * According to the datasheet:
 * "The SYM53C895 stacks interrupts if they occur one after another. If the SIP
 *or DIP bits in the ISTAT register are set (first level), then there is already
 *at least one pending interrupt, and any future interrupts will be stacked in
 *extra registers behind the SIST0, SIST1, and DSTAT registers (second level).
 *When two interrupts have occurred and the two levels of the stack are full,
 *any further interrupts will set additional bits in the extra registers behind
 *SIST0, SIST1 and DSTAT. When the first level of interrupts are cleared, all
 *the interrupts that came in afterward will move into the SIST0, SIST1 and
 *DSTAT. After the first interrupt is cleared by reading the appropriate
 *register, the IRQ/ pin will be deasserted for a minimum of three CLKs; the
 *stacked interrupt(s) will move into the SIST0, SIST1 or DSTAT; and the IRQ/
 *pin will be asserted once again.
 *
 * Since a masked non-fatal interrupt will not set the SIP or DIP bits,
 *interrupt stacking will not occur. A masked, non-fatal interrupt will still
 *post the interrupt in SIST0, but will not assert the IRQ/ pin. Since no
 *interrupt is generated, future interrupts will move right into the SIST0 or
 *SIST1 instead of being stacked behind another interrupt. When another
 *condition occurs that generates an interrupt, the bit corresponding to the
 *earlier masked non-fatal interrupt will still be set."
 **/
void CSym53C8xx::CChannel::set_interrupt(int reg, u8 interrupt) {
  // printf("set interrupt %02x, %02x.\n",reg,interrupt);
  switch (reg) {
  case R_DSTAT:
    if (TB_R8(ISTAT, DIP) || TB_R8(ISTAT, SIP)) {
      state.dstat_stack |= interrupt;

      // printf("DSTAT stacked.\n");
    } else {
      R8(DSTAT) |= interrupt;

      // printf("DSTAT.\n");
    }
    break;

  case R_SIST0:
    if (TB_R8(ISTAT, DIP) || TB_R8(ISTAT, SIP)) {
      state.sist0_stack |= interrupt;

      // printf("SIST0 stacked.\n");
    } else {
      R8(SIST0) |= interrupt;

      // printf("SIST0.\n");
    }
    break;

  case R_SIST1:
    if (TB_R8(ISTAT, DIP) || TB_R8(ISTAT, SIP)) {
      state.sist1_stack |= interrupt;

      // printf("SIST1 stacked.\n");
    } else {
      R8(SIST1) |= interrupt;

      // printf("SIST1.\n");
    }
    break;

  case R_ISTAT:

    // printf("ISTAT.\n");
    R8(ISTAT) |= interrupt;
    break;

  default:
    FAILURE_1(NotImplemented, "set_interrupt reg %02x!!\n", reg);
  }

  // printf("--> eval int\n");
  eval_interrupts();

  // printf("<-- eval_int\n");
}

/**
 * Evaluate interrupt status.
 *
 * Check interrupt registers, and determine if an interrupt should be generated.
 **/
void CSym53C8xx::CChannel::eval_interrupts() {
  // will_assert: when this boolean value is true at the end of this function,
  // an interrupt will be signalled to the system.
  bool will_assert = false;

  // will_halt: when this boolean value is true at the end of this function,
  // program execution will be halted. (fatal interrupt)
  bool will_halt = false;

  // Check current interrupt status. If no interrupt is active, move interrupt
  // flags from the interrupt stack down.
  //
  // (When an interrupt is signalled, but another interrupt bit is already
  // active, the interrupt doesn't go to the interrupt register, but to the
  // interrupt stack. The interrupt stack, however, doesn't keep track of the
  // order in which interrupts come in, so when the interrupt stack is moved
  // down into the interrupt registers, multiple interrupt bits may become
  // active.) Mask DFE (DSTAT bit 7, an always-set status bit) so this stacked-
  // interrupt drain isn't dead code -- DSTAT is never 0 otherwise.
  if (!R8(SIST0) && !R8(SIST1) && !(R8(DSTAT) & DSTAT_RC)) {
    R8(SIST0) |= state.sist0_stack;
    R8(SIST1) |= state.sist1_stack;
    R8(DSTAT) |= state.dstat_stack;
    state.sist0_stack = 0;
    state.sist1_stack = 0;
    state.dstat_stack = 0;
  }

  // Check for DMA interrupts.
  if (R8(DSTAT) & DSTAT_FATAL) {
    // DMA interrupt conditions always halt execution (always fatal)
    will_halt = true;

    // printf("  will halt(DSTAT).\n");

    // Set the DMA interrupt pending bit.
    SB_R8(ISTAT, DIP, true);

    // If the interrupt is also enabled in the DIEN register, it will
    // be signalled to the system.
    if (R8(DSTAT) & R8(DIEN) & DSTAT_FATAL) {
      will_assert = true;

      // printf("  will assert(DSTAT).\n");
    }
  } else {
    // Reset the DMA interrupt pending bit. (It may still be set).
    SB_R8(ISTAT, DIP, false);
  }

  // Check for SCSI engine interrupts.
  //
  // Per the SYM53C810A data manual (Interrupt Handling / Masking): a
  // masked non-fatal interrupt (CMP/SEL/RSL/GEN/HTH in initiator role)
  // posts its bit in SIST0/SIST1 but must NOT set SIP, assert IRQ/, or
  // cause interrupt stacking. Only a fatal interrupt -- or a non-fatal one
  // enabled in SIEN0/SIEN1 -- sets SIP. Gating SIP on the raw SIST bits
  // latched SIP on a masked GEN timer, after which every later completion
  // interrupt stacked behind it and was never delivered to the host.
  if ((R8(SIST0) & (SIST0_FATAL | R8(SIEN0))) ||
      (R8(SIST1) & (SIST1_FATAL | R8(SIEN1)))) {
    // Set the SCSI interrupt pending bit.
    SB_R8(ISTAT, SIP, true);

    // Check if the interrupt is either fatal, or enabled.
    if ((R8(SIST0) & (SIST0_FATAL | R8(SIEN0))) ||
        (R8(SIST1) & (SIST1_FATAL | R8(SIEN1)))) {
      // In either case, stop execution.
      will_halt = true;

      // printf("  will halt(SIST).\n");

      // If the interrupt is enabled, signal it to the system.
      if ((R8(SIST0) & R8(SIEN0)) || (R8(SIST1) & R8(SIEN1))) {
        will_assert = true;

        // printf("  will assert(SIST).\n");
      }
    }
  } else {
    // Reset the SCSI interrupt pending bit (It may still be set).
    SB_R8(ISTAT, SIP, false);
  }

  // Check the interrupt on the fly bit.
  if (TB_R8(ISTAT, INTF)) {
    // Signal this to the system
    will_assert = true;

    // printf("  will assert(INTF).\n");
  }

  // If interrupts are disabled, don't signal any interrupt to the system.
  // DCNTL IRQD may only be written while SCRIPTS are stopped; the 896's
  // SIRQD does the same from a register the host may touch at any time,
  // and each channel's bit masks only its own pin.
  if (TB_R8(DCNTL, IRQD) || (m_chip.reg_bytes > 128 && TB_R8(ISTAT1, SIRQD))) {
    will_assert = false;

    // printf("  won't assert(IRQD).\n");
  }

  // Halt execution if will_halt is true.
  if (will_halt)
    state.executing = false;

  // Assert or de-assert the interrupt line as needed
  if (will_assert != state.irq_asserted) {

    // printf("  doing...%d\n",will_assert);
    dev.do_pci_interrupt(index, will_assert);
    state.irq_asserted = will_assert;
  }
}
