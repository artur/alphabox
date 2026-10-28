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
 * One EV7's on-chip register block (Ev7Csr.hpp).
 *
 * The interrupt wiring is the console PALcode's own decode (its interrupt
 * entry, 0x38ec0 in the decompressed SRM V7.3-1): the Rbox drives the
 * core's six external interrupt lines, which arrive in ISUM<38:33> exactly
 * as a Tsunami's do, and the handler for each line reads RBOX_INT &
 * RBOX_IMASK and looks at these bits:
 *
 *   EI0  bits 0-10 and 24-63   errors and their summaries
 *   EI1  bits 12 and 14        the I/O interrupt queue (RBOX_INTQ)
 *   EI2  bits 15-17            15 is the interval timer (RBOX_IT)
 *   EI3  bits 18-19
 *   EI4  bits 21-23            interprocessor: halt, start, IPI
 *   EI5                        machine check (no bit is tested)
 *
 * A bit is cleared by writing it to RBOX_INT (the handler for bit 15 writes
 * 1 << 15 and reads back). A processor raises a bit in another's RBOX_INT by
 * writing it to that processor's RBOX_IREQ (0x396b8: 1 << 23 to a target
 * PID's; 0x3f3f4: 1 << 22). Bits 11, 13 and 20 drive no line here: nothing
 * the console does says which they would [guess: none of them is raised by
 * anything this emulator models yet].
 *
 * The clock rendezvous. The interval-timer handler of the PALcode (0x39344,
 * the console's and OpenVMS's) counts its entries in its per-processor
 * area (+480); on every 128th, the primary (after some IO7 housekeeping)
 * writes 1 << 23 to every other processor's RBOX_IREQ (0x396b8), and a
 * secondary writes 1 << 23 to its own RBOX_INT -- clearing it -- and spins,
 * in PALmode, until RBOX_INT<23> or <22> is set (0x3972c-0x39738). <23> is
 * masked in RBOX_IMASK: it is only ever polled there. On the hardware both
 * processors' timers fire together and the secondary is waiting before the
 * primary's broadcast leaves; here CPU 0's thread delivers the tick to both
 * and runs its own handler at once, so the broadcast often reached CPU 1
 * before CPU 1 had taken the same tick -- and CPU 1's clear then erased it,
 * leaving it to spin for the next window, 128 ticks (~130 ms) with its
 * interrupts off, every window: ALPHABOX_TRACE_RBOX showed CPU 1 losing a
 * third of its ticks that way, and OpenVMS 8.4 bugchecked CPUSPINWAIT (and
 * once ran out of nonpaged pool) under DECwindows with two processors. So a
 * broadcast that arrives before the processor has started waiting for it
 * is held, and set at its clear -- the order the hardware's timing gives
 * [inference: nothing documents the bit beyond this code].
 **/
#include "StdAfx.hpp"

#include "AlphaCPU.hpp"
#include "CpuModel.hpp"
#include "Ev7.hpp"
#include "Ev7Csr.hpp"
#include "Marvel.hpp"
#include "System.hpp"
#include "Topology.hpp"

#include <algorithm>
#include <vector>

using namespace ev7csr;

