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
 * console. What is not yet: the IO7 (packet M5) -- its space is traced as
 * unknown -- and the CMM's answers on GIO (packet M4).
 * docs/platforms/marvel.md has the plan and the findings.
 **/
#if !defined(INCLUDED_MARVEL_H_)
#define INCLUDED_MARVEL_H_

#include "Chipset.hpp"
#include "Ev7Csr.hpp"
#include "Gio.hpp"

#include <memory>

class CMarvel : public CChipset {
public:
  explicit CMarvel(CSystem *sys);
  ~CMarvel() override;

  chipset_kind kind() const override { return CHIPSET_MARVEL; }
  const char *name() const override { return "Marvel (EV7)"; }

  u64 phys_mask() const override;
  unsigned memory_span_bits(unsigned membits, int max_cpus) override;

  u64 read_io(u64 a, u64 raw, int dsize, CSystemComponent *source) override;
  void write_io(u64 a, u64 raw, int dsize, u64 data,
                CSystemComponent *source) override;

  void interrupt(int number, bool assert) override;
  void interval_tick() override;
  void ack_interval_timer(int cpu) override;
  void ack_ipi(int cpu) override;

  u64 pci_space_base(int hose, pci_space space) const override;
  u64 pci_phys(int hose, u32 address) override;

  void console_started(CAlphaCPU **cpus, int ncpus, u64 image_base) override;

  void reset() override;
  void save_state(FILE *f) override;
  bool restore_state(FILE *f) override;

  /// The board's management side for every processor's GIO port, in place
  /// of the recorder: the ES47's CMM (platforms/es47/Cmm.hpp).
  void set_management(std::unique_ptr<GioManagement> far);

  /// Each processor's own memory, in bytes (memory.bits).
  u64 memory_per_pid() const { return m_memory_per_pid; }

  /// The processor block of PE `pid`, or nullptr when there is none.
  CEv7Csr *csr(u32 pid) const;

private:
  void start_secondaries_as_console(CAlphaCPU **cpus, int ncpus);
  static constexpr int kMaxPids = 4; ///< CSystem's processor limit
  u64 m_memory_per_pid = 0;
  std::unique_ptr<GioManagement> m_gio;
  std::unique_ptr<CEv7Csr> m_csr[kMaxPids];
};

#endif // !defined(INCLUDED_MARVEL_H_)
