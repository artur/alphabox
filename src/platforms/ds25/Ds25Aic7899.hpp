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
 * The DS25's on-board Adaptec AIC-7899 at hose 2 device 1, as far as the
 * console needs it to be there: its PCI configuration space and expansion
 * ROM, no SCSI (docs/platforms/ds25.md, Findings).
 **/
#if !defined(INCLUDED_DS25_AIC7899_H_)
#define INCLUDED_DS25_AIC7899_H_

#include "PCIDevice.hpp"

class CDs25Aic7899 : public CPCIDevice {
public:
  CDs25Aic7899(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);
  void init() override;
  u32 ReadMem_Bar(int func, int bar, u32 address, int dsize) override;
  void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                    u32 data) override;
};

#endif // !defined(INCLUDED_DS25_AIC7899_H_)
