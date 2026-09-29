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

#if !defined(INCLUDED_EHCI_H)
#define INCLUDED_EHCI_H

#include "DiskController.hpp"
#include "PCIDevice.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class CUsbDevice;

/**
 * \brief A USB 2.0 host controller on a PCI card: EHCI 1.0.
 *
 * The card presents itself as the EHCI function of a NEC uPD720101 (1033:
 * 00e0, class 0C0320), alone: no companion controllers, so only high-speed
 * devices can be used on it (a full- or low-speed device is left for a
 * companion that is not there, as the specification has it). One 256-byte
 * memory BAR; 32-bit data structures; four root hub ports with power
 * switching.
 *
 * The schedule runs on a thread paced in 1 ms frames (eight microframes at
 * a time, FRINDEX counting microframes): each frame the periodic list's
 * entry for the frame (interrupt QHs; isochronous iTDs and siTDs are
 * skipped), then the asynchronous list -- the ring of QHs from
 * ASYNCLISTADDR -- until a pass moves nothing. A device that finishes a
 * transfer on another thread (a host device behind libusb) wakes the thread
 * to go round the asynchronous list again at once, as does the driver
 * turning a schedule on. Queue heads execute their qTDs through the
 * overlay; a qTD finishing with IOC or short sets USBINT, an error sets
 * USBERRINT, and the interrupt-on-async-advance doorbell is answered on the
 * next pass. Interrupts are raised when they occur (the interrupt threshold
 * is a maximum latency, and this is within it).
 *
 * Devices: `port<n> = "host:vvvv:pppp"` (passthrough, high-speed devices)
 * and `disk<n>.0 = file { ... }` (USB mass storage), n = 1 to 4.
 *
 * Documentation consulted: Enhanced Host Controller Interface Specification
 * for Universal Serial Bus, revision 1.0 (Intel, 2002); Universal Serial Bus
 * Specification 2.0, chapters 8, 9 and 11.
 **/
class CEhci : public CPCIDevice, public CDiskController {
public:
  CEhci(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);
  ~CEhci() override;
  int SaveState(FILE *f) override;
  int RestoreState(FILE *f) override;
  void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                    u32 data) override;
  u32 ReadMem_Bar(int func, int bar, u32 address, int dsize) override;
  void start_threads() override;
  void stop_threads() override;
  void check_state() override;
  void register_disk(class CDisk *dsk, int bus, int dev) override;
  /// A machine reset resets the card: halted, schedules off, ports
  /// unpowered, devices back to their default state.
  void ResetPCI() override;

  static constexpr int kPorts = 4;

private:
  u32 reg_read(u32 offset);
  void reg_write(u32 offset, u32 data);
  void reset_controller();
  void status(u32 bits); // set USBSTS bits and update the interrupt line
  void update_irq();

  // The schedule, called with m_mx held.
  void run();
  void frame();
  void async_pass();
  void periodic_frame();
  int service_qh(u32 qh_addr, bool periodic);
  CUsbDevice *device_at(int address);

  void attach(int p, std::unique_ptr<CUsbDevice> dev);

  // Memory the controller reads and writes: through the PCI DMA windows,
  // or -- in the self-test, which runs before any firmware has set them --
  // guest physical addresses as they are.
  void dma_read(u32 a, void *d, size_t size, size_t count);
  void dma_write(u32 a, const void *s, size_t size, size_t count);
  std::atomic_bool m_selftest{false}; // read by the schedule and the CPUs
  std::unique_ptr<std::thread> m_selftest_thread;
  void selftest();
  void kick();
  void port_refresh(int p);
  void port_write(int p, u32 data);

  std::unique_ptr<CUsbDevice> m_dev[kPorts];
  std::mutex m_mx;
  std::unique_ptr<std::thread> myThread;
  std::atomic_bool StopThread{false};
  std::atomic_bool myThreadDead{false};
  std::mutex m_kick_mx;
  std::condition_variable m_kick_cv;
  bool m_kicked = false;
  std::vector<u8> m_xfer; // one qTD's data (up to 20 KB)

  // Operational registers (the capability ones are constant).
  struct SEhci_state {
    u32 usbcmd, usbsts, usbintr, frindex, periodic_base, async_addr;
    u32 configflag;
    u32 portsc[kPorts];
    bool doorbell; // USBCMD.IAAD rung, answered on the next async pass
  } state;

  // USBCMD
  static constexpr u32 CMD_RS = 1u << 0, CMD_HCRESET = 1u << 1,
                       CMD_PSE = 1u << 4, CMD_ASE = 1u << 5, CMD_IAAD = 1u << 6;
  // USBSTS
  static constexpr u32 STS_INT = 1u << 0, STS_ERR = 1u << 1, STS_PCD = 1u << 2,
                       STS_FLR = 1u << 3, STS_HSE = 1u << 4, STS_IAA = 1u << 5,
                       STS_HALTED = 1u << 12, STS_RECL = 1u << 13,
                       STS_PSS = 1u << 14, STS_ASS = 1u << 15;
  // PORTSC
  static constexpr u32 PS_CCS = 1u << 0, PS_CSC = 1u << 1, PS_PED = 1u << 2,
                       PS_PEDC = 1u << 3, PS_OCC = 1u << 5, PS_FPR = 1u << 6,
                       PS_SUSP = 1u << 7, PS_PR = 1u << 8, PS_PP = 1u << 12,
                       PS_OWNER = 1u << 13;
};

#endif
