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
 * The Marvel system logic (Marvel.hpp).
 **/
#include "StdAfx.hpp"

#include "AlphaCPU.hpp"
#include "Ev7.hpp"
#include "Marvel.hpp"
#include "System.hpp"

/**
 * The processors the board row can hold, where its layout puts them: a
 * register block for each of their PIDs. Which of them are there is
 * decided later, by the processors the configuration names (present()).
 */
CMarvel::CMarvel(CSystem *sys) : CChipset(sys), m_gio(new GioRecorder()) {
  const platform_config &row = sys->platform();
  m_topology.build(row.marvel, row.max_cpus);
  for (int i = 0; i < m_topology.count(); i++) {
    const u32 pid = m_topology.node(i).pid;
    if (pid >= (u32)kMaxPids)
      FAILURE(Configuration, "a processor's PID beyond the console's range");
    m_csr[pid].reset(new CEv7Csr(sys, pid, m_gio.get()));
  }
  // ALPHABOX_TRACE_RBOX=<ms> (default 200): report any processor that
  // leaves an enabled interrupt pending that long, with its state -- for a
  // guest's CPUSPINWAIT, to tell an interrupt not raised from one not taken.
  if (const char *e = getenv("ALPHABOX_TRACE_RBOX")) {
    const int ms = atoi(e) > 1 ? atoi(e) : 200;
    m_rbox_watch = std::thread([this, ms] {
      for (int n = 1; !m_rbox_watch_stop.load(); n++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        for (auto &c : m_csr)
          if (c) {
            c->check_pending(ms);
            if (n % 500 == 0) // every 10 s
              c->tick_stats();
          }
      }
    });
  }
}

int CMarvel::present() const { return m_sys->get_cpu_num(); }

/// Processor `index` of the configuration is the topology's: its PID.
u32 CMarvel::cpu_pid(int index) const {
  return index < m_topology.count() ? m_topology.node(index).pid : (u32)index;
}

CMarvel::~CMarvel() {
  m_rbox_watch_stop = true;
  if (m_rbox_watch.joinable())
    m_rbox_watch.join();
}

void CMarvel::set_management(std::unique_ptr<GioManagement> far) {
  for (auto &c : m_csr)
    if (c)
      c->set_gio_management(far.get());
  m_gio = std::move(far);
}

u64 CMarvel::phys_mask() const { return ev7::kPhysMask; }

/**
 * memory.bits is each processor's memory; PID n's begins at
 * ev7::memory_base(n), 16 GB apart, so the array spans from 0 to the end of
 * the last processor's. The host allocates it lazily (calloc of untouched
 * pages costs no memory), so only what the guest touches is backed.
 *
 * The holes between processors' memory read as zero here rather than as
 * nonexistent memory: the console takes memory sizes from its management
 * side, not by probing [inference], and nothing has been seen to touch them.
 */
unsigned CMarvel::memory_span_bits(unsigned membits, int max_cpus) {
  if (membits > 34)
    FAILURE(Configuration,
            "an EV7 holds at most 16 GB of its own memory (memory.bits 34)");
  m_memory_per_pid = U64(1) << membits;
  u64 end = m_memory_per_pid;
  for (int i = 0; i < max_cpus && i < m_topology.count(); i++) {
    const u64 e = ev7::memory_base(m_topology.node(i).pid) + m_memory_per_pid;
    if (e > end)
      end = e;
  }
  unsigned bits = membits;
  while ((U64(1) << bits) < end)
    bits++;
  printf("%%MVL-I-MEMORY: %u MB per processor, each at its PID's base "
         "(%d processor%s, the array spans %u bits).\n",
         (unsigned)(m_memory_per_pid >> 20), max_cpus, max_cpus == 1 ? "" : "s",
         bits);
  return bits;
}

void CMarvel::attach_io7(u32 pid, u8 backplane_rev, u8 io_type) {
  if (pid >= (u32)kMaxPids || !m_csr[pid])
    FAILURE(Configuration, "an IO7 on a processor that cannot exist");
  m_io7[pid].reset(new CIo7(m_sys, this, pid, backplane_rev, io_type));
  m_csr[pid]->set_io7_attached(true);
}

