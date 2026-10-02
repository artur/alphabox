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
 **/
#include "StdAfx.hpp"

#include "AlphaCPU.hpp"
#include "CpuModel.hpp"
#include "Ev7.hpp"
#include "Ev7Csr.hpp"
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

bool is_route(u32 off) {
  return off >= RBOX_ROUTE && off < RBOX_ROUTE + RBOX_ROUTE_ENTRIES * 0x10 &&
         !(off & 0xf);
}

const char *reg_name(u32 off) {
  if (is_route(off))
    return "RBOX_ROUTE";
  for (const named_reg &r : kRegs)
    if (r.off == off)
      return r.name;
  for (const named_reg &r : kMoreRegs)
    if (r.off == off)
      return r.name;
  switch (off) {
  case GIO_LOCK:
    return "GIO lock";
  }
  return nullptr;
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

void trace_csr(u32 pid, u32 off, bool write, u64 v) {
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
  const char *name = reg_name(off);
  char nm[32];
  if (!name) {
    snprintf(nm, sizeof(nm), "+%05x", off);
    name = nm;
  }
  printf("%%MVL-T-CSR: pid %u %s %-22s %s %016" PRIx64 "%s pc=%" PRIx64 "\n",
         pid, write ? "write" : "read ", name, write ? "=" : "->", v,
         n > 3 ? " (repeated)" : "", pc);
}

} // namespace

CEv7Csr::CEv7Csr(CSystem *sys, u32 pid, GioManagement *gio)
    : m_sys(sys), m_pid(pid), m_gio(pid, gio) {
  reset();
}

CAlphaCPU *CEv7Csr::cpu() const {
  for (int i = 0; i < m_sys->get_cpu_num(); i++)
    if (m_sys->get_cpu(i)->get_pid() == m_pid)
      return m_sys->get_cpu(i);
  return nullptr;
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
  for (u32 n = 0; n < RBOX_ROUTE_ENTRIES; n++)
    m_regs[RBOX_ROUTE + n * 0x10] = 0; // load_routes fills them
  // RBOX_WHOAMI: the PID [guess: the field's position is not known; the
  // console takes its PID from r28 and has not been seen to read this].
  m_regs[RBOX_WHOAMI] = m_pid;
  // RBOX_IO_CFG: <0> and <2> say an IO7 is on the I/O port. The console's
  // PALcode tests exactly those two (its interval-timer path, 0x3941c,
  // before it touches the IO7's POx_RST registers), and with them clear the
  // console reports "No Local I/O" [inference: the other bits configure the
  // port and are not modelled].
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
}

u64 CEv7Csr::reg(u32 off) const {
  auto it = m_regs.find(off);
  return it == m_regs.end() ? 0 : it->second;
}

u64 CEv7Csr::read(u32 off, int dsize) {
  std::lock_guard<std::mutex> g(m_lock);
  u64 v;
  switch (off) {
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
  case 0x28020:
    // The revision probe (see kMoreRegs): with chip ID 2, bit 19 tells
    // "EV7 rev 2.2" (set) from "2.1" (clear); no row is a 2.2 part, and the
    // EV7z has its own chip ID (CpuModels.cpp). Reads 0.
    v = reg(off) & ~(U64(1) << 19);
    break;
  default: {
    auto it = m_regs.find(off);
    if (it == m_regs.end()) {
      m_sys->trace_unknown("EV7 CSR", ev7::csr_base(m_pid) | off, dsize, false,
                           0, nullptr);
      return 0;
    }
    v = it->second;
  }
  }
  if (dsize == 32)
    v &= 0xffffffff;
  if (trace_csr_on())
    trace_csr(m_pid, off, false, v);
  return v;
}

void CEv7Csr::write(u32 off, int dsize, u64 data) {
  std::lock_guard<std::mutex> g(m_lock);
  if (trace_csr_on() && (reg_name(off) || off == 0x28020))
    trace_csr(m_pid, off, true, data);
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
  case RBOX_IREQ:
    // A request to this processor: the bits appear in its RBOX_INT. The
    // register itself reads back what was last written.
    m_regs[RBOX_IREQ] = data;
    m_regs[RBOX_INT] |= data;
    update_irq();
    return;
  case RBOX_SCRATCH1:
    m_regs[RBOX_SCRATCH1] = data;
    scratch_written(data);
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
    m_sys->trace_unknown("EV7 CSR", ev7::csr_base(m_pid) | off, dsize, true,
                         data, nullptr);
    return;
  }
  it->second =
      (dsize == 32)
          ? ((it->second & ~U64(0xffffffff)) | (data & U64(0xffffffff)))
          : data;
}

void CEv7Csr::post_iid(u64 iid) {
  std::lock_guard<std::mutex> g(m_lock);
  m_intq.push_back(iid & (INTQ_VALID - 1));
  m_regs[RBOX_INT] |= INT_IOQ;
  update_irq();
}

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
  for (u32 n = 0; n < RBOX_ROUTE_ENTRIES; n++)
    m_regs[RBOX_ROUTE + n * 0x10] =
        n < 0x100 ? topology.route(m_pid, n, present) : 0;
}

void CEv7Csr::request(u64 bits) {
  std::lock_guard<std::mutex> g(m_lock);
  m_regs[RBOX_INT] |= bits;
  update_irq();
}

/**
 * The interval timer. The console's PALcode clears RBOX_IT early, takes
 * RBOX_INT<15> as the clock interrupt, and on each one reads RBOX_IT<31:22>
 * as a count of further ticks it missed (0x39344-0x393cc: none when 0 or
 * 0x3ff). The console's C code writes 7, and tells the operating system
 * (HWRPB intr_freq, get_iclk_freq at 0x2e2270) that the timer runs at
 * cpu_hz / ((n + 1) * 2^17), times another 1/4 on a revision 1.0 part: so
 * RBOX_IT<21:0> is taken as n, a period of n + 1 times 2^17 processor
 * cycles [inference]. The machine's schedule (CPU 0's, cpu/AlphaCPU.hpp)
 * runs at PID 0's period (CMarvel::interval_period_ns); a processor whose
 * RBOX_IT is zero takes no tick, with the missed-tick count left as written.
 */
u64 CEv7Csr::interval_period_ns(u64 cpu_hz) const {
  const u64 n = m_it.load(std::memory_order_relaxed) & 0x3fffff;
  if (!n || !cpu_hz)
    return 0;
  return (n + 1) * (U64(1) << 17) * 1000000000 / cpu_hz;
}

void CEv7Csr::interval_tick() {
  std::lock_guard<std::mutex> g(m_lock);
  if (!reg(RBOX_IT))
    return;
  m_regs[RBOX_INT] |= INT_IT;
  update_irq();
}

void CEv7Csr::update_irq() {
  CAlphaCPU *c = cpu();
  if (!c)
    return;
  const u64 pending = reg(RBOX_INT) & reg(RBOX_IMASK);
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
  m_lines = 0;
  update_irq();
  return true;
}