namespace {

/// The console's table of the EV7's registers (SRM V7.3-1 at 0x3ac6b0 of the
/// running image), sorted by offset.
struct named_reg {
  u32 off;
  const char *name;
};
const named_reg kRegs[] = {
    {0x000000, "RBOX_CFG"},
    {0x000010, "RBOX_NSVC"},
    {0x000020, "RBOX_EWVC"},
    {0x000030, "RBOX_WHOAMI"},
    {0x000040, "RBOX_TCTL"},
    {0x000050, "RBOX_INT"},
    {0x000060, "RBOX_IMASK"},
    {0x000070, "RBOX_IREQ"},
    {0x000080, "RBOX_INTQ"},
    {0x000090, "RBOX_INTA"},
    {0x0000a0, "RBOX_IT"},
    {0x0000b0, "RBOX_SCRATCH1"},
    {0x0000d0, "RBOX_L_ERR"},
    {0x000100, "GIO_CFG"},
    {0x000110, "GIO_DAT"},
    {0x000120, "GIO_CTL"},
    {0x004000, "RBOX_N_CFG"},
    {0x004010, "RBOX_N_ERR"},
    {0x004020, "RBOX_N_PERF"},
    {0x004030, "RBOX_N_T1CFG"},
    {0x004040, "RBOX_N_T2CFG"},
    {0x006000, "RBOX_S_CFG"},
    {0x006010, "RBOX_S_ERR"},
    {0x006020, "RBOX_S_PERF"},
    {0x006030, "RBOX_S_T1CFG"},
    {0x006040, "RBOX_S_T2CFG"},
    {0x008000, "RBOX_E_CFG"},
    {0x008010, "RBOX_E_ERR"},
    {0x008020, "RBOX_E_PERF"},
    {0x008030, "RBOX_E_T1CFG"},
    {0x008040, "RBOX_E_T2CFG"},
    {0x00a000, "RBOX_W_CFG"},
    {0x00a010, "RBOX_W_ERR"},
    {0x00a020, "RBOX_W_PERF"},
    {0x00a030, "RBOX_W_T1CFG"},
    {0x00a040, "RBOX_W_T2CFG"},
    {0x00c000, "RBOX_IO_CFG"},
    {0x00c010, "RBOX_IO_ERR"},
    {0x00c020, "RBOX_IO_PERF"},
    {0x00c030, "RBOX_IO_T1CFG"},
    {0x00c040, "RBOX_IO_BUF"},
    {0x00e000, "CBOX_CTL"},
    {0x00e010, "CBOX_STP_CTL"},
    {0x00e020, "CBOX_ACC_CTL"},
    {0x00e030, "CBOX_LCL_SET"},
    {0x00e040, "CBOX_GBL_SET"},
    {0x00e050, "CBOX_TMR_CTL"},
    {0x00e060, "CBOX_PRF_CTL"},
    {0x00e070, "CBOX_PRF_ADR"},
    {0x00e090, "CBOX_PRF_MAT"},
    {0x00e0b0, "CBOX_PRF_CNT"},
    {0x010000, "ZBOX0_DIFT_CTL"},
    {0x010010, "ZBOX0_DIFT_TIMEOUT"},
    {0x010020, "ZBOX0_DRAM_ERR_ADR"},
    {0x010030, "ZBOX0_DIFT_ERR_STATUS"},
    {0x012000, "ZBOX0_DRAM_TIMING_CTL2"},
    {0x012010, "ZBOX0_DRAM_TIMING_CTL4"},
    {0x012020, "ZBOX0_DRAM_CALIB_CTL1"},
    {0x012030, "ZBOX0_DRAM_REFRESH_ROW"},
    {0x012040, "ZBOX0_DRAM_REFR_CTL"},
    {0x012100, "ZBOX0_FRC_ERR_ADR"},
    {0x012110, "ZBOX0_DRAM_CALIB_CTL2"},
    {0x012120, "ZBOX0_DRAM_TIMING_CTL3"},
    {0x012130, "ZBOX0_DRAM_INIT_CTL"},
    {0x012200, "ZBOX0_ZPM_CTR0"},
    {0x012210, "ZBOX0_ZPM_CTR1"},
    {0x012220, "ZBOX0_ZPM_CTL"},
    {0x012230, "ZBOX0_DRAM_MAPPER_CTL"},
    {0x014000, "ZBOX0_DRAM_ERR_STATUS1"},
    {0x014010, "ZBOX0_DRAM_ERR_STATUS2"},
    {0x014020, "ZBOX0_DRAM_SWEEP_DIR"},
    {0x014030, "ZBOX0_DRAM_ERR_STATUS3"},
    {0x014100, "ZBOX0_DRAM_ERROR_CTL"},
    {0x014110, "ZBOX0_DRAM_TIMING_CTL1"},
    {0x016000, "ZBOX1_DIFT_CTL"},
    {0x016010, "ZBOX1_DIFT_TIMEOUT"},
    {0x016020, "ZBOX1_DRAM_ERR_ADR"},
    {0x016030, "ZBOX1_DIFT_ERR_STATUS"},
    {0x018000, "ZBOX1_DRAM_TIMING_CTL2"},
    {0x018010, "ZBOX1_DRAM_TIMING_CTL4"},
    {0x018020, "ZBOX1_DRAM_CALIB_CTL1"},
    {0x018030, "ZBOX1_DRAM_REFRESH_ROW"},
    {0x018040, "ZBOX1_DRAM_REFR_CTL"},
    {0x018100, "ZBOX1_FRC_ERR_ADR"},
    {0x018110, "ZBOX1_DRAM_CALIB_CTL2"},
    {0x018120, "ZBOX1_DRAM_TIMING_CTL3"},
    {0x018130, "ZBOX1_DRAM_INIT_CTL"},
    {0x018200, "ZBOX1_ZPM_CTR0"},
    {0x018210, "ZBOX1_ZPM_CTR1"},
    {0x018220, "ZBOX1_ZPM_CTL"},
    {0x018230, "ZBOX1_DRAM_MAPPER_CTL"},
    {0x01a000, "ZBOX1_DRAM_ERR_STATUS1"},
    {0x01a010, "ZBOX1_DRAM_ERR_STATUS2"},
    {0x01a020, "ZBOX1_DRAM_SWEEP_DIR"},
    {0x01a030, "ZBOX1_DRAM_ERR_STATUS3"},
    {0x01a100, "ZBOX1_DRAM_ERROR_CTL"},
    {0x01a110, "ZBOX1_DRAM_TIMING_CTL1"},
    {0x01c000, "BBOX_CTL"},
    {0x01c010, "BBOX_ERR_STS"},
    {0x01c020, "BBOX_ERR_IDX"},
    {0x01c030, "CBOX_DDP_ERR_STS"},
    {0x01c040, "BBOX_DAT_RMP"},
    {0x020000, "PADS_RBOX_N0"},
    {0x020010, "PADS_RBOX_N1"},
    {0x020100, "PADS_RBOX_E0"},
    {0x020110, "PADS_RBOX_E1"},
    {0x020200, "PADS_RBOX_S0"},
    {0x020210, "PADS_RBOX_S1"},
    {0x020300, "PADS_RBOX_W0"},
    {0x020310, "PADS_RBOX_W1"},
    {0x020400, "PADS_RBOX_IO0"},
    {0x020410, "PADS_RBOX_IO1"},
    {0x022000, "PADS_ZBOX_CH0"},
    {0x022100, "PADS_ZBOX_CH1"},
    {0x022200, "PADS_ZBOX_CH2"},
    {0x022300, "PADS_ZBOX_CH3"},
    {0x022400, "PADS_ZBOX_CH4"},
    {0x022500, "PADS_ZBOX_CH5"},
    {0x022600, "PADS_ZBOX_CH6"},
    {0x022700, "PADS_ZBOX_CH7"},
    {0x022800, "PADS_ZBOX_CH8"},
    {0x022900, "PADS_ZBOX_CH9"},
    {0x024000, "OCLA1_TMATCH"},
    {0x024010, "OCLA1_TMASK"},
    {0x024020, "OCLA1_CTL"},
    {0x024030, "OCLA1_MISC"},
    {0x024100, "OCLA1_SMATCH"},
    {0x024110, "OCLA1_SMASK"},
    {0x024120, "OCLA1_DATA"},
    {0x024200, "OCLA1_PC_TMATCH"},
    {0x024210, "OCLA1_PC_TMASK"},
    {0x024220, "OCLA1_PC_SMATCH"},
    {0x024230, "OCLA1_PC_SMASK"},
    {0x024240, "OCLA1_PC_CTL"},
    {0x026000, "OCLA0_TMATCH"},
    {0x026010, "OCLA0_TMASK"},
    {0x026020, "OCLA0_CTL"},
    {0x026030, "OCLA0_MISC"},
    {0x026100, "OCLA0_SMATCH"},
    {0x026110, "OCLA0_SMASK"},
    {0x026120, "OCLA0_DATA"},
};

/// Registers the console touches that are in no table: Linux's second
/// scratch register, and a block after the logic analysers that the
/// PALcode probes on an EV7 (0x3eaa0: it saves 0x28040, writes all ones to
/// 0x28020, takes bit 19 of what reads back as a revision bit and restores
/// 0x28040).
const named_reg kMoreRegs[] = {
    {RBOX_SCRATCH2, "RBOX_SCRATCH2"},
    {0x28020, "(unnamed 28020)"},
    {0x28040, "(unnamed 28040)"},
};

/**
 * 21464 mode (docs/platforms/ev8-native.md, 2.3, N20): the 21464's Rbox
 * registers the EV7 has no place for, at free offsets of their blocks
 * [decision]: R_CFG2 beside the Rbox's own, each port's CFG2 after its
 * T2CFG, the I/O port's CFG2 and T2CFG after its BUFSIZ (the EV7's
 * RBOX_IO_BUF). The ports' R_n_T2CFG is the EV7's RBOX_n_T2CFG, at + 0x40.
 * Each TPU's R_INT_MASK at R_INT_MASK_TPU + 0x40 x TPU (N22) is not in the
 * table: TPU 0's is RBOX_IMASK, the others' are made at reset.
 */
const named_reg kRegs21464[] = {
    {R_CFG2, "R_CFG2"},       {0x004050, "R_N_CFG2"}, {0x006050, "R_S_CFG2"},
    {0x008050, "R_E_CFG2"},   {0x00a050, "R_W_CFG2"}, {0x00c050, "R_IO_CFG2"},
    {0x00c060, "R_IO_T2CFG"},
};

u32 route_entries(bool m21464) {
  return m21464 ? R_ROUT_ENTRIES : RBOX_ROUTE_ENTRIES;
}

bool is_route(u32 off, bool m21464) {
  return off >= RBOX_ROUTE && off < RBOX_ROUTE + route_entries(m21464) * 0x10 &&
         !(off & 0xf);
}

/// The name of the register at `off`, in the EV7's block or (`m21464`) in
/// the 21464 mode's.
const char *reg_name(u32 off, bool m21464) {
  if (is_route(off, m21464))
    return m21464 ? "R_ROUT" : "RBOX_ROUTE";
  for (const named_reg &r : kRegs)
    if (r.off == off)
      return r.name;
  for (const named_reg &r : kMoreRegs)
    if (r.off == off)
      return r.name;
  if (m21464) {
    for (const named_reg &r : kRegs21464)
      if (r.off == off)
        return r.name;
    static const char *const masks[] = {"R_INT_MASK0", "R_INT_MASK1",
                                        "R_INT_MASK2", "R_INT_MASK3"};
    if (off >= R_INT_MASK_TPU && off < R_INT_MASK_TPU + 4 * 0x40 &&
        !(off & 0x3f))
      return masks[(off - R_INT_MASK_TPU) / 0x40];
    return nullptr;
  }
  switch (off) {
  case GIO_LOCK:
    return "GIO lock";
  }
  return nullptr;
}

/**
 * The level an R_INT_STAT bit drives in 21464 mode (N22, after the six
 * levels of [IDS p. 465]: 0 correctable or performance, 1 the interrupt
 * queue, 2 the interval timer, 3 interprocessor or software, 4 halt or
 * other, 5 uncorrectable): -1 for a bit past the register's 32.
 */
int int_level_21464(int bit) {
  if (bit < 4)
    return 3; // IPI to TPU 0-3
  if (bit < 8)
    return 2; // interval timer tick for TPU 0-3
  if (bit < 12)
    return 4; // halt to TPU 0-3
  if (bit == 12)
    return 1; // the interrupt queue
  if (bit == 13 || bit == 14)
    return 4; // GIO_INT, GIO_HINT
  if (bit == 15 || bit == 16)
    return 0; // performance counter, correctable error
  if (bit == 17)
    return 5;               // uncorrectable error
  return bit < 32 ? 3 : -1; // software use
}

u64 level_mask_21464(int level) {
  u64 m = 0;
  for (int b = 0; b < 32; b++)
    if (int_level_21464(b) == level)
      m |= U64(1) << b;
  return m;
}

/// Which external interrupt line an RBOX_INT bit drives (-1: none).
int int_line(int bit) {
  if (bit <= 10 || bit >= 24)
    return 0;
  if (bit == 12 || bit == 14)
    return 1;
  if (bit >= 15 && bit <= 17)
    return 2;
  if (bit == 18 || bit == 19)
    return 3;
  if (bit >= 21 && bit <= 23)
    return 4;
  return -1;
}

u64 line_mask(int line) {
  u64 m = 0;
  for (int b = 0; b < 64; b++)
    if (int_line(b) == line)
      m |= U64(1) << b;
  return m;
}

/// ALPHABOX_TRACE_CSR=1: every access to a modelled register, named, with
/// the instruction that made it -- the first three of each and then every
/// power of two, as the unknown-access trace does (a GIO poll repeats one
/// read 2^28 times).
bool trace_csr_on() {
  static const bool on = getenv("ALPHABOX_TRACE_CSR") != nullptr;
  return on;
}

void trace_csr(u32 pid, u32 off, bool write, u64 v, bool m21464) {
  static std::mutex m;
  static std::map<u64, u64> seen;
  const u64 pc = t_running_cpu ? t_running_cpu->get_pc() : 0;
  u64 n;
  {
    std::lock_guard<std::mutex> g(m);
    if (seen.size() > 100000)
      seen.clear();
    n = ++seen[(pc << 24) ^ ((u64)pid << 1) ^ ((u64)off << 2) ^
               (write ? 1 : 0)];
  }
  if (n > 3 && (n & (n - 1)))
    return;
  const char *name = reg_name(off, m21464);
  char nm[32];
  if (!name) {
    snprintf(nm, sizeof(nm), "+%05x", off);
    name = nm;
  }
  printf("%%MVL-T-CSR: pid %u %s %-22s %s %016" PRIx64 "%s pc=%" PRIx64 "\n",
         pid, write ? "write" : "read ", name, write ? "=" : "->", v,
         n > 3 ? " (repeated)" : "", pc);
}

/// ALPHABOX_TRACE_MP on Marvel: microseconds since the first line, so the
/// start protocol's hand-offs between processors can be timed.
long long mp_us() {
  static const auto t0 = std::chrono::steady_clock::now();
  return (long long)std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

} // namespace

/// ALPHABOX_TRACE_MP: an access to this processor's start registers
/// (RBOX_SCRATCH1, RBOX_IREQ), by whom, and where this processor is.
void CEv7Csr::trace_mp(const char *what, u32 off, u64 v) {
  CAlphaCPU *self = cpu();
  char by[48] = "device";
  if (t_running_cpu)
    snprintf(by, sizeof(by), "cpu%d pc %" PRIx64, t_running_cpu->get_cpuid(),
             t_running_cpu->get_pc());
  printf("%%MVL-T-MP: %10lld us %s PID %u %s %s %016" PRIx64, mp_us(), by,
         m_pid, reg_name(off, m_21464), what, v);
  if (self && self != t_running_cpu)
    printf(" (PID %u %s pc %" PRIx64 " icount %" PRIu64 ")", m_pid,
           self->get_waiting() ? "parked" : "running", self->get_pc(),
           (u64)self->state.instruction_count);
  printf("\n");
}

/**
 * The I/O port's clock ratio. The CMM keeps a code for it at its memory's
 * area + 0x12a9 (CMM3 V2.7-5: smlan_set_freq's ratios looked up in its
 * table at 0x3fb0270, ratios 1.5 to 8.0 in halves, code = 2 x ratio - 2,
 * 1 to 14); the XSROM writes RBOX_IO_CFG = (code << 5) | 0xa001, then <2>,
 * <4> and <14> on and <13> off (0x4494-0x45ec), as the console's own
 * power_on_ior (0x2e1b30) does from that byte; and io7_init_all_sp
 * (0x2ee280) prints "IO7 North port speed is %d MHz" as the clock in MHz
 * over RBOX_IO_CFG<8:5> + 2, that is over twice the ratio. So the code
 * settles the field and its encoding.
 *
 * Which ratio a clock gets is the MBM's (SetFreq; its table was not found).
 * Three real start logs give one each: 216 MHz at 1300 MHz (a real ES47
 * 7/1300, test/platforms/es47/power-up.log: code 4, ratio 3.0), 210 MHz at
 * 1050 (the User Information's log: code 3) and 133 MHz at 533 (the SRM
 * reference's; 533 MHz and 2.0 are the CMM's own defaults: code 2). The
 * smallest code from 2 that keeps the North port at or under 216 MHz gives
 * all three [inference]: code 3 at 1000 MHz (200 MHz), 4 at 1150 (191).
 */
u32 ev7csr::io_clock_code(u32 mhz) {
  u32 code = 2;
  while (code < 14 && mhz / (code + 2) > 216)
    code++;
  return code;
}

CEv7Csr::CEv7Csr(CSystem *sys, CMarvel *marvel, u32 pid, GioManagement *gio)
    : m_sys(sys), m_marvel(marvel), m_pid(pid), m_gio(pid, gio) {
  m_21464 = marvel->mode_21464();
  m_tpus = marvel->tpus_per_block();
  m_gio.set_21464(m_21464);
  reset();
}

CAlphaCPU *CEv7Csr::cpu() const {
  if (CAlphaCPU *c = m_cpu.load(std::memory_order_acquire))
    return c;
  for (int i = 0; i < m_sys->get_cpu_num(); i++)
    if (m_sys->get_cpu(i)->get_pid() == m_pid) {
      m_cpu.store(m_sys->get_cpu(i), std::memory_order_release);
      return m_sys->get_cpu(i);
    }
  return nullptr;
}

/// A chip's TPUs are the processors numbered PID x TPUs + t (dense PIDs,
/// N19: the topology lays them out chip by chip).
CAlphaCPU *CEv7Csr::tpu_cpu(int t) const {
  if (t < 0 || t >= m_tpus)
    return nullptr;
  if (CAlphaCPU *c = m_tpu_cpu[t].load(std::memory_order_acquire))
    return c;
  const int i = (int)m_pid * m_tpus + t;
  if (i >= m_sys->get_cpu_num() || m_sys->get_cpu(i)->get_pid() != m_pid)
    return nullptr;
  m_tpu_cpu[t].store(m_sys->get_cpu(i), std::memory_order_release);
  return m_sys->get_cpu(i);
}

bool CEv7Csr::int_mask_tpu(u32 off, int *t) {
  if (off == RBOX_IMASK) {
    *t = 0;
    return true;
  }
  if (off < R_INT_MASK_TPU || off >= R_INT_MASK_TPU + 4 * 0x40 || (off & 0x3f))
    return false;
  *t = (int)((off - R_INT_MASK_TPU) / 0x40);
  return true;
}

/**
 * The state the XSROM leaves: every named register holds 0 -- the error
 * registers clean, the router and memory controllers as they read once
 * configured [guess: what a configured Zbox or router reads is not known;
 * the console reads none of them before its GIO conversation] -- except
 * the processor's own identity.
 */
void CEv7Csr::reset() {
  m_regs.clear();
  for (const named_reg &r : kRegs)
    m_regs[r.off] = 0;
  for (const named_reg &r : kMoreRegs)
    m_regs[r.off] = 0;
  for (u32 n = 0; n < route_entries(m_21464); n++)
    m_regs[RBOX_ROUTE + n * 0x10] = 0; // load_routes fills them
  // RBOX_WHOAMI: the PID [guess: the field's position is not known; the
  // console takes its PID from r28 and has not been seen to read this].
  m_regs[RBOX_WHOAMI] = m_pid;
  if (m_21464) {
    // The 21464's own registers, and R_WHOAMI as the IDS gives it: the
    // chip's coordinates, EW <9:5> and NS <4:0> [IDS p. 527; N19].
    for (const named_reg &r : kRegs21464)
      m_regs[r.off] = 0;
    for (u32 t = 1; t < 4; t++)
      m_regs[R_INT_MASK_TPU + t * 0x40] = 0;
    const CMarvelTopology &topo = m_marvel->topology();
    if (const ev7_node *n = topo.by_pid(m_pid, topo.count()))
      m_regs[RBOX_WHOAMI] = CMarvelTopology::whoami21464(n->ns, n->ew);
    for (int &l : m_lines8)
      l = 0;
    m_iids_refused = 0;
  }
  // RBOX_IO_CFG: <0> and <2> say an IO7 is on the I/O port. The console's
  // PALcode tests exactly those two (its interval-timer path, 0x3941c,
  // before it touches the IO7's POx_RST registers), and with them clear the
  // console reports "No Local I/O"; <8:5>, the clock ratio, is read()'s
  // (io_clock_code) [inference: the XSROM's <4>, <14> and <15> configure
  // the port and are not modelled].
  m_regs[RBOX_IO_CFG] = m_io7 ? 5 : 0;
  m_intq.clear();
  m_it.store(0, std::memory_order_relaxed);
  // BBOX_CTL<6:0>: the L2's enabled ways, 256 KB each, which the XSROM set
  // (the CMM's "cache_enable_mask") and get_bcache_size_pid counts.
  const cpu_model *model =
      cpu() ? &cpu()->model() : find_cpu_model(m_sys->platform().cpu_model);
  const u32 ways = model ? model->l2_kb / 256 : 7;
  m_regs[0x1c000] = (U64(1) << (ways > 7 ? 7 : ways)) - 1;
  m_gio.reset();
  m_lines = 0;
  m_start_hi = 0;
  m_start_hi_valid = false;
  m_handoff.reset();
  m_sync_waiting = true; // nothing held: the first broadcast goes through
  m_sync_held = false;
}

u64 CEv7Csr::reg(u32 off) const {
  auto it = m_regs.find(off);
  return it == m_regs.end() ? 0 : it->second;
}

u64 CEv7Csr::read(u32 off, int dsize) {
  // VCTL, Tarantula's vector enable, on a processor that has it in
  // EV7-compatible mode (the `ev8` row): VEN <0>, <63:1> RAZ
  // (docs/platforms/ev8-isa.md, section 2). On any other row the offset is
  // an unknown register, as before, and in 21464 mode it is TPU 0's
  // R_INT_MASK (N22): the native EV8's VCTL is the IPR 0xAC (N8). The
  // processor keeps the value; no lock of this block is needed.
  if (!m_21464 && off == tarantula::kVctlOffset) {
    CAlphaCPU *c = cpu();
    if (c && c->vctl_is_csr()) {
      const u64 v = c->vctl_csr_read();
      if (trace_csr_on())
        trace_csr(m_pid, off, false, v, m_21464);
      return dsize == 32 ? (v & 0xffffffff) : v;
    }
  }
  std::unique_lock<std::mutex> lk(m_lock);
  u64 v;
  if (m_21464) {
    // TPU 0's R_INT_MASK at both of its offsets (N22); the EV7 console's
    // GIO lock is not there: our PALcode's is a word in memory (2.5).
    if (off == R_INT_MASK_TPU)
      off = RBOX_IMASK;
    if (off == GIO_LOCK) {
      m_sys->trace_unknown("EV7 CSR", m_marvel->map().csr_base(m_pid) | off,
                           dsize, false, 0, nullptr);
      return 0;
    }
  }
  switch (off) {
  case RBOX_SCRATCH1:
    v = read_scratch(lk);
    break;
  case GIO_CFG:
    v = m_gio.read_cfg();
    break;
  case GIO_DAT:
    v = m_gio.read_dat();
    break;
  case GIO_CTL:
    v = m_gio.read_ctl();
    break;
  case GIO_LOCK:
    v = m_gio.read_lock();
    break;
  case RBOX_INTQ:
    // The oldest IID an IO7 sent, valid in <24>; 0 when none waits.
    v = m_intq.empty() ? 0 : (m_intq.front() | INTQ_VALID);
    break;
  case RBOX_IO_CFG:
    // With an IO7 on the port, <8:5> the I/O clock ratio's code for this
    // processor's clock (io_clock_code), as the XSROM sets it; not in 21464
    // mode, whose register is not known to have the field.
    v = reg(off);
    if (m_io7 && !m_21464)
      if (CAlphaCPU *c = cpu())
        v |= (u64)io_clock_code((u32)(c->get_speed() / 1000000))
             << IO_CFG_RATIO_SHIFT;
    break;
  case 0x28020:
    // The revision probe (see kMoreRegs): with chip ID 2, bit 19 tells
    // "EV7 rev 2.2" (set) from "2.1" (clear); no row is a 2.2 part, and the
    // EV7z has its own chip ID (CpuModels.cpp). Reads 0.
    v = reg(off) & ~(U64(1) << 19);
    break;
  default: {
    auto it = m_regs.find(off);
    if (it == m_regs.end()) {
      m_sys->trace_unknown("EV7 CSR", m_marvel->map().csr_base(m_pid) | off,
                           dsize, false, 0, nullptr);
      return 0;
    }
    v = it->second;
  }
  }
  if (dsize == 32)
    v &= 0xffffffff;
  if (trace_csr_on())
    trace_csr(m_pid, off, false, v, m_21464);
  return v;
}

void CEv7Csr::write(u32 off, int dsize, u64 data) {
  if (!m_21464 && off == tarantula::kVctlOffset) { // VCTL (read() above)
    CAlphaCPU *c = cpu();
    if (c && c->vctl_is_csr()) {
      if (trace_csr_on())
        trace_csr(m_pid, off, true, data, m_21464);
      c->vctl_csr_write(data);
      return;
    }
  }
  if (m_21464 && off == RBOX_INTQ) {
    // R_INT_QUE: a write removes the head [IDS p. 528], and an IO7 whose
    // IID the full queue refused sends it again (N21), outside this lock:
    // the IO7 posts with its own held.
    bool room;
    {
      std::lock_guard<std::mutex> g(m_lock);
      if (trace_csr_on())
        trace_csr(m_pid, off, true, data, m_21464);
      m_regs[RBOX_INTQ] = data;
      room = !m_intq.empty();
      if (room)
        m_intq.pop_front();
      if (m_intq.empty())
        m_regs[RBOX_INT] &= ~INT8_QUEUE;
      update_irq();
    }
    if (room)
      m_marvel->resend_iids();
    return;
  }
  if (off == RBOX_IREQ) {
    // A halt is not done with when the register is written (see "The
    // halt"): the wait for its targets is outside this block's lock.
    HaltWait hw;
    {
      std::lock_guard<std::mutex> g(m_lock);
      if (trace_csr_on())
        trace_csr(m_pid, off, true, data, m_21464);
      if (m_21464)
        write_ireq_21464(data, hw);
      else
        write_ireq(data, hw);
    }
    halt_wait(hw);
    return;
  }
  std::lock_guard<std::mutex> g(m_lock);
  if (trace_csr_on() && (reg_name(off, m_21464) || off == 0x28020))
    trace_csr(m_pid, off, true, data, m_21464);
  if (m_21464 && write_21464(off, dsize, data))
    return;
  switch (off) {
  case GIO_CFG:
    m_gio.write_cfg(data);
    return;
  case GIO_DAT:
    m_gio.write_dat(data);
    return;
  case GIO_CTL:
    m_gio.write_ctl(data);
    return;
  case GIO_LOCK:
    m_gio.write_lock(data);
    return;
  case RBOX_WHOAMI:
    return; // read-only
  case RBOX_INT:
    m_regs[RBOX_INT] &= ~data; // write one to clear
    if (data & INT_SYNC) {     // waiting for the clock rendezvous
      m_sync_waiting = !m_sync_held;
      if (m_sync_held)
        m_regs[RBOX_INT] |= INT_SYNC; // the broadcast that came first
      m_sync_held = false;
    }
    if (!m_intq.empty())
      m_regs[RBOX_INT] |= INT_IOQ; // still something in the queue
    update_irq();
    return;
  case RBOX_INTQ:
    // The PALcode writes back each IID it has taken (0x398f8): the queue
    // moves on [inference].
    m_regs[RBOX_INTQ] = data;
    if (!m_intq.empty())
      m_intq.pop_front();
    return;
  case RBOX_IO_CFG:
    return; // the port's configuration, the XSROM's [read-only here]
  case RBOX_IMASK:
    m_regs[RBOX_IMASK] = data;
    update_irq();
    return;
  case RBOX_SCRATCH1:
    write_scratch(data);
    return;
  case RBOX_IT:
    m_regs[RBOX_IT] = data;
    m_it.store(data, std::memory_order_relaxed);
    return;
  default:
    break;
  }
  auto it = m_regs.find(off);
  if (it == m_regs.end()) {
    m_sys->trace_unknown("EV7 CSR", m_marvel->map().csr_base(m_pid) | off,
                         dsize, true, data, nullptr);
    return;
  }
  it->second =
      (dsize == 32)
          ? ((it->second & ~U64(0xffffffff)) | (data & U64(0xffffffff)))
          : data;
}

/**
 * 21464 mode (docs/platforms/ev8-native.md, 2.4, N21 and N22): the
 * interrupt registers. With m_lock held; false for a register the EV7's
 * code serves alike (GIO, R_WHOAMI, the scratch registers, the timer, the
 * stored ones).
 *
 *   R_INT_STAT (0x050)  32 bits, written with ones to clear; <12> clears
 *                       only as the queue empties.
 *   R_INT_MASK[t]       TPU 0's at 0x060 and 0x3F0000, TPU t's at 0x3F0000
 *                       + 0x40 x t: 32 bits.
 *   R_INT_REQ (0x070)   sets its bits in R_INT_STAT: the IPIs <3:0>, the
 *                       halts <11:8> and the software bits <31:18>, the
 *                       fields N22 has R_INT_REQ set [inference: its table
 *                       names no other source for them]; reads back what
 *                       was last written, as the EV7's.
 *   R_INT_QUEADD (0x090) an IID into the queue; a full queue refuses it
 *                       (WrIONack [IDS p. 528]), which a processor's store
 *                       cannot see: dropped and counted [decision].
 */
bool CEv7Csr::write_21464(u32 off, int dsize, u64 data) {
  (void)dsize;
  if (off == R_INT_MASK_TPU)
    off = RBOX_IMASK;
  int t;
  if (int_mask_tpu(off, &t)) {
    m_regs[off] = data & 0xffffffff;
    update_irq();
    return true;
  }
  switch (off) {
  case GIO_LOCK:
    m_sys->trace_unknown("EV7 CSR", m_marvel->map().csr_base(m_pid) | off,
                         dsize, true, data, nullptr);
    return true;
  case RBOX_INT:
    m_regs[RBOX_INT] &= ~(data & 0xffffffff & ~INT8_QUEUE);
    update_irq();
    return true;
  case RBOX_INTA:
    m_regs[RBOX_INTA] = data;
    if (!queue_iid(data)) {
      ++m_iids_refused;
      if (trace_csr_on() || CSystem::trace_mp_on())
        printf("%%MVL-T-CSR: pid %u R_INT_QUEADD %06" PRIx64
               " refused: the queue is full (%" PRIu64 " refused)\n",
               m_pid, data & (INTQ_VALID - 1), m_iids_refused);
    }
    update_irq();
    return true;
  }
  return false;
}

/// A request to this processor: the bits appear in its RBOX_INT. The
/// register itself reads back what was last written.
void CEv7Csr::write_ireq(u64 data, HaltWait &hw) {
  if (CSystem::trace_mp_on() && (data & ~INT_SYNC))
    trace_mp("write =", RBOX_IREQ, data);
  m_regs[RBOX_IREQ] = data;
  if (data & INT_HALT)
    note_halt(cpu(), hw);
  if (data & INT_SYNC) {
    if (m_sync_waiting)
      m_sync_waiting = false; // the rendezvous it is waiting for
    else {
      m_sync_held = true; // not waiting yet: kept for its clear
      data &= ~INT_SYNC;
    }
  }
  m_regs[RBOX_INT] |= data;
  update_irq();
}

/// 21464 mode: R_INT_REQ (write_21464's table); <8 + t> halts TPU t.
void CEv7Csr::write_ireq_21464(u64 data, HaltWait &hw) {
  if (CSystem::trace_mp_on())
    trace_mp("write =", RBOX_IREQ, data);
  m_regs[RBOX_IREQ] = data;
  for (int t = 0; t < m_tpus && t < 4; t++)
    if (data & (U64(0x100) << t))
      note_halt(tpu_cpu(t), hw);
  m_regs[RBOX_INT] |= data & INT8_REQUESTABLE;
  update_irq();
}

/**
 * The halt. At `init` the console's PALcode writes RBOX_IREQ<22> to every
 * processor, itself first (0x3f3f4), takes its own halt and starts the
 * console again, which sets up its data afresh: its scheduler's spinlock
 * (0x428180 in SRM V7.3-1) among them, 0.4 to 1.1 ms after the first write
 * when processor 0 runs throughout (ALPHABOX_TRACE_MP; lab/runs/stall-r1).
 * On the machine the other processors have stopped long before: each takes
 * the halt at its next instruction, since the console's PALcode leaves EI4
 * enabled at every IPL, 31 included (its IPL table, PAL_BASE + 0xd00). Here
 * each is a host thread, and one the host has not run for a while took its
 * halt up to 36 ms after the request, running the console's code on the
 * data set up afresh until then. When that code was the spinlock's
 * acquire, it took the lock's interlock word, halted holding it, and the
 * console waited for it for ever (`init` on a 16-processor ES80, 2026-10-26).
 *
 * So a processor that asks another, running, one to halt goes on only once
 * that one has taken an interrupt since (CAlphaCPU::interrupts_taken), or
 * can no longer run native code before it next looks for one: parked, in
 * PALmode, being stopped, the machine being reset or held by a debugger.
 * The wait is bounded at 100 ms for each processor asked, with a warning:
 * one that has EI4 masked would not take it on the machine either.
 */
void CEv7Csr::note_halt(CAlphaCPU *target, HaltWait &hw) {
  if (!target || !t_running_cpu || target == t_running_cpu ||
      target->get_waiting() || hw.n >= 4)
    return;
  hw.cpu[hw.n] = target;
  hw.seen[hw.n] = target->interrupts_taken();
  hw.n++;
}

void CEv7Csr::halt_wait(const HaltWait &hw) {
  for (int i = 0; i < hw.n; i++) {
    CAlphaCPU *c = hw.cpu[i];
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    if (!c->wait_interrupt_taken(hw.seen[i], until))
      printf("%%MVL-W-HALT: processor %d has not taken the halt processor %d "
             "sent it within 100 ms; going on.\n",
             c->get_cpuid(), t_running_cpu ? t_running_cpu->get_cpuid() : -1);
  }
}

/// With m_lock held.
bool CEv7Csr::queue_iid(u64 iid) {
  if (m_21464 && m_intq.size() >= kIntQueue21464)
    return false;
  m_intq.push_back(iid & (INTQ_VALID - 1));
  m_regs[RBOX_INT] |= m_21464 ? INT8_QUEUE : INT_IOQ;
  return true;
}

bool CEv7Csr::post_iid(u64 iid) {
  std::lock_guard<std::mutex> g(m_lock);
  if (!queue_iid(iid)) {
    ++m_iids_refused;
    return false;
  }
  update_irq();
  return true;
}

void CEv7Csr::gio_interrupt() { request(m_21464 ? INT8_GIO : INT_GIO); }

void CEv7Csr::set_io7_attached(bool on) {
  std::lock_guard<std::mutex> g(m_lock);
  m_io7 = on;
  m_regs[RBOX_IO_CFG] = on ? 5 : 0;
}

/**
 * Each entry n < 0x100 routes to PID n. The XSROM sets them up ("Configure
 * RBOX Routes", "Inverse Route Setup" in a real power-up log) and leaves a
 * copy in the CMM's memory, which the console hands to the operating system
 * (Cmm.cpp); the register holds the same fields here [guess: the
 * register's own layout is not known, only the copy's (Topology.cpp)].
 */
void CEv7Csr::load_routes(const CMarvelTopology &topology, int present) {
  std::lock_guard<std::mutex> g(m_lock);
  // 21464 mode: R_ROUT, entry n the route to PID n of the 512, in the IDS's
  // layout (N19, Topology.cpp route21464); the 20 sharing-mask entries 0.
  if (m_21464) {
    for (u32 n = 0; n < R_ROUT_ENTRIES; n++)
      m_regs[RBOX_ROUTE + n * 0x10] =
          n < 512 ? topology.route21464(m_pid, n, present) : 0;
    return;
  }
  for (u32 n = 0; n < RBOX_ROUTE_ENTRIES; n++)
    m_regs[RBOX_ROUTE + n * 0x10] =
        n < 0x100 ? topology.route(m_pid, n, present) : 0;
}

void CEv7Csr::request(u64 bits) {
  std::lock_guard<std::mutex> g(m_lock);
  m_regs[RBOX_INT] |= bits;
  update_irq();
}

/// With m_lock held: RBOX_INT & RBOX_IMASK is `pending` now; note when an
/// enabled interrupt began to wait (ALPHABOX_TRACE_RBOX, check_pending).
void CEv7Csr::note_pending(u64 pending) {
  if (pending && !m_pend) {
    m_pend_since = std::chrono::steady_clock::now();
    m_pend_reported = false;
  } else if (!pending && m_pend && m_pend_reported) {
    printf("%%MVL-T-RBOX: PID %u took its interrupt after %lld ms\n", m_pid,
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - m_pend_since)
               .count());
  }
  m_pend = pending != 0;
}

