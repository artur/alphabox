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
 * The ES47's CMM (Cmm.hpp). Addresses and routine names in the comments are
 * the console's (SRM V7.3-1 running image; docs/platforms/marvel.md, M4).
 **/
#include "StdAfx.hpp"

#include "AlphaCPU.hpp"
#include "Cmm.hpp"
#include "Ev7.hpp"
#include "Marvel.hpp"
#include "Serial.hpp"
#include "System.hpp"
#include "Topology.hpp"

#include <chrono>
#include <cstdarg>
#include <ctime>

namespace {

// --- The far-side registers (GIO register numbers) ---------------------------
constexpr u32 R_STATUS = 0x0;  ///< status and control
constexpr u32 R_DATA = 0x1;    ///< byte window: 16-bit data
constexpr u32 R_ADDRESS = 0x2; ///< byte window: CMM address
constexpr u32 R_TX = 0x4;      ///< console terminal: a character out
constexpr u32 R_RX = 0x5;      ///< console terminal: a character in
constexpr u32 R_STATE = 0x8;   ///< the PALcode's state and interrupt enables
constexpr u32 R_REASON = 0x9;  ///< read: the terminal interrupts pending
constexpr u32 R_COMM = 0xb;    ///< <0>: the console asks the CMM to start

// R_STATUS bits.
constexpr u64 ST_GO = 0x001;      ///< write: start a window access; read: busy
constexpr u64 ST_LOW = 0x002;     ///< write: store the low byte (even address)
constexpr u64 ST_HIGH = 0x004;    ///< write: store the high byte (odd address)
constexpr u64 ST_ATTN = 0x008;    ///< write: attention, a mailbox changed
constexpr u64 ST_INTR = 0x010;    ///< read: a terminal interrupt is pending
constexpr u64 ST_FLAG = 0x020;    ///< the console's own flag, kept as written
constexpr u64 ST_CPU1 = 0x040;    ///< read: this processor is the module's 2nd
constexpr u64 ST_TXFULL = 0x080;  ///< read: the terminal cannot take a char
constexpr u64 ST_RXREADY = 0x100; ///< read: a character waits in R_RX

// R_STATE: the PALcode keeps its state in <5:2> and the console terminal's
// interrupt enables in <1:0> and <7:6> (cserve 0x46 merges a mask of 0xc3
// into the word, 0x3d3fc): RX ready and TX empty, <0>/<1> for the module's
// first processor and <6>/<7> for its second (txon, 0x31aca0, sets 0x2 or
// 0x80 by cpu_id). R_REASON reports the same bits for the interrupts that
// happened, which the PALcode's handler (0x39bd0, for RBOX_INT<9>) turns
// into SCB vectors 0x6c0, 0x6d0, 0x700 and 0x710.
constexpr u64 rx_bit(u32 n) { return n ? 0x40 : 0x01; }
constexpr u64 tx_bit(u32 n) { return n ? 0x80 : 0x02; }
/// RBOX_INT<9>: the CMM's interrupt (EI0; the PALcode's handler tests it
/// at 0x392b0 and clears it when done).
constexpr u64 RBOX_INT_GIO = U64(1) << 9;

// --- The CMM's memory, per processor (n = place on the module) -------------
// Offsets from area(n) = 0x40000 + n * 0x6000, as the console computes them.
constexpr u32 area(u32 n) { return 0x40000 + n * 0x6000; }
/// PALcode, block + 0x1298: 0 for the partition's primary, which builds its
/// PAL area and HWRPB pointers from scratch (0x3ea0c); anything else for a
/// secondary, whose PALcode then keeps the pointers the console copied into
/// its PAL area before starting it (start_secondary: PAL area 0x128 and
/// 0x190-0x1a7). Without that a secondary maps the console's addresses onto
/// its own memory and halts on the zeros there.
constexpr u32 A_STATE = 0x12a0;
constexpr u32 A_SPEED =
    0x12a6; ///< PALcode: u16 MHz (block + 0x129e), cserve 0x4a
/**
 * The route table the XSROM leaves for the console (start_secondaries,
 * 0x2dd0f0, for the primary; the PALcode's own copy loop at 0x3f108 for a
 * secondary): u16 the number of routes to processors, u16 the number of
 * further routes, then that many longwords. The console stores each one
 * shifted left by four in its route table, entry n for PID n (and 0x80 on
 * for the others), which the PALcode's cserve 0x4f returns to OpenVMS
 * (Topology.cpp has the fields).
 */
constexpr u32 A_ROUTES = 0xe10;
constexpr u32 A_IO_ROUTES = 0xe12;
constexpr u32 A_ROUTE = 0xe14;
constexpr u32 kRoutes = 0x80;
constexpr u32 A_TOY = 0x1aac;    ///< rtc_read: MC146818 registers 0-11
constexpr u32 A_REQ = 0x1ac8;    ///< smlan_write: 3 request slots
constexpr u32 A_RSP = 0x3310;    ///< smlan_read: 3 response slots
constexpr u32 A_TOY_NV = 0x4b98; ///< rtc_read: TOY NVRAM, index 12 and up
constexpr u32 kSlots = 3;
constexpr u32 kSlotSize = 0x818;
constexpr u32 kMsgMax = kSlotSize - 4;
/// The system type, a longword whose low byte the PALcode reads first and
/// smlan_init keeps (0x3ac160): 1 is a GS1280, 0x11 an ES47 or ES80 -- told
/// apart by <19:16>, 0 for an ES47 and 1 for an ES80 -- and anything else
/// the console's development system, "TS212c" (build_dsrdb, 0x2dd8f0). The
/// board row's layout holds it (Topology.hpp).
constexpr u32 A_SYSTYPE = 0x40004;

// Request slot: +0 u8 state (3 = free, 0 = posted by the console), +2 u16
// length, +4 the message. Response slot: +0 u8 state (<0> full, set by the
// CMM; <1> taken, set by the console, <2> console: tell me when read),
// +4 the message.
constexpr u8 REQ_FREE = 3;
constexpr u8 RSP_FULL = 1;
constexpr u8 RSP_TAKEN = 2;

// An SMLAN message: the header the console builds (get_own_partition_number
// and its siblings) and the CMM's own dump routine names (dumppkt.c:
// destination, originator, identifier, command, status).
constexpr u32 M_ORIG = 0x0; ///< 0 from the console
constexpr u32 M_DEST = 0x4; ///< the micro asked: pid2ip, e.g. 10.0.1.0

constexpr u32 M_ID = 0x8;
constexpr u32 M_CMD = 0xc;
constexpr u32 M_STATUS = 0xe;
constexpr u32 M_DATA = 0x10;

/// The console's NVRAM image (fetch_sm_nvram, save_sm_nvram).
constexpr u32 kNvramSize = 0x800;
constexpr u32 kMbmConfigSize = 0xd8;

u16 get16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
u32 get32(const u8 *p) { return get16(p) | (u32)get16(p + 2) << 16; }
void put16(u8 *p, u16 v) {
  p[0] = (u8)v;
  p[1] = (u8)(v >> 8);
}
void put32(u8 *p, u32 v) {
  put16(p, (u16)v);
  put16(p + 2, (u16)(v >> 16));
}

} // namespace