CIo7 *CMarvel::io7(u32 pid) const {
  return pid < (u32)kMaxPids ? m_io7[pid].get() : nullptr;
}

/// An IO7's space: PA<43> set, the PE inverted in PA<42:35>, the port
/// inverted in PA<34:32> (Io7.hpp).
static bool io7_decode(u64 a, u32 *pe, u32 *port, u32 *off) {
  if (!ev7::is_io(a))
    return false;
  *pe = (u32)(~(a >> 35) & 0xff);
  *port = (u32)(~(a >> 32) & 7);
  *off = (u32)a;
  return true;
}

/// Only the processors the configuration has: another PID's block is
/// space nobody answers.
CEv7Csr *CMarvel::csr(u32 pid) const {
  if (pid >= (u32)kMaxPids || !m_csr[pid] || !m_topology.by_pid(pid, present()))
    return nullptr;
  return m_csr[pid].get();
}

u64 CMarvel::read_io(u64 a, u64 raw, int dsize, CSystemComponent *source) {
  (void)raw;
  u32 pe, off;
  if (ev7::csr_decode(a, &pe, &off))
    if (CEv7Csr *c = csr(pe))
      return c->read(off, dsize);
  u32 port;
  if (io7_decode(a, &pe, &port, &off))
    if (CIo7 *io = io7(pe))
      return io->read(port, off, dsize, source);
  // Another processor's space that is not there, or memory nobody owns.
  m_sys->trace_unknown(ev7::is_io(a) ? "EV7 I/O" : "EV7 memory", a, dsize,
                       false, 0, source);
  return 0;
}

void CMarvel::write_io(u64 a, u64 raw, int dsize, u64 data,
                       CSystemComponent *source) {
  (void)raw;
  u32 pe, off;
  if (ev7::csr_decode(a, &pe, &off))
    if (CEv7Csr *c = csr(pe)) {
      c->write(off, dsize, data);
      return;
    }
  u32 port;
  if (io7_decode(a, &pe, &port, &off))
    if (CIo7 *io = io7(pe)) {
      io->write(port, off, dsize, data, source);
      return;
    }
  m_sys->trace_unknown(ev7::is_io(a) ? "EV7 I/O" : "EV7 memory", a, dsize, true,
                       data, source);
}

/**
 * A device's interrupt line: `number` is the IO7's PE << 8 | the LSI
 * (io7::lsi; the board's pci_interrupt makes it from the hose and slot).
 */
void CMarvel::interrupt(int number, bool assert) {
  if (number < 0)
    return;
  if (CIo7 *io = io7((u32)number >> 8))
    io->lsi((u32)number & 0xff, assert);
}

void CMarvel::interval_tick() {
  for (int i = 0; i < present() && i < m_topology.count(); i++)
    if (CEv7Csr *c = csr(m_topology.node(i).pid))
      c->interval_tick();
  m_gio->tick();
  m_sys->note_interval_tick();
}

/// The schedule follows PID 0's RBOX_IT (CEv7Csr::interval_period_ns).
u64 CMarvel::interval_period_ns() const {
  CEv7Csr *c = csr(0);
  CAlphaCPU *cpu0 = m_sys->get_cpu_num() ? m_sys->get_cpu(0) : nullptr;
  return (c && cpu0) ? c->interval_period_ns(cpu0->get_speed()) : 0;
}

/// The native PALcode's acknowledgements: never used on an EV7, whose board
/// row keeps the native PALcode routines off (vmspal_pal_base 0).
void CMarvel::ack_interval_timer(int cpu) { (void)cpu; }
void CMarvel::ack_ipi(int cpu) { (void)cpu; }

/// Hose h is PID h / 4, IO7 port h % 4 (Linux marvel_find_console_vga_hose).
u64 CMarvel::pci_space_base(int hose, pci_space space) const {
  const u64 base = ev7::io7_base((u32)hose / 4, (u32)hose % 4);
  switch (space) {
  case PCI_SPACE_MEM:
    return base;
  case PCI_SPACE_CONFIG:
    return base + U64(0xFE000000);
  case PCI_SPACE_IO:
    return base + U64(0xFF000000);
  }
  return base;
}