void CEv7Csr::check_pending(int ms) {
  std::lock_guard<std::mutex> g(m_lock);
  if (!m_pend || m_pend_reported)
    return;
  const auto age = std::chrono::steady_clock::now() - m_pend_since;
  if (age < std::chrono::milliseconds(ms))
    return;
  m_pend_reported = true;
  CAlphaCPU *c = cpu();
  printf("%%MVL-T-RBOX: PID %u has had an enabled interrupt pending %lld ms: "
         "RBOX_INT %016" PRIx64 " IMASK %016" PRIx64 " lines %02x",
         m_pid,
         (long long)std::chrono::duration_cast<std::chrono::milliseconds>(age)
             .count(),
         reg(RBOX_INT), reg(RBOX_IMASK), m_lines);
  if (c) {
    printf("; cpu pc %016" PRIx64 " eir %02x eien %02x check_int %d icount "
           "%" PRIu64,
           c->get_pc(), (unsigned)c->state.eir.load(), (unsigned)c->state.eien,
           (int)c->state.check_int.load(), (u64)c->state.instruction_count);
#ifdef ES40_JIT
    printf(" idle-sleeping %d", (int)c->m_idle_sleeping.load());
#endif
  }
  printf("\n");
  fflush(stdout);
}

/**
 * The interval timer. The console's PALcode clears RBOX_IT early, takes
 * RBOX_INT<15> as the clock interrupt, and on each one reads RBOX_IT<31:22>
 * as a count of further ticks it missed (0x39344-0x393cc: none when 0 or
 * 0x3ff). The console's C code writes n, which follows the clock: 7 at
 * 1000 MHz and 9 at 1300 MHz (n + 1 = ceil(cpu_hz / 2^27), the fastest rate
 * not above 1024 Hz, fits both [inference]; docs/platforms/marvel.md,
 * "Captures from a real ES47"), and tells the operating system (HWRPB
 * intr_freq, get_iclk_freq at 0x2e2270) that the timer runs at
 * cpu_hz / ((n + 1) * 2^17), times another 1/4 on a revision 1.0 part: so
 * RBOX_IT<21:0> is taken as n, a period of n + 1 times 2^17 processor
 * cycles [inference]. Every processor's schedule (its own,
 * cpu/TickSchedule.hpp, in processor 0's phase) runs at PID 0's period
 * (CMarvel::interval_period_ns); a processor whose RBOX_IT is zero takes no
 * tick, with the missed-tick count left as written.
 */
