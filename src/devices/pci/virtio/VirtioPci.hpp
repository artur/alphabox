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

#if !defined(INCLUDED_VIRTIO_PCI_H)
#define INCLUDED_VIRTIO_PCI_H

#include "PCIDevice.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/**
 * \brief A paravirtual virtio device on PCI, legacy interface.
 *
 * The transport of the OASIS Virtual I/O Device specification 1.0,
 * sections "Legacy Interface" (4.1.4.8 and 2.4.2) -- the virtio 0.9.5
 * register layout: vendor 1AF4, device 1000 + type - 1, revision 0, one
 * I/O BAR holding
 *
 *   0x00 u32 host features      0x0c u16 queue size (RO)
 *   0x04 u32 guest features     0x0e u16 queue select
 *   0x08 u32 queue PFN (4 KB)   0x10 u16 queue notify
 *   0x12 u8  device status      0x13 u8  ISR status (read clears)
 *   0x14 ...  device-specific configuration (no MSI-X)
 *
 * all little-endian. Split virtqueues of 256 entries in the legacy layout
 * (descriptors, available ring, then the used ring at the next 4 KB
 * boundary), indirect descriptors offered. A notify wakes the device's
 * thread, which does the work (disk and network I/O never run on a CPU
 * thread), writes the used ring and raises INTA; the ISR read clears the
 * interrupt. Writing 0 to the status register resets the device.
 *
 * Every address the driver hands over -- queue PFN and descriptors -- is a
 * PCI bus address (on Alpha, a logical address through the chipset's DMA
 * windows), reached with do_pci_read/do_pci_write. docs/virtio.md is the
 * driver writer's reference.
 **/
class CVirtioPci : public CPCIDevice {
public:
  CVirtioPci(class CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev,
             u16 type, u32 class_code, int queues, const char *name);
  ~CVirtioPci() override;
  u32 ReadMem_Bar(int func, int bar, u32 address, int dsize) override;
  void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                    u32 data) override;
  void ResetPCI() override;
  void start_threads() override;
  void stop_threads() override;
  void check_state() override;
  int SaveState(FILE *f) override;
  int RestoreState(FILE *f) override;

  static constexpr int kQueueSize = 256;
  static constexpr u32 kPage = 4096; // legacy PFN unit and used-ring align

  // Common registers (legacy layout, no MSI-X).
  enum : u32 {
    REG_HOST_FEATURES = 0x00,
    REG_GUEST_FEATURES = 0x04,
    REG_QUEUE_PFN = 0x08,
    REG_QUEUE_SIZE = 0x0c,
    REG_QUEUE_SEL = 0x0e,
    REG_QUEUE_NOTIFY = 0x10,
    REG_STATUS = 0x12,
    REG_ISR = 0x13,
    REG_CONFIG = 0x14,
  };
  // Device status bits.
  enum : u8 {
    S_ACKNOWLEDGE = 1,
    S_DRIVER = 2,
    S_DRIVER_OK = 4,
    S_FEATURES_OK = 8,
    S_FAILED = 0x80,
  };
  // Transport feature bits.
  static constexpr u32 F_NOTIFY_ON_EMPTY = 1u << 24;
  static constexpr u32 F_ANY_LAYOUT = 1u << 27;
  static constexpr u32 F_INDIRECT_DESC = 1u << 28;
  static constexpr u32 F_EVENT_IDX = 1u << 29;
  // Descriptor and ring flags.
  static constexpr u16 D_NEXT = 1, D_WRITE = 2, D_INDIRECT = 4;
  static constexpr u16 AVAIL_F_NO_INTERRUPT = 1;
  // ISR bits.
  static constexpr u8 ISR_QUEUE = 1, ISR_CONFIG = 2;