/**
 * A CMM for every module the board row's layout can hold, set up as it is
 * when the console starts: free mailboxes, the TOY, the system type, and
 * each processor's start state -- 0 for the partition's primary (the
 * first processor), non-zero for every other [inference: what the CMM
 * writes there is not known beyond zero and non-zero].
 */
CEs47Cmm::CEs47Cmm(CSystem *sys, const char *nvram)
    : m_sys(sys), m_nvram_file(nvram), m_nvram(kNvramSize, 0) {
  nvram_load();
  if (const char *fn = getenv("ALPHABOX_GIO_LOG"))
    m_log = fopen(fn, "w");
  m_trace = getenv("ALPHABOX_TRACE_CMM") != nullptr;
  CMarvel *mv = marvel();
  if (!mv)
    FAILURE(Configuration, "the CMM needs the Marvel chipset");
  const CMarvelTopology &t = mv->topology();
  for (int i = 0; i < t.count(); i++) {
    const u32 pid = t.node(i).pid;
    Module &m = m_modules[CMarvelTopology::module_key(pid)];
    if (m.mem.empty()) {
      m.mem.assign(kMemSize, 0);
      m.read_seen.assign(kMemSize, false);
      const u32 sys_type = t.cmm_systype();
      for (u32 b = 0; b < 4; b++)
        mem_write(pid, A_SYSTYPE + b, (u8)(sys_type >> (8 * b)));
    }
    m_port[pid] = Port();
    const u32 n = CMarvelTopology::place(pid);
    for (u32 s = 0; s < kSlots; s++) {
      mem_write(pid, area(n) + A_REQ + s * kSlotSize, REQ_FREE);
      mem_write(pid, area(n) + A_RSP + s * kSlotSize, RSP_TAKEN);
    }
    refresh_toy(pid);
    mem_write(pid, area(n) + A_STATE, i == 0 ? 0 : 1);
  }
}

CMarvel *CEs47Cmm::marvel() const {
  return dynamic_cast<CMarvel *>(m_sys->chipset());
}

int CEs47Cmm::present() const { return m_sys->get_cpu_num(); }

u32 CEs47Cmm::primary() const {
  CMarvel *mv = marvel();
  return mv && mv->topology().count() ? mv->topology().node(0).pid : 0;
}

CEs47Cmm::Module &CEs47Cmm::module_of(u32 pid) {
  return m_modules[CMarvelTopology::module_key(pid)];
}