u64 CEv7Csr::interval_period_ns(u64 cpu_hz) const {
  // 21464 mode: R_INTER_TIM's ITV <7:0> [IDS p. 528], the same period for
  // its value (2.4, N22) [inference]; zero is off, as on the EV7
  // [inference].
  const u64 n = m_it.load(std::memory_order_relaxed) &
                (m_21464 ? U64(0xff) : U64(0x3fffff));
  if (!n || !cpu_hz)
    return 0;
  return (n + 1) * (U64(1) << 17) * 1000000000 / cpu_hz;
}

void CEv7Csr::interval_tick(int tpu) {
  std::lock_guard<std::mutex> g(m_lock);
  // 21464 mode: one timer per chip, a tick for each TPU in R_INT_STAT<7:4>
  // (N22), which each TPU clears. Each TPU's thread sets its own bit; TPU
  // 0's sets those of the TPUs the chip has no processor for, which the
  // chip's one timer sets too.
  if (m_21464) {
    if (!(reg(RBOX_IT) & 0xff))
      return;
    u64 bit = U64(0x10) << tpu;
    ++m_ticks;
    if (m_regs[RBOX_INT] & bit)
      ++m_ticks_merged;
    if (tpu == 0)
      for (int t = 1; t < 4; t++)
        if (!tpu_cpu(t))
          bit |= U64(0x10) << t;
    m_regs[RBOX_INT] |= bit;
    update_irq();
    return;
  }
  if (!reg(RBOX_IT))
    return;
  ++m_ticks;
  if (m_regs[RBOX_INT] & INT_IT)
    ++m_ticks_merged; // the last one not taken yet: this one is lost
  m_regs[RBOX_INT] |= INT_IT;
  update_irq();
}

