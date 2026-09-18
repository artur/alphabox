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

/* QLogic ISP1020/ISP1040 PCI SCSI adapter -- the KZPBA the ES40 console
 * knows as "QLogic ISP10x0", whose disks it names dka0 like the Symbios.
 *
 * This is a different kind of adapter from the 53C8xx: instead of a
 * processor running SCRIPTS the host wrote, it has a RISC running QLogic's
 * own firmware, and the driver talks to that firmware. It hands commands
 * over in a request queue in host memory, each a 64-byte entry naming a
 * target, a command block and the buffers to move, and collects results
 * from a response queue; a set of mailbox registers carries everything
 * else (reset, queue addresses, parameters).
 *
 * So the emulation does not run QLogic's firmware: it answers the mailbox
 * commands the drivers use, and executes queue entries against the SCSI
 * bus. A driver that uploads firmware gets it accepted and ignored, which
 * is what it expects -- it never reads it back.
 *
 *   Isp1040.cpp          construction, PCI header, registers, reset,
 *                        the thread, NVRAM contents, state file
 *   Isp1040Mailbox.cpp   the mailbox command set
 *   Isp1040Queues.cpp    the request and response queues, and running a
 *                        queue entry against a SCSI target
 *   Isp1040Regs.hpp      registers, mailbox commands, queue entries
 *
 * Documentation consulted:
 *  - the NetBSD isp(4) driver (sys/dev/ic/isp.c, ispreg.h, ispmbox.h) and
 *    its PCI attachment, which document the register map, the mailbox
 *    command set, the queue entries and the NVRAM layout
 *  - Linux qla1280 and FreeBSD isp, for the same interface from another
 *    angle
 *  - the ES40 SRM console, which names the part and edits its NVRAM
 *    (isp1020_edit)
 */
#if !defined(INCLUDED_ISP1040_H_)
#define INCLUDED_ISP1040_H_

#include "DiskController.hpp"
#include "Eeprom93cx6.hpp"
#include "PCIDevice.hpp"
#include "SCSIDevice.hpp"

#include <condition_variable>
#include <mutex>

/**
 * \brief What distinguishes one ISP10x0 part from another.
 **/
struct isp_chip_config {
  const char *name;   ///< configuration class, e.g. "isp1040"
  const char *part;   ///< the part, for messages
  u16 device_id;      ///< PCI config 0x02
  u8 revision;        ///< PCI config 0x08
  bool wide;          ///< 16 targets rather than 8
  bool ultra;         ///< Ultra SCSI (20 MB/s)
  u16 firmware_major; ///< what ABOUT FIRMWARE reports
  u16 firmware_minor;
  u16 firmware_micro;
};

/**
 * \brief Emulated QLogic ISP1020/ISP1040 SCSI adapter.
 **/
class CIsp1040 : public CPCIDevice, public CDiskController, public CSCSIDevice {
public:
  CIsp1040(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev,
           const isp_chip_config &chip);
  virtual ~CIsp1040();

  virtual void init();
  virtual void start_threads();
  virtual void stop_threads();
  virtual void check_state();
  virtual void ResetPCI();
  virtual int SaveState(FILE *f);
  virtual int RestoreState(FILE *f);

  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize);
  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data);

  virtual void register_disk(class CDisk *dsk, int bus, int dev);

  /// The part named `name` ("isp1020", "isp1040"), or nullptr.
  static const isp_chip_config *find_chip(const char *name);

private:
  const isp_chip_config m_chip;

  // Isp1040.cpp
  void run();
  void chip_reset(bool keep_parameters);
  void build_nvram();
  void nvram_pins(u16 value);
  void update_irq();
  void raise_async(u16 event);

  // Isp1040Mailbox.cpp
  void mailbox_command();
  void mailbox_done(u16 status);

  // Isp1040Queues.cpp
  void run_request_queue();
  bool execute_entry(u32 entry_address, u8 *entry);
  int gather_segments(u32 entry_address, const u8 *entry, u32 *address,
                      u32 *count, int max_segments);
  void post_response(const u8 *request, u16 completion, u16 scsi_status,
                     u16 state_flags, u32 residual, const u8 *sense,
                     int sense_length);

  // DMA helpers: queue entries are little-endian in host memory.
  u16 dma_read16(u32 address);
  u32 dma_read32(u32 address);
  void dma_write16(u32 address, u16 value);
  void dma_write32(u32 address, u32 value);

  std::unique_ptr<std::thread> myThread;
  std::atomic_bool myThreadDead{false};
  /// Serializes the registers, the queues and the SCSI bus.
  std::recursive_mutex myLock;
  std::condition_variable_any myWake;
  bool StopThread = false;

  /// Everything the state file keeps.
  struct SIsp_state {
    u16 icr;  ///< interrupt control
    u16 isr;  ///< interrupt status
    u16 sema; ///< semaphore: a mailbox result is waiting
    u16 conf1;
    u16 nvram;          ///< the EEPROM's pins, as last written
    u16 mailbox[8];     ///< what the driver wrote
    u16 mailbox_out[8]; ///< what we answer with
    bool risc_paused;
    bool risc_reset;
    bool firmware_running;

    u32 request_base; ///< the queues, in host memory
    u16 request_length;
    u16 request_in;  ///< the driver's producer index
    u16 request_out; ///< ours
    u32 response_base;
    u16 response_length;
    u16 response_in;  ///< ours
    u16 response_out; ///< the driver's consumer index

    u8 initiator_id;
    u16 pending_async; ///< an event to report once the mailbox is read
    bool irq_asserted;
    bool queue_pending; ///< entries left for the thread

    CEeprom93cx6 nvram_eeprom;
  } state;
};

#endif // !defined(INCLUDED_ISP1040_H_)
