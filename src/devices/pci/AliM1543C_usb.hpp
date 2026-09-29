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

#if !defined(INCLUDED_ALIM1543C_USB_H_)
#define INCLUDED_ALIM1543C_USB_H_

#include "DiskController.hpp"
#include "PCIDevice.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

class CUsbDevice;

/**
 * \brief The USB function of the ALi M1543C: an OHCI 1.0a host controller.
 *
 * Registers, a 1 ms frame thread, and the schedule: each frame the
 * controller walks the periodic (interrupt) list the HCCA points at for
 * that frame, then the control and bulk lists, running every general TD
 * whose endpoint is not halted or skipped against the device at its address,
 * and hands finished TDs back through the done queue. The root hub has three
 * ports; a device is attached to one from the configuration
 * (`port1 = "tablet";`). Isochronous TDs are not implemented: they are
 * retired as not accessed.
 *
 * Runtime state beyond the registers (the devices' addresses and
 * configuration, the done queue) is not part of a saved state yet.
 *
 * Documentation consulted:
 *  - OpenHCI Open Host Controller Interface Specification for USB, 1.0a
 *  - Ali M1543C B1 South Bridge Version 1.20
 *    (http://mds.gotdns.com/sensors/docs/ali/1543dScb1-120.pdf)
 *  .
 **/
class CAliM1543C_usb : public CPCIDevice, public CDiskController {
public:
  virtual int SaveState(FILE *f);
  virtual int RestoreState(FILE *f);

  CAliM1543C_usb(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);
  virtual ~CAliM1543C_usb();
  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data);
  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize);
  void start_threads() override;
  void stop_threads() override;
  void check_state() override;
  /// A disk declared as disk<port>.0 plugs a USB mass storage device into
  /// that root hub port (1 to 3).
  void register_disk(class CDisk *dsk, int bus, int dev) override;

private:
  u64 usb_hci_read(u64 address, int dsize);
  void usb_hci_write(u64 address, int dsize, u64 data);
  bool ohci_operational() const;
  void ohci_update_irq();
  void ohci_status(u32 bits); // set HcInterruptStatus bits, update the line

  // The schedule, called with m_mx held.
  void run();
  void frame();
  bool service_list(u32 head);
  void service_async_lists(); // control and bulk, between frames
  int service_ed(u32 ed_addr, bool periodic, bool *found = nullptr);
  void retire_td(u32 td_addr, u32 td[4], int cc);
  void write_back_done();
  CUsbDevice *device_at(int address);

  // Root hub.
  static constexpr int kPorts = 3;
  u32 &port_reg(int p) { return state.usb_data[0x54 / 4 + p]; }
  void port_refresh(int p); // CCS/LSDA from attachment and power
  void port_write(int p, u32 data);
  void port_change(int p, u32 bits);
  std::unique_ptr<CUsbDevice> m_dev[kPorts];
  void attach(int p, std::unique_ptr<CUsbDevice> dev);

  // A device finishing a transfer on another thread wakes the frame thread,
  // which retries the control and bulk lists at once (kick()).
  void kick();
  std::mutex m_kick_mx;
  std::condition_variable m_kick_cv;
  bool m_kicked = false;
  // A pass between frames that retires TDs wanting an interrupt at once
  // (DI=0) writes the done queue back then, not at the frame's end.
  // Consecutive bulk TDs of an endpoint are moved as one transfer (see
  // service_ed), in this buffer:
  static constexpr int kMaxRun = 0x10000; // bytes in one coalesced transfer
  std::vector<u8> m_xfer;

  std::mutex m_mx; // registers and schedule: MMIO thread vs frame thread
  std::unique_ptr<std::thread> myThread;
  std::atomic_bool StopThread{false};
  std::atomic_bool myThreadDead{false};
  u32 m_frame = 0;      // HcFmNumber
  u32 m_done_head = 0;  // TDs retired and not yet written back
  int m_done_delay = 7; // frames until the done queue interrupts; 7 = none
  // The ISA IRQ the interrupt line is on, and its level.
  int m_irq = -1;
  bool m_irq_level = false;

  // OHCI 1.0a register bits
  static constexpr u32 OHCI_CTL_PLE = 0x00000004; // PeriodicListEnable
  static constexpr u32 OHCI_CTL_IE = 0x00000008;  // IsochronousEnable
  static constexpr u32 OHCI_CTL_CLE = 0x00000010; // ControlListEnable
  static constexpr u32 OHCI_CTL_BLE = 0x00000020; // BulkListEnable
  static constexpr u32 OHCI_CMD_HCR = 0x00000001; // HostControllerReset
  static constexpr u32 OHCI_CMD_CLF = 0x00000002; // ControlListFilled
  static constexpr u32 OHCI_CMD_BLF = 0x00000004; // BulkListFilled
  static constexpr u32 OHCI_CMD_OCR = 0x00000008; // OwnershipChangeRequest
  static constexpr u32 OHCI_INT_WDH = 0x00000002; // WritebackDoneHead
  static constexpr u32 OHCI_INT_SF = 0x00000004;  // StartofFrame
  static constexpr u32 OHCI_INT_FNO = 0x00000020; // FrameNumberOverflow
  static constexpr u32 OHCI_INT_RHSC = 0x00000040; // RootHubStatusChange
  static constexpr u32 OHCI_INT_OC = 0x40000000;  // OwnershipChange
  static constexpr u32 OHCI_INT_MIE = 0x80000000; // MasterInterruptEnable
  // HcRhPortStatus
  static constexpr u32 RH_CCS = 1u << 0, RH_PES = 1u << 1, RH_PSS = 1u << 2,
                       RH_PRS = 1u << 4, RH_PPS = 1u << 8, RH_LSDA = 1u << 9,
                       RH_CSC = 1u << 16, RH_PESC = 1u << 17,
                       RH_PSSC = 1u << 18, RH_PRSC = 1u << 20;

  /// The state structure contains all elements that need to be saved to the
  /// statefile.
  struct SUSB_state {
    u32 usb_data[0x110 / 4];
  } state;
};
#endif // !defined(INCLUDED_ALIM1543C_USB_H)