void CEv7Csr::tick_stats() {
  std::lock_guard<std::mutex> g(m_lock);
  printf("%%MVL-T-RBOX: PID %u interval ticks %" PRIu64 ", merged into one "
         "not yet taken %" PRIu64 "\n",
         m_pid, m_ticks, m_ticks_merged);
}

void CEv7Csr::update_irq() {
  if (m_21464) {
    update_irq_21464();
    return;
  }
  CAlphaCPU *c = cpu();
  if (!c)
    return;
  const u64 pending = reg(RBOX_INT) & reg(RBOX_IMASK);
  note_pending(pending);
  int lines = 0;
  for (int l = 0; l < 6; l++)
    if (pending & line_mask(l))
      lines |= 1 << l;
  const int changed = lines ^ m_lines;
  m_lines = lines;
  for (int l = 0; l < 6; l++)
    if (changed & (1 << l))
      c->irq_h(l, (lines >> l) & 1, 0);
}

/**
 * 21464 mode (N22): TPU t's level L is asserted while a bit of level L is
 * set in R_INT_STAT & R_INT_MASK[t]; its six lines go into its own
 * ISUM<EI>, which its own IER_CM<EIEN> gates (the `ev8n` row's IPRs, N7).
 * So an IPI to TPU t (R_INT_STAT<t>) reaches whichever TPUs' masks enable
 * that bit, and only them.
 */
