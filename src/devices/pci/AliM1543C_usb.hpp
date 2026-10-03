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
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "UsbDevice.hpp"

/**
 * \brief What an OHCI core needs from the PCI function it is: memory it
 * reaches as a bus master, and its interrupt line.
 **/
class COhciHost {
public:
  virtual ~COhciHost() = default;
  virtual void ohci_dma_read(u32 address, void *dest, size_t size,
                             size_t count) = 0;
  virtual void ohci_dma_write(u32 address, void *source, size_t size,
                              size_t count) = 0;
  /// The interrupt line's level; called with the core's lock held.
  virtual void ohci_irq(bool level) = 0;
};

/**
 * \brief An OHCI 1.0a host controller, whichever PCI function it is.
 *
 * Registers, a 1 ms frame thread, and the schedule: each frame the
 * controller walks the periodic (interrupt) list the HCCA points at for
 * that frame, then the control and bulk lists, running every general TD
 * whose endpoint is not halted or skipped against the device at its address,
 * and hands finished TDs back through the done queue. The root hub has one
 * to 15 ports; a device is plugged into a port (CUsbPort) by the function
 * that owns the ports -- the ALi's USB function, or the EHCI card whose
 * companion this is, which may take a port away and give it back
 * (port_routed()). Isochronous endpoints move one packet a frame from
 * their TDs' eight-packet buffers (service_iso_ed).
 *
 * A saved state holds the registers (SUSB_state) and, in the port owner's
 * addition to it, the frame number and the done queue (save_runtime).
 *
 * Documentation consulted:
 *  - OpenHCI Open Host Controller Interface Specification for USB, 1.0a
 *  .
 **/
class COhci {
public:
  /// `legacy`: the controller has the legacy support registers (0x100 to
  /// 0x10c, HcRevision bit 8). `name` is what start/stop_threads print.
  COhci(COhciHost &host, int ports, bool legacy, const char *name);
  ~COhci();
  void bind_port(int p, CUsbPort *port) { m_port[p] = port; }

  u64 usb_hci_read(u64 address, int dsize);
  void usb_hci_write(u64 address, int dsize, u64 data);
  /// Power-on state: UsbReset, registers at their defaults, ports
  /// unpowered, the interrupt line low. The devices are the port owner's
  /// to reset.
  void reset();
  void start_threads();
  void stop_threads();
  bool thread_dead() const { return myThreadDead.load(); }
  /// Wake the frame thread to walk the control and bulk lists at once.
  void kick();
  bool inject_fault(const char *op, int port, int ep, int arg);
  /// A port was given to this controller (`owns`) or taken from it: the
  /// root hub sees the connection change. Takes the controller's lock, so
  /// no transfer to the port's device is under way when it returns.
  void port_routed(int p, bool owns);
  /// After a restored state: a device on the port is shown as reconnected.
  void reconnect(int p);
  /// A saved machine: the schedule's state beyond the registers -- the
  /// frame number, and TDs retired but not yet written back to the done
  /// queue. Without them a restored guest would lose those TDs, and see
  /// its frame counter jump back.
  void save_runtime(CUsbSaved &s) const;
  void load_runtime(CUsbSaved &s);
  /// The interrupt line again, from the restored registers.
  void refresh_irq();
  std::mutex &mutex() { return m_mx; }

  /// The state structure contains all elements that need to be saved to the
  /// statefile.
  struct SUSB_state {
    u32 usb_data[0x110 / 4];
  } state;

private:
  bool ohci_operational() const;
  void ohci_update_irq();
  void ohci_status(u32 bits); // set HcInterruptStatus bits, update the line

  // Bus-master memory access: the host function's.
  void do_pci_read(u32 a, void *d, size_t size, size_t count) {
    m_host.ohci_dma_read(a, d, size, count);
  }
  void do_pci_write(u32 a, void *s, size_t size, size_t count) {
    m_host.ohci_dma_write(a, s, size, count);
  }