/// What the CMM knows once the processors exist: each one's clock, which on
/// a real module the CMM set itself (ev7_clocks).
void CEs47Cmm::late_init() {
  m_ready = true;
  CMarvel *mv = marvel();
  for (int i = 0; i < m_sys->get_cpu_num(); i++) {
    CAlphaCPU *c = m_sys->get_cpu(i);
    const u32 pid = c->get_pid();
    const u32 n = CMarvelTopology::place(pid);
    const u32 mhz = (u32)(c->get_speed() / 1000000);
    mem_write(pid, area(n) + A_SPEED, (u8)mhz);
    mem_write(pid, area(n) + A_SPEED + 1, (u8)(mhz >> 8));
    // The routes from this processor to every PID, in the copy's form: the
    // fields Topology.cpp names, less their low four bits.
    mem_write(pid, area(n) + A_ROUTES, (u8)kRoutes);
    mem_write(pid, area(n) + A_ROUTES + 1, 0);
    mem_write(pid, area(n) + A_IO_ROUTES, 0);
    mem_write(pid, area(n) + A_IO_ROUTES + 1, 0);
    for (u32 to = 0; to < kRoutes && mv; to++) {
      const u32 r = mv->topology().route(pid, to, present()) >> 4;
      for (u32 b = 0; b < 4; b++)
        mem_write(pid, area(n) + A_ROUTE + to * 4 + b, (u8)(r >> (8 * b)));
    }
  }
}

CEs47Cmm::~CEs47Cmm() {
  if (m_log)
    fclose(m_log);
}

void CEs47Cmm::note(const char *fmt, ...) {
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  const u64 pc = t_running_cpu ? t_running_cpu->get_pc() : 0;
  printf("%%MVL-I-CMM: %s (pc %" PRIx64 ")\n", line, pc);
  if (m_log) {
    fprintf(m_log, "%s pc=%" PRIx64 "\n", line, pc);
    fflush(m_log);
  }
}

// --- Memory ------------------------------------------------------------------

u8 CEs47Cmm::mem_read(u32 pid, u32 a) {
  if (a < kMemBase || a >= kMemBase + kMemSize) {
    note("read of CMM address %05x outside the modelled memory", a);
    return 0;
  }
  Module &m = module_of(pid);
  const u8 v = m.mem[a - kMemBase];
  if (!m.read_seen[a - kMemBase]) {
    m.read_seen[a - kMemBase] = true;
    if (m_log)
      fprintf(m_log, "first read %05x = %02x (module of PID %u)\n", a, v, pid);
  }
  return v;
}

void CEs47Cmm::mem_write(u32 pid, u32 a, u8 v) {
  if (a < kMemBase || a >= kMemBase + kMemSize) {
    note("write of CMM address %05x = %02x outside the modelled memory", a, v);
    return;
  }
  module_of(pid).mem[a - kMemBase] = v;
}

u8 *CEs47Cmm::mem_at(u32 pid, u32 a) {
  return &module_of(pid).mem[a - kMemBase];
}

// --- The registers -----------------------------------------------------------

bool CEs47Cmm::gio_write(u32 pid, u32 reg, u64 value) {
  std::lock_guard<std::mutex> g(m_lock);
  if (!m_ready)
    late_init();
  const u32 n = CMarvelTopology::place(pid);
  Port &p = m_port[pid];
  switch (reg) {
  case R_STATUS:
    p.status = value & ST_FLAG;
    if (value & ST_GO)
      window_op(pid, p, value);
    // ST_ATTN: the console rings after changing a mailbox when the CMM asked
    // for it (reg 0xa); the model acts on the mailbox write itself.
    break;
  case R_DATA:
    p.data = value & 0xffff;
    break;
  case R_ADDRESS:
    p.address = value;
    break;
  case R_TX: {
    const u8 c = (u8)value;
    if (CSerial *t = terminal())
      t->WriteMem(0, 0, 8, c);
    else {
      putchar(c);
      fflush(stdout);
    }
    // The model's transmitter is never busy: ready again at once.
    if (p.regs[R_STATE] & tx_bit(n))
      p.tx_kick = true;
    break;
  }
  case R_COMM:
    // smlan_init_comm sets <0> and waits for the CMM to clear it.
    if (value & 1)
      note("PID %u: the console starts its SMLAN link", pid);
    p.regs[reg] = value & ~U64(1);
    break;
  default:
    // Register 8: the PALcode's state word (PAL scratch + 0x1b8), sent
    // whenever it changes: the processor's state [inference: the CMM shows
    // it] and the terminal's interrupt enables (R_STATE above); the others
    // are not known to be written. A booted OpenVMS changes it many times a
    // second, so it is only noted under ALPHABOX_TRACE_CMM.
    if (p.regs[reg & 15] != value || reg != R_STATE)
      if (reg != R_STATE || m_trace)
        note("PID %u: GIO register %u = %" PRIx64 " (stored)", pid, reg, value);
    if (reg == R_STATE && (value & tx_bit(n)) && !(p.regs[R_STATE] & tx_bit(n)))
      p.tx_kick = true; // transmit interrupts enabled with the line idle
    p.regs[reg & 15] = value;
    break;
  }
  return true;
}