void CEv7Csr::update_irq_21464() {
  const u64 stat = reg(RBOX_INT) & 0xffffffff;
  u64 any = 0;
  for (int t = 0; t < m_tpus && t < 4; t++) {
    const u64 pending =
        stat & reg(t ? R_INT_MASK_TPU + (u32)t * 0x40 : RBOX_IMASK);
    any |= pending;
    CAlphaCPU *c = tpu_cpu(t);
    if (!c)
      continue;
    int lines = 0;
    for (int l = 0; l < 6; l++)
      if (pending & level_mask_21464(l))
        lines |= 1 << l;
    const int changed = lines ^ m_lines8[t];
    m_lines8[t] = lines;
    for (int l = 0; l < 6; l++)
      if (changed & (1 << l))
        c->irq_h(l, (lines >> l) & 1, 0);
  }
  note_pending(any);
}

/**
 * The start hand-off (StartHandoff.hpp). The console's 50 reads for the echo
 * have no delay between them: a few microseconds of the host's time here,
 * where on the machine a read of another EV7's register goes out through the
 * router while the secondary goes round a local polling loop. When `init`
 * restarted four processors, the secondaries, polling in their PALcode's
 * loop (0x390e4) since the console halted them, answered 7 to 16 us after
 * the console's write; its 50 reads lasted 9 to 44 us, and a start was lost
 * whenever they ended first (ALPHABOX_TRACE_MP, docs/platforms/marvel.md,
 * M11). The hand-off keeps the hardware's order: a processor polling its
 * register sees what another processor writes there before that processor's
 * next read of it returns.
 */
