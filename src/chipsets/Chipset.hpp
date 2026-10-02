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
 * The system logic between the processors and everything else: what a
 * Tsunami/Typhoon (ES40, DS20E, DS10, DS20L), a Titan (ES45, DS25, DS15) or
 * a Marvel (the EV7's own Rbox and Zbox plus its IO7s) does for the
 * machine, as far as the rest of the emulator needs to know.
 *
 * CSystem owns guest memory and the registered device ranges; it hands the
 * chipset what neither of those claims, and forwards interrupts, the
 * interval timer and PCI DMA translation to it. The board row
 * (Platform.hpp) says which chipset a machine has; one binary carries them
 * all and picks at run time (docs/platforms.md, "Source layout").
 *
 * What is deliberately NOT here:
 *
 *  - Guest DRAM. A processor reads and writes memory without asking the
 *    chipset (cpu_defs.hpp READ_PHYS, the JIT's page cache), and so does a
 *    device through CSystem::ReadMem's memory path: a virtual call per
 *    memory access would cost real speed. Only an address above memory
 *    that no device range claimed reaches read_io()/write_io().
 *  - The board: which interrupt input a slot reaches, which slots exist,
 *    the firmware and its flash, the management hardware. Those are rows
 *    and hooks in platforms/.
 *  - Devices: a PCI device asks for its interrupt through CSystem and for
 *    DMA through pci_phys(), the same on any chipset.
 *
 * Hot paths and what they cost here: interrupt() is one indirect call on a
 * path that already takes a lock; pci_phys() one per DMA translation;
 * read_io()/write_io() one per access to chipset registers or unclaimed
 * I/O space, after the device-range scan that serves every device register.
 **/
#if !defined(INCLUDED_CHIPSET_H_)
#define INCLUDED_CHIPSET_H_

#include "StdAfx.hpp"
#include <atomic>
#include <cstdio>

class CAlphaCPU;
class CSystem;
class CSystemComponent;
struct dimm_population;

/// Which chipset a board has (platform_config::chipset).
enum chipset_kind {
  CHIPSET_TSUNAMI, ///< 21272 Tsunami/Typhoon: Cchip, Dchips, Pchips, TIG bus
  CHIPSET_MARVEL,  ///< EV7 machines: each processor's Rbox/Zbox/GIO, IO7s
  CHIPSET_TITAN,   ///< 21274 Titan: Cchip, Dchips, two PA-chips (4 hoses), TIG
};

/// The spaces a PCI hose opens into the processor's physical address space.
enum pci_space {
  PCI_SPACE_MEM,    ///< PCI memory, dense
  PCI_SPACE_IO,     ///< PCI I/O, dense
  PCI_SPACE_CONFIG, ///< configuration space, bus << 16 | dev << 11 | fn << 8
};

class CChipset {
public:
  explicit CChipset(CSystem *sys) : m_sys(sys) {}
  virtual ~CChipset() = default;
  CChipset(const CChipset &) = delete;
  CChipset &operator=(const CChipset &) = delete;

#ifdef ALPHABOX_HVF
  // The dispatch loop inside the VM fires the interval timer and reads the
  // interrupt state, so under ALPHABOX_HV=1 the chipset lives in memory the
  // VM shares, like CSystem itself.
  static void *operator new(size_t n);
  static void operator delete(void *p) noexcept;
#endif

  virtual chipset_kind kind() const = 0;
  virtual const char *name() const = 0;

  // --- Physical address space ---------------------------------------------

  /// The physical address bits this chipset decodes. A processor's address
  /// is masked with it before anything looks at it: the Tsunami ignores
  /// PA<42:35> once PA<43> selects I/O (0x807'ffff'ffff); an EV7 decodes 44
  /// bits. Read once, when the system is built, so the memory path keeps a
  /// plain AND with a member.
  virtual u64 phys_mask() const = 0;

  /// How many address bits the memory array must span for `membits` of
  /// memory as the configuration gives it (memory.bits) on a board of
  /// `max_cpus` processors. Read once, when the system is built. A Tsunami
  /// puts its memory at 0 and the two are the same; an EV7 machine gives
  /// every processor its own memory at a PID-dependent base, so the array
  /// spans to the last processor's (the holes between are never touched,
  /// and the host never backs them).
  virtual unsigned memory_span_bits(unsigned membits, int max_cpus) {
    (void)max_cpus;
    return membits;
  }

  /// A read or write of non-memory space that no registered device range
  /// claimed: the chipset's own registers, and the PCI and I/O space
  /// nothing answers in. `a` is the masked address, `raw` the address as
  /// the processor or device issued it (for traces of another machine's
  /// firmware). `source` is the CPU or device making the access.
  virtual u64 read_io(u64 a, u64 raw, int dsize, CSystemComponent *source) = 0;
  virtual void write_io(u64 a, u64 raw, int dsize, u64 data,
                        CSystemComponent *source) = 0;

  // --- Interrupts and the interval timer ----------------------------------

  /// A device interrupt input changes level. `number` is the chipset's own
  /// input as the board's interrupt map gives it (Tsunami: a DRIR bit).
  virtual void interrupt(int number, bool assert) = 0;

  /// One interval-timer period has elapsed. Processor 0's dispatch loop
  /// keeps the wall-clock schedule (cpu/AlphaCPU.hpp); the chipset decides
  /// which processors see it and how (Tsunami: MISC<ITINTR> and b_irq<2> on
  /// every processor; Marvel: each EV7's own RBOX_IT).
  virtual void interval_tick() = 0;

  /// The interval timer's period when the chipset's own registers set it,
  /// or 0: on boards with the ALi its PIT sets it (CAliM1543C), and with
  /// neither the schedule ticks once a second (Marvel: RBOX_IT).
  virtual u64 interval_period_ns() const { return 0; }

  /// The native PALcode's acknowledgements (cpu/AlphaCPU_vmspal.cpp): what
  /// the real PALcode does by writing the chipset's registers when it takes
  /// an interval-timer or interprocessor interrupt on processor `cpu`.
  virtual void ack_interval_timer(int cpu) = 0;
  virtual void ack_ipi(int cpu) = 0;

  // --- PCI hoses ------------------------------------------------------------

  /// Where `space` of PCI hose `hose` begins in physical address space.
  virtual u64 pci_space_base(int hose, pci_space space) const = 0;

  /// Translate an address a device on `hose` puts on its bus as master into
  /// a physical address: the hose's DMA windows, direct-mapped or
  /// scatter-gather. An address no window takes stays on the hose (peer to
  /// peer, as the Tsunami does).
  virtual u64 pci_phys(int hose, u32 address) = 0;

  // --- Console start ---------------------------------------------------------

  /// The console is in memory and every processor has been handed to it
  /// (CSystem::LoadROM), from an image whose self-decompressor ran at
  /// `image_base` (0 when not known). The place for the state a machine's own
  /// reset firmware leaves that the console reads: the EV7's XSROM handoff.
  virtual void console_started(CAlphaCPU **cpus, int ncpus, u64 image_base) {
    (void)cpus;
    (void)ncpus;
    (void)image_base;
  }
  // --- Memory ---------------------------------------------------------------
  /// The DIMMs the chipset's memory controller reports (DimmModel.hpp), for
  /// a management processor that keeps its own copy (the ES40's and ES45's
  /// DPR); nullptr when the chipset models none.
  virtual const dimm_population *dimms() const { return nullptr; }

  // --- Reset and state ------------------------------------------------------

  /// Back to the power-on state (a firmware-requested system reset).
  virtual void reset() = 0;
  /// The chipset's part of a state file, written right after CSystem's.
  virtual void save_state(FILE *f) = 0;
  /// Returns false when the file is truncated.
  virtual bool restore_state(FILE *f) = 0;

protected:
  CSystem *m_sys;
};

/// Build the chipset a board names.
CChipset *create_chipset(chipset_kind kind, CSystem *sys);

#endif // !defined(INCLUDED_CHIPSET_H_)
