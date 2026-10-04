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
 * The AlphaServer ES47 board (docs/platforms/marvel.md): two EV7s on one
 * module, and one IO7 on PID 0's I/O port. Also what every Marvel board
 * shares (marvel_board_devices, marvel_pci_interrupt, marvel_slot_refusal):
 * the ES80 and the GS1280 are the same platform with more processors and
 * more IO7s (platforms/es80/, platforms/gs1280/).
 *
 * The module's management processor, the CMM, is Cmm.hpp. The IO7
 * (chipsets/marvel/Io7.hpp) gives PID 0 hoses 0-3: on a real ES47 hose 0 is
 * a one-slot 66 MHz PCI-X bus, hoses 1 and 2 two-slot buses, hose 2 also
 * carries the I/O expander module's SCSI, IDE and USB controllers in slots
 * 1-3, and hose 3 is the AGP slot (slot 5). Sources: the User Information
 * (2P backplane, 2P I/O expander module) and a real ES47's show config
 * (test/platforms/es47/show-config.txt).
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"
#include "Cmm.hpp"
#include "Configurator.hpp"
#include "Io7.hpp"
#include "Marvel.hpp"
#include "System.hpp"
#include "Topology.hpp"

namespace {

/// One 2P drawer: the module's two processors at NS 0 and 1 (a real ES47's
/// show config: "NS,EW (0,0)", "(1,0)").
void es47_coordinates(int index, u8 *ns, u8 *ew) {
  *ns = (u8)index;
  *ew = 0;
}

/// The drawer's one IO7, on PID 0's I/O port: PID 1 has "No Local I/O".
bool es47_has_io7(u32 pid) { return pid == 0; }

/// I/O Drawer 0: a real ES47's show config.
u8 es47_io_drawer(u32) { return 0; }

CMarvel *marvel_of(CSystem *sys) {
  return sys ? dynamic_cast<CMarvel *>(sys->chipset()) : nullptr;
}

} // namespace

/// Backplane revision 2, embedded I/O: a real ES47's show config.
const marvel_layout es47_layout = {0x11, es47_coordinates,     es47_has_io7,
                                   2,    io7::kIoTypeEmbedded, es47_io_drawer};

/**
 * Every Marvel board: the CMMs answer every processor's GIO port (Cmm.hpp),
 * and an IO7 hangs on each processor the board row's layout cables one to.
 */
void marvel_board_devices(CConfigurator *cfg, CSystem *sys) {
  CMarvel *marvel = marvel_of(sys);
  if (!marvel)
    FAILURE(Configuration, "a Marvel board needs the Marvel chipset");
  marvel->set_management(std::unique_ptr<GioManagement>(
      new CEs47Cmm(sys, cfg->get_text_value("rom.nvram", "cmm_nvram.bin"))));
  const CMarvelTopology &t = marvel->topology();
  const marvel_layout *layout = sys->platform().marvel;
  for (int i = 0; i < t.count(); i++)
    if (t.has_io7(t.node(i).pid))
      marvel->attach_io7(t.node(i).pid, layout ? layout->io_backplane_rev : 0,
                         layout ? layout->io_type : io7::kIoTypeEmbedded);
}

void es47_board_devices(CConfigurator *cfg, CSystem *sys) {
  marvel_board_devices(cfg, sys);
}

/**
 * Every slot's INTx is an LSI of the IO7 the hose belongs to: port <7:5>,
 * slot <4:2>, INTx <1:0> (Linux core_marvel.h). The chipset's input number
 * is the IO7's PID << 8 | the LSI (CMarvel::interrupt). Hose h is PID h / 4,
 * port h % 4.
 *
 * Except on port 2 of an IO7 with embedded I/O, where slots 2 and 3 (the
 * real ES47's IDE and USB controllers) have no lines of their own and use
 * slot 1's spare ones: slot 2 INTB -- its function 1 INTD --, slot 3 INTC
 * for every function, whatever pins the functions name. That is what the
 * console writes into their interrupt line registers (the routine at
 * 0x2eab70 in SRM V7.3-1, for a device directly on the hose: the I/O type
 * from IO_SYS_REV<7:4>, port 2, then slot 2 -> slot 1 with pin 4 for
 * function 1 and 2 otherwise, slot 3 -> slot 1 pin 3), and OpenVMS enables
 * the LSI the line names. Probed: a card in hose 2 slot 2 gets 0x45, the
 * NEC USB's three functions in slot 3 all 0x46, slots 4 and 5 0x50, 0x54.
 * On the ES47 these are the I/O expander's controllers: the AIC-7892 (here
 * a 53C895) on 0x44, the CMD 649 on 0x45 (OpenVMS's DQDRIVER enables it),
 * the USS-344's four OHCI functions all on 0x46.
 *
 * The I/O type is the board row's (marvel_layout::io_type): the ES47's and
 * ES80's 2P drawers have embedded I/O, the GS1280's I/O drawers do not, and
 * there the console gives slots 2 and 3 their own lines. Behind a PCI-PCI
 * bridge (func -1) the console swizzles onto the bridge's slot with no
 * remap, which is CPCIDevice::interrupt_input's rotation. Checked against
 * the console's writes on every hose of an ES80 and on a GS1280
 * (docs/platforms/marvel.md, M7c).
 */
int marvel_pci_interrupt(int hose, int slot, int intx, int func) {
  if (hose < 0 || hose >= 4 * 256 || slot < 0 || slot > 7)
    return -1;
  const u32 port = (u32)hose % 4;
  if (port == 2 && func >= 0 && (slot == 2 || slot == 3)) {
    CMarvel *marvel = marvel_of(theSystem);
    const CIo7 *io7 = marvel ? marvel->io7((u32)hose / 4) : nullptr;
    if (io7 && io7->io_type() == io7::kIoTypeEmbedded) {
      intx = slot == 3 ? 2 : (func == 1 ? 3 : 1);
      slot = 1;
    }
  }
  return ((hose / 4) << 8) | (int)io7::lsi(port, slot, intx);
}

/// A hose exists where the board has an IO7 (hose PID * 4 + port), and an
/// LSI names the slot in three bits, so a slot is 1 to 7.
const char *marvel_slot_refusal(int hose, int slot) {
  CMarvel *marvel = marvel_of(theSystem);
  const u32 pid = (u32)hose / 4;
  if (hose < 0 || !marvel ||
      !marvel->topology().by_pid(pid, marvel->topology().count()) ||
      !marvel->topology().has_io7(pid))
    return "there is no IO7 for that hose: hose n is IO7 port n % 4 of the "
           "processor with PID n / 4 (show config lists the IO7s)";
  if (slot < 1 || slot > 7)
    return "an IO7 slot is 1 to 7 (its interrupt number has three bits for "
           "the slot)";
  return nullptr;
}

/// The ES47's hoses are PID 0's IO7 ports; the I/O expander's slots on
/// hose 2 are marvel_pci_interrupt's embedded-I/O rule.
int es47_pci_interrupt(int hose, int slot, int intx, int func) {
  if (hose < 0 || hose > 3)
    return -1;
  return marvel_pci_interrupt(hose, slot, intx, func);
}

const char *es47_slot_refusal(int hose, int slot) {
  if (hose < 0 || hose > 3)
    return "the ES47's IO7 has hoses 0-3 (PCI-X 0-2, AGP 3)";
  return marvel_slot_refusal(hose, slot);
}