bool CEs47Cmm::gio_read(u32 pid, u32 reg, u64 *value) {
  std::lock_guard<std::mutex> g(m_lock);
  if (!m_ready)
    late_init();
  const u32 n = CMarvelTopology::place(pid);
  Port &p = m_port[pid];
  switch (reg) {
  case R_STATUS: {
    u64 v = p.status | (n ? ST_CPU1 : 0) | (p.reason ? ST_INTR : 0);
    if (CSerial *t = terminal())
      if (t->ReadMem(0, 5, 8) & 1)
        v |= ST_RXREADY;
    *value = v;
    break;
  }
  case R_DATA:
    *value = p.data;
    break;
  case R_ADDRESS:
    *value = p.address;
    break;
  case R_REASON: {
    // Read to clear [inference: platform_init2 reads it once and discards
    // it before it unmasks the CMM's interrupt] -- one reason at a time:
    // the PALcode's handler (0x39d14) dispatches only the first bit it
    // finds, in the order below, so a second reason read with it would be
    // lost. Losing a transmit-ready that way left OpenVMS waiting for it
    // with its output stopped, whenever a character arrived while it
    // printed. What stays pending is signalled again (tick).
    static const u64 kOrder[] = {0x01, 0x02, 0x40, 0x80,
                                 0x04, 0x08, 0x10, 0x20};
    *value = 0;
    for (u64 bit : kOrder)
      if (p.reason & bit) {
        *value = bit;
        p.reason &= ~bit;
        break;
      }
    break;
  }
  case R_RX:
    *value = 0;
    if (CSerial *t = terminal())
      if (t->ReadMem(0, 5, 8) & 1)
        *value = t->ReadMem(0, 0, 8) & 0xff;
    break;
  default:
    *value = p.regs[reg & 15];
    if (reg != R_COMM && reg != 0xa)
      note("PID %u: GIO register %u read as %" PRIx64, pid, reg, *value);
    break;
  }
  return true;
}

/**
 * The console terminal's interrupts, for an operating system that asks for
 * them (the console's cb_set_term_int; OpenVMS does): a transmitter that is
 * ready while enabled, and a received character while enabled, are posted
 * in R_REASON and signalled through RBOX_INT<9> of the processor they
 * belong to. Checked once per interval-timer period: the CMM's own register
 * accesses arrive with that processor's register block locked, so the
 * interrupt is raised here, outside both locks.
 */
void CEs47Cmm::tick() {
  std::vector<u32> raise;
  {
    std::lock_guard<std::mutex> g(m_lock);
    if (!m_ready)
      return;
    const u32 console = primary();
    for (auto &kv : m_port) {
      const u32 pid = kv.first, n = CMarvelTopology::place(pid);
      Port &p = kv.second;
      const u64 en = p.regs[R_STATE];
      u64 add = 0;
      if (p.tx_kick && (en & tx_bit(n)))
        add |= tx_bit(n);
      p.tx_kick = false;
      // The terminal is the partition's console, on the primary [the
      // other processors' virtual UARTs carry nothing here].
      if (pid == console && (en & rx_bit(n)) && !(p.reason & rx_bit(n)))
        if (CSerial *t = terminal())
          if (t->ReadMem(0, 5, 8) & 1)
            add |= rx_bit(n);
      // Anything not taken yet is signalled again: the PALcode takes one
      // reason per interrupt (R_REASON).
      p.reason |= add;
      if (p.reason)
        raise.push_back(pid);
    }
  }
  CMarvel *mv = marvel();
  for (u32 pid : raise)
    if (CEv7Csr *c = mv ? mv->csr(pid) : nullptr)
      c->request(RBOX_INT_GIO);
}

/// A byte-window access (read_dma/write_dma): R_ADDRESS names a CMM byte,
/// R_DATA carries the 16-bit word holding it, the byte at an odd address in
/// <15:8>; a write stores the lanes ST_LOW/ST_HIGH select.
void CEs47Cmm::window_op(u32 pid, Port &p, u64 control) {
  const u32 a = (u32)p.address & ~1u;
  if (!(control & (ST_LOW | ST_HIGH))) {
    for (u32 m = 0; m < 2; m++) {
      const u32 toy = area(m) + A_TOY;
      if (a + 1 >= toy && a < toy + 12)
        refresh_toy((pid & ~1u) | m);
    }
    p.data = mem_read(pid, a) | (u16)mem_read(pid, a + 1) << 8;
    return;
  }
  if (control & ST_LOW)
    mem_write(pid, a, (u8)p.data);
  if (control & ST_HIGH)
    mem_write(pid, a + 1, (u8)(p.data >> 8));
  // Either processor of the module may write into the other's area; what
  // is written is the area's processor's (PID<0> is the place).
  for (u32 m = 0; m < 2; m++) {
    const u32 owner = (pid & ~1u) | m;
    const u32 toy = area(m) + A_TOY;
    if (a + 1 >= toy && a < toy + 12)
      toy_written(owner, a < toy ? 0 : a - toy);
    for (u32 s = 0; s < kSlots; s++)
      if (a == area(m) + A_REQ + s * kSlotSize && (control & ST_LOW) &&
          a >= kMemBase && a < kMemBase + kMemSize && *mem_at(pid, a) == 0)
        request_posted(owner, s);
  }
}

// --- The TOY -----------------------------------------------------------------