u64 CEv7Csr::read_scratch(std::unique_lock<std::mutex> &lk) {
  CAlphaCPU *self = cpu();
  CAlphaCPU *by = t_running_cpu;
  if (self && by == self) {
    const u64 v = m_regs[RBOX_SCRATCH1];
    m_handoff.owner_read(v, self->state.instruction_count);
    if (CSystem::trace_mp_on())
      trace_mp_read(v, 0, true);
    return v;
  }
  long long waited = 0;
  bool in_time = true;
  if (self && by && m_handoff.open()) {
    const auto t0 = std::chrono::steady_clock::now();
    // Not while the owner is parked (scratch_written answers for it) or the
    // machine is being reset; a processor being stopped is let go by the
    // bound.
    in_time = m_handoff.remote_read(lk, by->get_cpuid(), [&] {
      return !self->get_waiting() && !m_sys->IsSystemResetRequested();
    });
    waited = (long long)std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::steady_clock::now() - t0)
                 .count();
    if (!in_time)
      printf("%%MVL-W-START: PID %u did not look at its RBOX_SCRATCH1 within "
             "a second of another processor's write; the reads go on.\n",
             m_pid);
  }
  const u64 v = m_regs[RBOX_SCRATCH1];
  if (CSystem::trace_mp_on())
    trace_mp_read(v, waited, in_time);
  return v;
}

