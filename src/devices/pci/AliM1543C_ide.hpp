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
 * Contains the definitions for the emulated Ali M1543C IDE chipset part.
 **/
#if !defined(INCLUDED_ALIM1543C_IDE_H_)
#define INCLUDED_ALIM1543C_IDE_H_

#include "IdeController.hpp"

/**
 * \brief Emulated IDE part of ALi M1543C multi-function device.
 *
 * The ATA/ATAPI core (CIdeController) as the south bridge's IDE function
 * (10b9:5229, revision c1): its channels in compatibility mode at the
 * legacy ports 1F0h/3F6h and 170h/376h, bus master registers at F000h,
 * interrupts to ISA IRQ 14 and 15 through the bridge's cascaded 8259s; a
 * channel the guest switches to native mode interrupts on PCI INTA.
 *
 * Documentation consulted:
 *  - Ali M1543C B1 South Bridge Version 1.20
 *(http://mds.gotdns.com/sensors/docs/ali/1543dScb1-120.pdf)
 *  .
 **/
class CAliM1543C_ide : public CIdeController {
public:
  CAliM1543C_ide(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);

protected:
  void add_functions() override;
  void irq_raise(int channel) override;
  void irq_lower(int channel) override;
};

extern CAliM1543C_ide *theIDE;

#endif