/// The MC146818 registers 0-9 in the CMM's memory, from the host clock and
/// what the console set: binary, 24-hour (register B = 0x06).
void CEs47Cmm::refresh_toy(u32 pid) {
  u8 *t = mem_at(pid, area(CMarvelTopology::place(pid)) + A_TOY);
  if (t[11] & 0x80)
    return; // SET: the console is writing the time
  const auto host = std::chrono::system_clock::now().time_since_epoch();
  const s64 host_us =
      std::chrono::duration_cast<std::chrono::microseconds>(host).count();
  const time_t now = (time_t)(host_us / 1000000) + (time_t)m_toy_offset;
  struct tm tm;
  gmtime_r(&now, &tm);
  t[0] = (u8)tm.tm_sec;
  t[2] = (u8)tm.tm_min;
  t[4] = (u8)tm.tm_hour;
  t[6] = (u8)(tm.tm_wday + 1);
  t[7] = (u8)tm.tm_mday;
  t[8] = (u8)(tm.tm_mon + 1);
  t[9] = (u8)(tm.tm_year % 100);
  // Byte 10, an MC146818's register A, is the CMM's update flag: zero
  // unless the time is being updated. OpenVMS 8.4 (its TOY read at
  // ffffffff8001daf0 in the CD's boot) calls GET_TOY for byte 10 and waits,
  // with a delay between reads, until the whole byte is zero; the console's
  // PALcode tests only <7> (GET_TOY of byte 0 answers -2 while it is set).
  // So <7>, UIP, for the last 2 ms of each second as an MC146818 holds it,
  // and nothing else [inference: the divider bits an MC146818 has there
  // (0x26) made OpenVMS wait for ever].
  t[10] = (host_us % 1000000) >= 998000 ? 0x80 : 0;
  t[11] = (t[11] & 0x70) | 0x06;
  t[12] = 0;
  t[13] = 0x80; // register D: the battery is good
}

/// The console wrote TOY register `idx`; when it releases SET, what it wrote
/// becomes the time, kept as an offset from the host's.
void CEs47Cmm::toy_written(u32 pid, u32 idx) {
  if (idx != 11)
    return;
  u8 *t = mem_at(pid, area(CMarvelTopology::place(pid)) + A_TOY);
  if (t[11] & 0x80)
    return;
  struct tm tm = {};
  tm.tm_sec = t[0];
  tm.tm_min = t[2];
  tm.tm_hour = t[4];
  tm.tm_mday = t[7];
  tm.tm_mon = t[8] - 1;
  tm.tm_year = t[9] < 70 ? t[9] + 100 : t[9];
  const time_t set = timegm(&tm);
  if (set != (time_t)-1)
    m_toy_offset = (s64)(set - time(nullptr));
  note("the console set the TOY (offset %" PRId64 " s from the host)",
       m_toy_offset);
}

// --- The console terminal ---------------------------------------------------

/// The configuration's first serial port carries the console terminal
/// (`serial0 = serial { port = ...; }` inside the system block), as the
/// CMM's own serial port does on a real module.
CSerial *CEs47Cmm::terminal() {
  if (!m_terminal_looked) {
    m_terminal_looked = true;
    for (int i = 0; i < m_sys->component_count() && !m_terminal; i++)
      m_terminal = dynamic_cast<CSerial *>(m_sys->component(i));
    if (!m_terminal)
      printf("%%MVL-W-CMM: no serial port configured: the console terminal "
             "prints to standard output and takes no input.\n");
  }
  return m_terminal;
}

// --- SMLAN mailboxes ---------------------------------------------------------

/// smlan_write filled request slot `s` of processor `n` and cleared its state
/// byte: answer it now and free the slot (state 3, which wait_for_cmm
/// waits for).
void CEs47Cmm::request_posted(u32 pid, u32 s) {
  u8 *slot =
      mem_at(pid, area(CMarvelTopology::place(pid)) + A_REQ + s * kSlotSize);
  u32 len = get16(slot + 2);
  if (len > kMsgMax)
    len = kMsgMax;
  std::vector<u8> req(slot + 4, slot + 4 + kMsgMax);
  answer(pid, req.data(), len);
  *slot = REQ_FREE;
}

/// Put an answer to `req` into a free response slot of processor `n`.
void CEs47Cmm::respond(u32 pid, const u8 *req, u16 status, const u8 *data,
                       u32 len) {
  if (len > kMsgMax - M_DATA)
    len = kMsgMax - M_DATA;
  for (u32 s = 0; s < kSlots; s++) {
    u8 *slot =
        mem_at(pid, area(CMarvelTopology::place(pid)) + A_RSP + s * kSlotSize);
    if (*slot & RSP_FULL)
      continue;
    u8 *msg = slot + 4;
    memset(msg, 0, kMsgMax);
    put32(msg + M_DEST, get32(req + M_ORIG));
    put32(msg + M_ORIG, get32(req + M_DEST));
    put32(msg + M_ID, get32(req + M_ID));
    put16(msg + M_CMD, get16(req + M_CMD));
    put16(msg + M_STATUS, status);
    if (len)
      memcpy(msg + M_DATA, data, len);
    put16(slot + 2, (u16)(M_DATA + len));
    slot[1] = 0;
    slot[0] = RSP_FULL;
    return;
  }
  note("PID %u: no free response slot for command %04x", pid,
       get16(req + M_CMD));
}

// --- What the MBM would answer ----------------------------------------------