void CEv7Csr::write_scratch(u64 v) {
  CAlphaCPU *self = cpu();
  if (CSystem::trace_mp_on() &&
      (t_running_cpu != self || v != m_regs[RBOX_SCRATCH1]))
    trace_mp("write =", RBOX_SCRATCH1, v);
  m_regs[RBOX_SCRATCH1] = v;
  if (self && t_running_cpu == self)
    m_handoff.owner_wrote();
  else if (self && !self->get_waiting()) { // a parked one is answered below
    m_handoff.remote_wrote(self->state.instruction_count);
    // The hand-off waits for the owner to look: one asleep in QUIESCE (the
    // 21464's) looks now.
    self->quiesce_wake(WATCH_OTHER);
  }
  scratch_written(v);
}

/// ALPHABOX_TRACE_MP: a read of RBOX_SCRATCH1. A poll repeats one read, so a
/// reader's first read of each value is reported, with how long it waited
/// for the owner to look.
void CEv7Csr::trace_mp_read(u64 v, long long waited_us, bool in_time) {
  const int who = t_running_cpu ? t_running_cpu->get_cpuid() : 0;
  if (who < 0 || who >= 32)
    return;
  if (m_mp_seen_valid[who] && m_mp_seen[who] == v && !waited_us)
    return;
  m_mp_seen[who] = v;
  m_mp_seen_valid[who] = true;
  char what[64] = "read ->";
  if (waited_us)
    snprintf(what, sizeof(what), "read (waited %lld us%s) ->", waited_us,
             in_time ? "" : ", gave up");
  trace_mp(what, RBOX_SCRATCH1, v);
}

/**
 * The XSROM's last command (0x50, at 0x6650 of MVXSROM_V1_0_31.BIN less
 * its header) leaves a processor polling its RBOX_SCRATCH1 for a start
 * address: 0xf1 in bits 31:24 with the upper 24 bits of the address, which
 * it echoes as 0xf2; then 0xf3 with the lower 24, after which it enters the
 * address in PALmode. The console starts its secondaries this way (and its
 * own PALcode waits the same way when it parks one, 0x390d4). For a
 * processor still parked in the emulator this is that loop's part.
 */
void CEv7Csr::scratch_written(u64 v) {
  CAlphaCPU *c = cpu();
  if (!c || !c->get_waiting())
    return; // a running processor reads its scratch register itself
  const u32 tag = (u32)(v >> 24) & 0xff;
  if (tag == 0xf1) {
    m_start_hi = v & 0xffffff;
    m_start_hi_valid = true;
    m_regs[RBOX_SCRATCH1] = (U64(0xf2) << 24) | m_start_hi; // the echo
  } else if (tag == 0xf3 && m_start_hi_valid) {
    const u64 addr = (m_start_hi << 24) | (v & 0xffffff);
    m_start_hi_valid = false;
    printf("%%MVL-I-START: PID %u started at %011" PRIx64
           " through RBOX_SCRATCH1.\n",
           m_pid, addr);
    ev7::xsrom_handoff(c, m_pid, 0);
    c->set_pc(addr | 1);
    c->stop_waiting();
  }
}

void CEv7Csr::save_state(FILE *f) {
  std::lock_guard<std::mutex> g(m_lock);
  const u32 n = (u32)m_regs.size();
  fwrite(&n, sizeof(n), 1, f);
  for (const auto &kv : m_regs) {
    fwrite(&kv.first, sizeof(kv.first), 1, f);
    fwrite(&kv.second, sizeof(kv.second), 1, f);
  }
  const CGioPort::State s = m_gio.save();
  fwrite(&s, sizeof(s), 1, f);
  fwrite(&m_start_hi, sizeof(m_start_hi), 1, f);
  fwrite(&m_start_hi_valid, sizeof(m_start_hi_valid), 1, f);
  const u32 q = (u32)m_intq.size();
  fwrite(&q, sizeof(q), 1, f);
  for (u64 iid : m_intq)
    fwrite(&iid, sizeof(iid), 1, f);
}

bool CEv7Csr::restore_state(FILE *f) {
  std::lock_guard<std::mutex> g(m_lock);
  u32 n = 0;
  if (fread(&n, sizeof(n), 1, f) != 1)
    return false;
  m_regs.clear();
  for (u32 i = 0; i < n; i++) {
    u32 off;
    u64 v;
    if (fread(&off, sizeof(off), 1, f) != 1 || fread(&v, sizeof(v), 1, f) != 1)
      return false;
    m_regs[off] = v;
  }
  CGioPort::State s;
  if (fread(&s, sizeof(s), 1, f) != 1 ||
      fread(&m_start_hi, sizeof(m_start_hi), 1, f) != 1 ||
      fread(&m_start_hi_valid, sizeof(m_start_hi_valid), 1, f) != 1)
    return false;
  m_gio.restore(s);
  u32 q = 0;
  if (fread(&q, sizeof(q), 1, f) != 1)
    return false;
  m_intq.clear();
  for (u32 i = 0; i < q; i++) {
    u64 iid;
    if (fread(&iid, sizeof(iid), 1, f) != 1)
      return false;
    m_intq.push_back(iid);
  }
  // Not in the file: a processor that may be waiting for the clock
  // rendezvous (<23> clear) is let through by the next broadcast.
  m_sync_waiting = !(reg(RBOX_INT) & INT_SYNC);
  m_sync_held = false;
  m_lines = 0;
  for (int &l : m_lines8)
    l = 0;
  update_irq();
  return true;
}
