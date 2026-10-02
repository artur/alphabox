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

CMarvel::CMarvel(CSystem *sys) : CChipset(sys), m_gio(new GioRecorder()) {
  for (u32 pid = 0; pid < kMaxPids; pid++)
    m_csr[pid].reset(new CEv7Csr(sys, pid, m_gio.get()));
}

CMarvel::~CMarvel() = default;

void CMarvel::set_management(std::unique_ptr<GioManagement> far) {
  for (auto &c : m_csr)
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
  const u32 last = (u32)(max_cpus > 0 ? max_cpus - 1 : 0);
  const u64 end = ev7::memory_base(last) + m_memory_per_pid;
  unsigned bits = membits;
  while ((U64(1) << bits) < end)
    bits++;
  printf("%%MVL-I-MEMORY: %u MB per processor, PID n at n * 16 GB "
         "(%d processor%s, the array spans %u bits).\n",
         (unsigned)(m_memory_per_pid >> 20), max_cpus, max_cpus == 1 ? "" : "s",
         bits);
  return bits;
}

CEv7Csr *CMarvel::csr(u32 pid) const {
  if (pid >= kMaxPids || (int)pid >= m_sys->get_cpu_num())
    return nullptr;
  return m_csr[pid].get();
}

u64 CMarvel::read_io(u64 a, u64 raw, int dsize, CSystemComponent *source) {
  (void)raw;
  u32 pe, off;
  if (ev7::csr_decode(a, &pe, &off))
    if (CEv7Csr *c = csr(pe))
      return c->read(off, dsize);
  // An IO7's space (packet M5), another processor's that is not there, or
  // memory nobody owns.
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
  m_sys->trace_unknown(ev7::is_io(a) ? "EV7 I/O" : "EV7 memory", a, dsize, true,
                       data, source);
}

/// Device interrupts reach an EV7 through an IO7 (M5); there is none yet.
void CMarvel::interrupt(int number, bool assert) {
  (void)number;
  (void)assert;
}

void CMarvel::interval_tick() {
  for (u32 pid = 0; pid < kMaxPids; pid++)
    if (CEv7Csr *c = csr(pid))
      c->interval_tick();
  m_sys->note_interval_tick();
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

/// No IO7 yet, so no DMA windows: a bus address is taken as physical.
u64 CMarvel::pci_phys(int hose, u32 address) {
  (void)hose;
  return address;
}

/**
 * Leave every processor as the XSROM does (cpu/ev7/Ev7Reset.cpp): the
 * primary in the console with the registers its decompressor hands on, the
 * others parked until the console writes a start address to their
 * RBOX_SCRATCH1 (CEv7Csr::scratch_written).
 */
void CMarvel::console_started(CAlphaCPU **cpus, int ncpus, u64 image_base) {
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
    c->reset();
}

void CMarvel::save_state(FILE *f) {
  for (auto &c : m_csr)
    c->save_state(f);
}

bool CMarvel::restore_state(FILE *f) {
  for (auto &c : m_csr)
    if (!c->restore_state(f))
      return false;
  return true;
}
