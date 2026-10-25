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
 * Marvel: the system logic of the EV7 machines (ES47, ES80, GS1280), which
 * is not a chipset at all. Each EV7 carries its own memory controllers,
 * interrupt logic and router (its CSR block, Ev7Csr.hpp) and reaches I/O
 * through an IO7 bridge on its I/O port.
 *
 * What is here: the 44-bit physical address map (cpu/ev7/Ev7.hpp), memory
 * per PID, every processor's CSR block with its GIO port to a management
 * side (Gio.hpp), the Rbox interval timer, and the XSROM's handoff to the
 * console, and the IO7s (Io7.hpp) with their PCI hoses: hose PID * 4 +
 * port. The CMM's answers on GIO are the board's (platforms/es47/Cmm.hpp).
 * docs/platforms/marvel.md has the plan and the findings.
 *
 * 21464 mode (docs/platforms/ev8-native.md, section 2): the same machines
 * with `ev8n` processors, the 21464 as it was designed for them. The map is
 * the 21464's (48 bits, ipe8, N18), the PIDs are dense (N19), a chip's four
 * TPUs share its register block, whose interrupts are steered to them by a
 * mask per TPU over one status register (N22), the interrupt queue is four
 * deep and an IO7 resends what it refuses (N21), GIO carries 32 bits
 * (N23). No console runs: every TPU starts in a boot image's PALcode
 * (console_started).
 **/
#if !defined(INCLUDED_MARVEL_H_)
#define INCLUDED_MARVEL_H_

#include "Chipset.hpp"
#include "Ev7.hpp"
#include "Ev7Csr.hpp"
#include "Gio.hpp"
#include "Io7.hpp"
#include "Topology.hpp"

#include <atomic>
#include <memory>
#include <thread>

class CMarvel : public CChipset {
public:
  explicit CMarvel(CSystem *sys);
  ~CMarvel() override;

  chipset_kind kind() const override { return CHIPSET_MARVEL; }
  const char *name() const override {
    return m_21464 ? "Marvel (21464 mode)" : "Marvel (EV7)";
  }

  u64 phys_mask() const override;
  unsigned memory_span_bits(unsigned membits, int max_cpus) override;
  bool memory_regions(std::vector<memory_region> &out) const override;

  u64 read_io(u64 a, u64 raw, int dsize, CSystemComponent *source) override;
  void write_io(u64 a, u64 raw, int dsize, u64 data,
                CSystemComponent *source) override;

  void interrupt(int number, bool assert) override;
  void interval_tick() override;
  u64 interval_period_ns() const override;

  u64 pci_space_base(int hose, pci_space space) const override;
  u64 pci_phys(int hose, u32 address) override;

  void console_started(CAlphaCPU **cpus, int ncpus, u64 image_base) override;
  u32 cpu_pid(int index) const override;

  void reset() override;
  void save_state(FILE *f) override;
  bool restore_state(FILE *f) override;
  void stop_watch() override;

  /// The board's management side for every processor's GIO port, in place
  /// of the recorder: the ES47's CMM (platforms/es47/Cmm.hpp).
  void set_management(std::unique_ptr<GioManagement> far);

  /// Each processor's own memory, in bytes (memory.bits).
  u64 memory_per_pid() const { return m_memory_per_pid; }

  /// The processor block of PE `pid`, or nullptr when there is none.
  CEv7Csr *csr(u32 pid) const;

  /// Where the processors sit, from the board row (Topology.hpp).
  const CMarvelTopology &topology() const { return m_topology; }
  /// 21464 mode: the processors are native EV8s (the `ev8n` row), and the
  /// machine has the 21464's map, dense PIDs, a register block per chip
  /// with its interrupts steered to the TPUs, and 32-bit GIO
  /// (docs/platforms/ev8-native.md, section 2).
  bool mode_21464() const { return m_21464; }
  /// The physical address map: the EV7's, or the 21464 mode's (Ev7.hpp).
  const ev7::address_map &map() const { return *m_map; }
  /// TPUs per chip sharing a register block: the chip's in 21464 mode, 1
  /// otherwise (each EV7, and each listed TPU of an EV7-compatible EV8, has
  /// a block of its own).
  int tpus_per_block() const { return m_21464 ? m_topology.tpus() : 1; }
  /// Processor `index` of the configuration, or nullptr.
  CAlphaCPU *processor(int index) const;
  /// A processor's interrupt queue has room again: the IO7s send what it
  /// refused (21464 mode, N21). Called with no register block locked.
  void resend_iids();
  /// How many processors are configured (the topology's first ones).
  int present() const;

  /// The board puts an IO7 on PE `pid`'s I/O port, on an I/O backplane of
  /// revision `backplane_rev` in I/O of type `io_type` (io7::kIoType*).
  void attach_io7(u32 pid, u8 backplane_rev, u8 io_type);
  /// The IO7 on PE `pid`, or nullptr.
  CIo7 *io7(u32 pid) const;

private:
  void start_secondaries_as_console(CAlphaCPU **cpus, int ncpus);
  /// PIDs are 8 bits wide in the console's coordinates (Topology.hpp).
  static constexpr int kMaxPids = 256;
  CMarvelTopology m_topology;
  bool m_21464 = false;
  const ev7::address_map *m_map = &ev7::kEv7Map;
  u64 m_memory_per_pid = 0;
  std::vector<memory_region> m_memory_regions; ///< each processor's, by base
  std::unique_ptr<GioManagement> m_gio;
  std::unique_ptr<CEv7Csr> m_csr[kMaxPids];
  std::unique_ptr<CIo7> m_io7[kMaxPids];
  /// ALPHABOX_TRACE_RBOX=<ms>: a thread that reports interrupts left pending.
  std::thread m_rbox_watch;
  std::atomic<bool> m_rbox_watch_stop{false};
};

#endif // !defined(INCLUDED_MARVEL_H_)