void CEs47Cmm::nvram_load() {
  FILE *f = fopen(m_nvram_file.c_str(), "rb");
  if (!f)
    return; // a new machine: the console finds its NVRAM blank
  const size_t got = fread(m_nvram.data(), 1, kNvramSize, f);
  fclose(f);
  printf("%%MVL-I-CMM: console NVRAM read from %s (%zu bytes).\n",
         m_nvram_file.c_str(), got);
}

void CEs47Cmm::nvram_save() {
  FILE *f = fopen(m_nvram_file.c_str(), "wb");
  if (!f) {
    printf("%%MVL-W-CMM: cannot write the console NVRAM to %s.\n",
           m_nvram_file.c_str());
    return;
  }
  fwrite(m_nvram.data(), 1, kNvramSize, f);
  fclose(f);
}

/**
 * The partition database (SMLAN 0x0323, 0x800 bytes), laid out as memconfig
 * walks it and as the console's own built-in database for its simulator
 * (get_partition_database at 0x3095e0) fills it: four lists, each a count
 * byte (in a longword) followed by its entries.
 *
 *   hard partitions, 0x1c each: <0> number, <4> u32 0xff, <8> name
 *   sub partitions, 0x20 each:  <0> hard partition, <2> 4, <6> name
 *   processors, 12 each:        <0> 0x80 the sub partition's primary,
 *                               0 another member, <1> N/S, <2> E/W,
 *                               <3> PID, <4> hard partition,
 *                               <6> sub partition
 *   I/O, 8 each:                <0> I/O drawer, <1> cabinet, <2> riser
 *                               (show_core_system, 0x2dcdac, prints them
 *                               in the "I/O Drawer" line, as found by
 *                               find_io7_data), <3> N/S, <4> E/W of the
 *                               IO7's EV7 (coord2pid, 0x2e1240, matches
 *                               them with the processors' <1> and <2>),
 *                               <5> present
 *
 * One hard and one sub partition holding every processor and every IO7,
 * at the coordinates the topology gives them (Topology.hpp); coord2id
 * turns those into the Hard ID and CPU number `show config` prints (a real
 * ES47: NS,EW (0,0) and (1,0), Hard ID 0 and 1). [guess] the meaning of
 * 0xff and 4, copied from the console's built-in database; the console
 * also takes the hard partition's 0xff as the number of PIDs to size its
 * route table for (start_secondaries).
 */
void CEs47Cmm::partition_database(u8 *db) {
  memset(db, 0, kNvramSize);
  CMarvel *mv = marvel();
  const CMarvelTopology &t = mv->topology();
  u8 *p = db;
  *p = 1; // hard partitions
  p += 4;
  p[0] = 0;
  put32(p + 4, 0xff);
  strcpy((char *)p + 8, "hard_partition0");
  p += 0x1c;
  *p = 1; // sub partitions
  p += 4;
  p[0] = 0;
  p[2] = 4;
  strcpy((char *)p + 6, "sub_partition0");
  p += 0x20;
  const int cpus = present();
  *p = (u8)cpus; // processors
  p += 4;
  for (int i = 0; i < cpus; i++, p += 12) {
    const ev7_node &n = t.node(i);
    // The primary: memconfig makes the last 0x80 processor of the sub
    // partition the one that builds the GCT (0x282948), which only the
    // primary's powerup does.
    p[0] = i == 0 ? 0x80 : 0;
    p[1] = n.ns;
    p[2] = n.ew;
    p[3] = (u8)n.pid;
    p[4] = 0;
    p[6] = 0;
  }
  u8 *io_count = p; // I/O: an IO7 on each processor the board cables one to
  p += 4;
  for (int i = 0; i < cpus; i++) {
    const ev7_node &n = t.node(i);
    if (!mv->io7(n.pid))
      continue;
    // The I/O drawer the board row names, the cabinet the PID names (<7:5>,
    // the console's pid2rack), riser 0: a real ES47's "I/O Drawer 0
    // Cabinet 0 Riser 0", a real GS1280's "I/O Drawer 1 Cabinet 0 Riser 0".
    const marvel_layout *layout = m_sys->platform().marvel;
    p[0] = layout && layout->io_drawer ? layout->io_drawer(n.pid)
                                       : (u8)((n.pid >> 3) & 3);
    p[1] = (u8)(n.pid >> 5);
    p[2] = 0;
    p[3] = n.ns;
    p[4] = n.ew;
    p[5] = 1;
    p += 8;
    (*io_count)++;
  }
}

/**
 * The MBM's configuration (SMLAN 0x0321, 0xd8 bytes) -- of the drawer the
 * request goes to, since every drawer has its own MBM (pid2ip: the MBM of
 * the drawer holding a PID is 10.<cabinet * 16 + drawer>.0.1; memconfig
 * asks once per drawer, for the drawer's PIDs, cabinet <7:5> and drawer
 * <4:3>). Four CPU modules, 0x34 bytes apart, as the console's built-in
 * answer for its simulator fills it (get_mbm_configuration at 0x30a210):
 * <4> u16 0xffff for a module that is not there and 0 for one that is;
 * then the module's two processors at <8> and <0x20>, 0x18 bytes each (the
 * second one's last words overlap the next module's first longword, which
 * nothing reads): <0> u16 1 for a processor that is there, and from <4> ten
 * RIMM words, Zbox 0's RIMMs 0-4 then Zbox 1's 5-9, as memconfig copies
 * them into the table `show memory` prints a P (non-zero) or a dot from.
 * Module m holds PIDs drawer base + 2m and + 2m + 1 (PID<2:1>). The console
 * takes the memory sizes from the partition's memory assignment (0x0418);
 * the RIMM words here are each RIMM's size in MB [guess at the unit]: four
 * RIMMs and the fifth (RAID) on Zbox 0, as the real ES47's `show config`
 * lists PPPPP.....
 */