protected:
  // One buffer the driver made available: its descriptors, flattened.
  struct Seg {
    u64 addr;
    u32 len;
    bool write; // device-writable
  };
  struct Chain {
    u16 head = 0;
    std::vector<Seg> segs;
    size_t readable = 0; // bytes the device may read
    size_t writable = 0; // bytes the device may write
  };

  // For the devices, on the device thread (m_work held).
  bool queue_ready(int q);
  bool pop_chain(int q, Chain &c); // false: nothing available (or broken)
  void push_used(int q, u16 head, u32 len);
  void queue_interrupt(int q); // after pushing: ISR bit 0 unless suppressed
  size_t chain_read(const Chain &c, size_t off, void *buf, size_t n);
  size_t chain_write(const Chain &c, size_t off, const void *buf, size_t n);
  void raise_isr(u8 bits);
  bool driver_ok();

  // Guest memory as the device sees it: PCI bus addresses through the DMA
  // windows, or -- in the self-test, before any firmware has set them --
  // guest physical addresses as they are. False if out of reach.
  bool dma_read(u64 a, void *d, size_t n);
  bool dma_write(u64 a, const void *s, size_t n);

  // What each device supplies.
  virtual u32 device_features() = 0;
  virtual void process_queue(int q) = 0; // on the thread, m_work held
  virtual void poll() {}                 // each wake-up, m_work held
  virtual int poll_ms() { return 0; }    // 0: sleep until notified
  virtual void device_reset() {}         // m_work and m_mx held
  virtual void config_written(u32 off, int size) {}
  virtual void refresh_config() {} // before a config read, m_mx held
  virtual void selftest() {}       // ALPHABOX_VIRTIO_SELFTEST=1

  // Self-test driver: registers straight, structures in guest RAM.
  u32 st_reg_read(u32 off, int size);
  void st_reg_write(u32 off, int size, u32 v);
  void st_wait_ms(int ms);
  u32 st_base(); // this device's 1 MB of guest RAM for the test
  // A queue as the self-test's driver keeps it.
  struct StQueue {
    int index;
    u32 desc, avail, used; // guest physical
    u16 next_desc = 0, avail_idx = 0, used_seen = 0;
  };
  struct StDesc {
    u32 addr, len;
    bool write;
  };
  bool st_setup_queue(StQueue &sq, int q, u32 at); // 3 pages at `at`
  u16 st_submit(StQueue &sq, const std::vector<StDesc> &d, bool notify = true);
  // Wait for the next used entry; false on a timeout.
  bool st_wait_used(StQueue &sq, u32 &id, u32 &len, int timeout_ms = 2000);
  u8 *st_mem(u32 a);
  bool st_say(const char *what, bool ok);
  std::atomic_bool m_selftest{false}; // the self-test owns the device
  std::atomic_bool m_st_direct{true}; // ...and addresses memory physically
  u64 m_st_bus_off = 0;               // else bus = physical + this
  u64 st_bus(u32 phys) const { return phys + m_st_bus_off; }
  bool st_find_window();
  bool st_irq() const { return m_irq_level; }
  u8 st_isr_peek(); // without the read's side effect
  bool st_wait_irq(int timeout_ms = 500);

  std::vector<u8> m_config; // device-specific configuration, little-endian
  std::mutex m_mx;          // registers
  std::mutex m_work;        // queue processing (taken before m_mx)
  const char *m_name;
  void kick();

private:
  u32 reg_read(u32 off, int size);
  void reg_write(u32 off, int size, u32 v);
  void reset_device(); // m_work and m_mx held
  void update_irq();
  void run();
  u64 desc_base(int q) const { return (u64)m_state.q[q].pfn * kPage; }
  u64 avail_base(int q) const { return desc_base(q) + 16 * kQueueSize; }
  u64 used_base(int q) const {
    return (avail_base(q) + 6 + 2 * kQueueSize + kPage - 1) & ~(u64)(kPage - 1);
  }

  int m_queues;
  int m_instance = 0; // which self-test slice of guest RAM
  std::atomic_bool m_irq_level{false};
  bool m_broken_reported = false;

  struct SVirtio_state {
    u32 guest_features;
    u16 queue_sel;
    u8 status;
    u8 isr;
    struct {
      u32 pfn;
      u16 last_avail; // next available-ring entry the device takes
      u16 used_idx;   // the used ring's index
    } q[4];
  } m_state;

  std::unique_ptr<std::thread> myThread;
  std::unique_ptr<std::thread> m_selftest_thread;
  std::atomic_bool StopThread{false};
  std::atomic_bool myThreadDead{false};
  std::mutex m_kick_mx;
  std::condition_variable m_kick_cv;
  bool m_kicked = false;
};

#endif