  // The schedule, called with m_mx held.
  void run();
  void frame();
  bool service_list(u32 head);
  void service_async_lists(); // control and bulk, between frames
  int service_ed(u32 ed_addr, bool periodic, bool *found = nullptr);
  int service_iso_ed(u32 ed_addr, u32 ed[4]);
  void retire_td(u32 td_addr, u32 td[4], int cc);
  void retire_iso_td(u32 td_addr, u32 td[8], int cc);
  void write_back_done();
  CUsbDevice *device_at(int address);

  // Root hub.
  static constexpr int kMaxPorts = 15;
  u32 &port_reg(int p) { return state.usb_data[0x54 / 4 + p]; }
  // The device on port p while this controller has the port, else null.
  CUsbDevice *port_dev(int p) const {
    return m_port[p] && m_port[p]->ohci_owns ? m_port[p]->dev.get() : nullptr;
  }
  void port_refresh(int p); // CCS/LSDA from attachment and power
  void port_write(int p, u32 data);
  void port_change(int p, u32 bits);
  CUsbPortFaults *faults_for(const CUsbDevice *d) {
    for (int p = 0; p < m_ports; ++p)
      if (m_port[p] && m_port[p]->dev.get() == d)
        return &m_port[p]->faults;
    return nullptr;
  }

  COhciHost &m_host;
  const int m_ports;
  const bool m_legacy;
  const char *m_name;
  // ALPHABOX_USBTRACE: register reads counted between trace lines.
  void usbtrace_line(const char *what, u64 address, u64 data);
  u64 m_trace_reads[0x110 / 4 + 1] = {};
  u64 m_trace_reads_total = 0;
  CUsbPort *m_port[kMaxPorts] = {};

  // A device finishing a transfer on another thread wakes the frame thread,
  // which retries the control and bulk lists at once (kick()).
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
  // Frames the thread overslept that are still run (see run()).
  static constexpr int kCatchUpFrames = 32;
  u32 m_done_head = 0;  // TDs retired and not yet written back
  int m_done_delay = 7; // frames until the done queue interrupts; 7 = none
  // The done queue holds a control-endpoint TD, and may not go back before
  // this time (see write_back_early).
  bool m_done_hold = false;
  std::chrono::steady_clock::time_point m_done_hold_until;
  bool write_back_early() const;

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
};

/**
 * \brief The USB function of the ALi M1543C: an OHCI 1.0a host controller
 * (COhci) with three root hub ports.
 *
 * A device is attached to a port from the configuration (`port1 =
 * "tablet";`, or `disk<port>.0` for USB mass storage). The function's
 * interrupt leaves through the bridge's USBIR routing byte to an ISA IRQ.
 *
 * Documentation consulted:
 *  - Ali M1543C B1 South Bridge Version 1.20
 *    (http://mds.gotdns.com/sensors/docs/ali/1543dScb1-120.pdf)
 *  .
 **/
class CAliM1543C_usb : public CPCIDevice,
                       public CDiskController,
                       public CUsbFaultTarget,
                       public COhciHost {
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
  /// A machine reset resets the controller (UsbReset, registers at their
  /// defaults, ports unpowered) and its devices.
  void ResetPCI() override;
  bool inject_fault(const char *op, int port, int ep, int arg) override;

  void ohci_dma_read(u32 a, void *d, size_t size, size_t count) override {
    do_pci_read(a, d, size, count);
  }
  void ohci_dma_write(u32 a, void *s, size_t size, size_t count) override {
    do_pci_write(a, s, size, count);
  }
  void ohci_irq(bool level) override;

private:
  static constexpr int kPorts = 3;
  CUsbPort m_port[kPorts];
  COhci m_ohci;
  void attach(int p, std::unique_ptr<CUsbDevice> dev);

  // The ISA IRQ the interrupt line is on, and its level.
  int m_irq = -1;
  bool m_irq_level = false;
};
#endif // !defined(INCLUDED_ALIM1543C_USB_H)