void CEs47Cmm::mbm_configuration(u32 mbm_ip, u8 *c) {
  memset(c, 0, kMbmConfigSize);
  CMarvel *mv = marvel();
  const u32 x = (mbm_ip >> 8) & 0xff; // cabinet * 16 + drawer
  const u32 base = ((x >> 4) & 7) << 5 | (x & 3) << 3;
  const u16 rimm_mb = (u16)(mv->memory_per_pid() / 4 / (1024 * 1024));
  for (u32 m = 0; m < 4; m++) {
    u8 *mod = c + m * 0x34;
    bool here = false;
    for (u32 k = 0; k < 2; k++) {
      if (!mv->topology().by_pid(base + 2 * m + k, present()))
        continue;
      here = true;
      u8 *cpu = mod + 8 + k * 0x18;
      put16(cpu, 1);
      for (int r = 0; r < 5; r++)
        put16(cpu + 4 + 2 * r, rimm_mb);
    }
    put16(mod + 4, here ? 0 : 0xffff);
  }
}

/**
 * The hard partition's memory (SMLAN 0x0418, 0x800 bytes), as
 * build_memory_chunks reads it and the console's simulator answer
 * (0x309f38) fills it: <0> u64 [guess] the partition's total, <8> u32 the
 * number of groups; then each group: <0> u8 sub partition, <1> u8 its
 * chunks, two bytes, and the chunks, 16 bytes each: u64 base, u64 size.
 * Each processor's own memory is one chunk, at its PID's base.
 */
void CEs47Cmm::memory_assignment(u8 *a) {
  memset(a, 0, kNvramSize);
  CMarvel *mv = marvel();
  const int cpus = present();
  const u64 each = mv->memory_per_pid();
  const u64 total = each * (u64)cpus;
  put32(a, (u32)total);
  put32(a + 4, (u32)(total >> 32));
  put32(a + 8, 1);
  u8 *g = a + 0xc;
  g[0] = 0;
  g[1] = (u8)cpus;
  u8 *chunk = g + 4;
  for (int i = 0; i < cpus; i++, chunk += 16) {
    const u64 base = ev7::memory_base(mv->topology().node(i).pid);
    put32(chunk, (u32)base);
    put32(chunk + 4, (u32)(base >> 32));
    put32(chunk + 8, (u32)each);
    put32(chunk + 12, (u32)(each >> 32));
  }
}

/// The micro an IP address names (pid2ip): a drawer's MBM is
/// 10.<cabinet * 16 + drawer>.0.1, a module's CMM 10.<..>.<module + 1>.0
/// (stored least significant byte first).
bool CEs47Cmm::is_mbm(u32 ip) { return (ip >> 16) == 0x0100; }

/// An I/O drawer's backplane manager (phys2ip type 4, 0x2e1610):
/// 10.254.<E/W>.<N/S> of the EV7 its IO7 hangs on.
bool CEs47Cmm::is_pbm(u32 ip) { return (ip & 0xffff) == 0xfe0a; }

const char *CEs47Cmm::micro_name(u32 ip) {
  return is_mbm(ip) ? "the MBM" : is_pbm(ip) ? "the PBM" : "the CMM";
}

/**
 * Environmental readings (SMLAN 0x0901 voltages, 0x0902 temperatures):
 * <0> u32 the count, then records of 0x1c bytes: <0> u16 the sensor's
 * index (0, 1, ...), <2> s16 the reading (<15> set: none), <4> the
 * sensor's subpacket data the console copies into the GCT, 0x18 bytes
 * for a voltage and 0xc for a temperature (build_cmm_hw, build_mbm_hw;
 * locate_voltage_data, locate_temp_data). The console expects a CMM to
 * have 8 sensors and an MBM 17 on an ES47 or ES80 (system type 0x11), 5 on
 * a GS1280, voltages and temperatures together (build_mbm_hw, 0x2f8db0),
 * and an I/O drawer's PBM 17 in a standard I/O drawer (build_pbm_hw,
 * 0x2f9070: 17 for I/O types 0 and 3, 23 for 2; the embedded I/O has none);
 * what each one is, and the reading's unit, are [guess]: six voltages and
 * the two EV7s' temperatures on the CMM, thirteen and four on a 2P
 * drawer's MBM and on a PBM, three and two on an 8P drawer's, readings in mV
 * and degrees C, the subpacket data zero. Returns the count.
 */