/// DMA through the windows of the IO7 port the hose is.
u64 CMarvel::pci_phys(int hose, u32 address) {
  if (CIo7 *io = io7((u32)hose / 4))
    return io->pci_phys((u32)hose % 4, address);
  return address;
}

/**
 * Leave every processor as the XSROM does (cpu/ev7/Ev7Reset.cpp): the
 * primary in the console with the registers its decompressor hands on, the
 * others parked until the console writes a start address to their
 * RBOX_SCRATCH1 (CEv7Csr::scratch_written).
 */
void CMarvel::console_started(CAlphaCPU **cpus, int ncpus, u64 image_base) {
  // The register blocks again, now that each knows its processor's part.
  for (int i = 0; i < ncpus; i++)
    if (CEv7Csr *c = csr(cpus[i]->get_pid())) {
      c->reset();
      c->load_routes(m_topology, present());
    }
  for (int i = 0; i < ncpus; i++) {
    if (i == 0)
      ev7::xsrom_handoff(cpus[i], cpus[i]->get_pid(), image_base);
    else
      printf("%%MVL-I-PARKED: PID %u waits on RBOX_SCRATCH1 for a start "
             "address.\n",
             cpus[i]->get_pid());
  }
  if (getenv("ALPHABOX_EV7_START_SECONDARIES"))
    start_secondaries_as_console(cpus, ncpus);
}

/**
 * ALPHABOX_EV7_START_SECONDARIES=1, a test hook: do at once what the
 * console does once it has its configuration (0x2dd600 in the running
 * image) -- copy its PALcode into each secondary's own memory and start the
 * secondary there through its RBOX_SCRATCH1, f1/f2/f3, on the system bus
 * like any processor's stores. The console itself cannot get that far
 * until its management side answers (packet M4); this shows the parked
 * processors, the per-PID memory and the per-PID register blocks working
 * without it.
 */
void CMarvel::start_secondaries_as_console(CAlphaCPU **cpus, int ncpus) {
  const u64 pal = U64(0x30000), pal_bytes = U64(0x20000);
  for (int i = 1; i < ncpus; i++) {
    const u32 pid = cpus[i]->get_pid();
    const u64 base = ev7::memory_base(pid);
    char *from = m_sys->PtrToMem(pal), *to = m_sys->PtrToMem(base + pal);
    if (!from || !to)
      continue;
    memcpy(to, from, pal_bytes);
    m_sys->code_pages()->note_write_all();
    const u64 entry = base + pal; // the PALcode's reset entry, in PALmode
    const u64 scratch = ev7::csr_base(pid) | ev7csr::RBOX_SCRATCH1;
    m_sys->WriteMem(scratch, 64, (U64(0xf1) << 24) | ((entry >> 24) & 0xffffff),
                    nullptr);
    const u64 echo = m_sys->ReadMem(scratch, 64, nullptr);
    printf("%%MVL-I-SELFTEST: PID %u echoed %08" PRIx64 " on RBOX_SCRATCH1.\n",
           pid, echo);
    m_sys->WriteMem(scratch, 64, (U64(0xf3) << 24) | (entry & 0xffffff),
                    nullptr);
  }
}

void CMarvel::reset() {
  for (auto &c : m_csr)
    if (c) {
      c->reset();
      c->load_routes(m_topology, present());
    }
  for (auto &io : m_io7)
    if (io)
      io->reset();
}

void CMarvel::save_state(FILE *f) {
  for (auto &c : m_csr)
    if (c)
      c->save_state(f);
  for (auto &io : m_io7)
    if (io)
      io->save_state(f);
}

bool CMarvel::restore_state(FILE *f) {
  for (auto &c : m_csr)
    if (c && !c->restore_state(f))
      return false;
  for (auto &io : m_io7)
    if (io && !io->restore_state(f))
      return false;
  return true;
}