int CEs47Cmm::sensor_readings(u32 ip, bool volts, u8 *r) {
  static const s16 cmm_mv[] = {1500, 1500, 1800, 2500, 1500, 1200};
  static const s16 cmm_c[] = {35, 35};
  static const s16 mbm_mv[] = {3300, 3300, 5000, 5000, 12000, 12000, 1500,
                               1500, 2500, 2500, 1800, 1800,  3300};
  static const s16 mbm_c[] = {25, 27, 30, 30};
  const bool mbm = is_mbm(ip) || is_pbm(ip);
  const bool gs = (marvel()->topology().cmm_systype() & 0xff) == 1;
  const s16 *v = volts ? (mbm ? mbm_mv : cmm_mv) : (mbm ? mbm_c : cmm_c);
  int n = volts ? (mbm ? 13 : 6) : (mbm ? 4 : 2);
  if (is_mbm(ip) && gs)
    n = volts ? 3 : 2;
  memset(r, 0, 0x1e0);
  put32(r, (u32)n);
  for (int i = 0; i < n; i++) {
    u8 *rec = r + 4 + i * 0x1c;
    put16(rec, (u16)i);
    put16(rec + 2, (u16)v[i]);
  }
  return n;
}

/// The SMLAN commands the console sends (docs/platforms/marvel.md, M4).
void CEs47Cmm::answer(u32 n, const u8 *req, u32 len) {
  // `n` is the asking processor's PID.
  const u16 cmd = get16(req + M_CMD);
  const u32 id = get32(req + M_ID);
  switch (cmd) {
  case 0x0323: {
    // get_partition_database: the request names the database (byte 1).
    std::vector<u8> db(kNvramSize);
    partition_database(db.data());
    note("PID %u: SMLAN %04x (partition database) id %u", n, cmd, id);
    respond(n, req, 0, db.data(), kNvramSize);
    return;
  }
  case 0x0321: {
    // get_mbm_configuration
    u8 c[kMbmConfigSize];
    mbm_configuration(get32(req + M_DEST), c);
    note("PID %u: SMLAN %04x (MBM configuration of drawer %u) id %u", n, cmd,
         (get32(req + M_DEST) >> 8) & 0xff, id);
    respond(n, req, 0, c, sizeof(c));
    return;
  }
  case 0x0418: {
    // get_hard_partition_mem_assignment: the hard partition in byte 0.
    std::vector<u8> a(kNvramSize);
    memory_assignment(a.data());
    note("PID %u: SMLAN %04x (memory of hard partition %u) id %u", n, cmd,
         req[M_DATA], id);
    respond(n, req, 0, a.data(), kNvramSize);
    return;
  }
  case 0x0a01:
    // get_eerom_data: a FRU's EEPROM by the IP address of the micro that
    // holds it (request <4>). Status 3, which the console takes quietly as
    // "not there": [stub] no FRU contents are modelled.
    note("PID %u: SMLAN %04x (EEPROM of micro %08x) id %u: none [stub]", n, cmd,
         get32(req + M_DEST), id);
    respond(n, req, 3, nullptr, 0);
    return;
  case 0x0901:
  case 0x0902: {
    // get_voltage_readings / get_temperature_readings, of the micro the
    // request names: a count, then 0x1c-byte records.
    std::vector<u8> r(0x1e0);
    const int count =
        sensor_readings(get32(req + M_DEST), cmd == 0x0901, r.data());
    note("PID %u: SMLAN %04x (%s of %s) id %u: %d sensors", n, cmd,
         cmd == 0x0901 ? "voltages" : "temperatures",
         micro_name(get32(req + M_DEST)), id, count);
    respond(n, req, 0, r.data(), (u32)r.size());
    return;
  }
  case 0x0903: // get_fan_rpm_readings
  case 0x0908: // get_power_supply_state
  case 0x090e: // get_ps_tray
  case 0x090f: // get_vrm_status
    // [stub] none reported: the answer the console builds itself on its
    // simulator (a zeroed buffer and success).
    note("PID %u: SMLAN %04x (environment) id %u: nothing to report [stub]", n,
         cmd, id);
    respond(n, req, 0, nullptr, 0);
    return;
  case 0x041c:
    // fetch_sm_nvram: hard and soft partition in the request's data bytes
    // 0 and 1; the answer is the 2 KB image.
    note("PID %u: SMLAN %04x (fetch NVRAM, partition %u.%u) id %u", n, cmd,
         req[M_DATA], req[M_DATA + 1], id);
    respond(n, req, 0, m_nvram.data(), kNvramSize);
    return;
  case 0x041b:
    // save_sm_nvram: partition bytes, then the 2 KB image.
    memcpy(m_nvram.data(), req + M_DATA + 2, kNvramSize);
    nvram_save();
    note("PID %u: SMLAN %04x (save NVRAM) id %u", n, cmd, id);
    respond(n, req, 0, nullptr, 0);
    return;
  case 0x0333: {
    // get_own_partition_number: the hard partition in byte 0, the soft
    // partition in byte 1.
    const u8 d[2] = {0, 0};
    note("PID %u: SMLAN %04x (own partition number) id %u: hard 0, soft 0", n,
         cmd, id);
    respond(n, req, 0, d, sizeof(d));
    return;
  }
  default:
    note("PID %u: SMLAN %04x id %u, %u bytes: not modelled, answered with "
         "status 1",
         n, cmd, id, len);
    respond(n, req, 1, nullptr, 0);
    return;
  }
}
