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

#include <cstdarg>
#include <ctime>

namespace {

// --- The far-side registers (GIO register numbers) ---------------------------
constexpr u32 R_STATUS = 0x0;  ///< status and control
constexpr u32 R_DATA = 0x1;    ///< byte window: 16-bit data
constexpr u32 R_ADDRESS = 0x2; ///< byte window: CMM address
constexpr u32 R_TX = 0x4;      ///< console terminal: a character out
constexpr u32 R_RX = 0x5;      ///< console terminal: a character in
constexpr u32 R_COMM = 0xb;    ///< <0>: the console asks the CMM to start

// R_STATUS bits.
constexpr u64 ST_GO = 0x001;      ///< write: start a window access; read: busy
constexpr u64 ST_LOW = 0x002;     ///< write: store the low byte (even address)
constexpr u64 ST_HIGH = 0x004;    ///< write: store the high byte (odd address)
constexpr u64 ST_ATTN = 0x008;    ///< write: attention, a mailbox changed
constexpr u64 ST_FLAG = 0x020;    ///< the console's own flag, kept as written
constexpr u64 ST_CPU1 = 0x040;    ///< read: this processor is the module's 2nd
constexpr u64 ST_TXFULL = 0x080;  ///< read: the terminal cannot take a char
constexpr u64 ST_RXREADY = 0x100; ///< read: a character waits in R_RX

// --- The CMM's memory, per processor (n = place on the module) -------------
// Offsets from area(n) = 0x40000 + n * 0x6000, as the console computes them.
constexpr u32 area(u32 n) { return 0x40000 + n * 0x6000; }
constexpr u32 A_STATE = 0x12a0;  ///< PALcode: 0 = cold start (block + 0x1298)
constexpr u32 A_TOY = 0x1aac;    ///< rtc_read: MC146818 registers 0-11
constexpr u32 A_REQ = 0x1ac8;    ///< smlan_write: 3 request slots
constexpr u32 A_RSP = 0x3310;    ///< smlan_read: 3 response slots
constexpr u32 A_TOY_NV = 0x4b98; ///< rtc_read: TOY NVRAM, index 12 and up
constexpr u32 kSlots = 3;
constexpr u32 kSlotSize = 0x818;
constexpr u32 kMsgMax = kSlotSize - 4;
/// The system byte the PALcode reads first and smlan_init keeps
/// (0x3ac160): [guess] the system type; read as 0 on the first runs.
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
constexpr u32 M_DEST = 0x0;
constexpr u32 M_ORIG = 0x4;
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

CEs47Cmm::CEs47Cmm(CSystem *sys, const char *nvram)
    : m_sys(sys), m_mem(kMemSize, 0), m_read_seen(kMemSize, false),
      m_nvram_file(nvram), m_nvram(kNvramSize, 0) {
  nvram_load();
  if (const char *fn = getenv("ALPHABOX_GIO_LOG"))
    m_log = fopen(fn, "w");
  m_trace = getenv("ALPHABOX_TRACE_CMM") != nullptr;
  for (u32 n = 0; n < 2; n++) {
    for (u32 s = 0; s < kSlots; s++) {
      mem_write(area(n) + A_REQ + s * kSlotSize, REQ_FREE);
      mem_write(area(n) + A_RSP + s * kSlotSize, RSP_TAKEN);
    }
    refresh_toy(n);
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

u8 CEs47Cmm::mem_read(u32 a) {
  if (a < kMemBase || a >= kMemBase + kMemSize) {
    note("read of CMM address %05x outside the modelled memory", a);
    return 0;
  }
  const u8 v = m_mem[a - kMemBase];
  if (!m_read_seen[a - kMemBase]) {
    m_read_seen[a - kMemBase] = true;
    if (m_log)
      fprintf(m_log, "first read %05x = %02x\n", a, v);
  }
  return v;
}

void CEs47Cmm::mem_write(u32 a, u8 v) {
  if (a < kMemBase || a >= kMemBase + kMemSize) {
    note("write of CMM address %05x = %02x outside the modelled memory", a, v);
    return;
  }
  m_mem[a - kMemBase] = v;
}

// --- The registers -----------------------------------------------------------

bool CEs47Cmm::gio_write(u32 pid, u32 reg, u64 value) {
  std::lock_guard<std::mutex> g(m_lock);
  const u32 n = pid & 1;
  Port &p = m_port[n];
  switch (reg) {
  case R_STATUS:
    p.status = value & ST_FLAG;
    if (value & ST_GO)
      window_op(n, p, value);
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
    // whenever it changes [inference: the CMM shows it as the processor's
    // state]; the others are not known to be written.
    if (p.regs[reg & 15] != value || reg != 8)
      note("PID %u: GIO register %u = %" PRIx64 " (stored)", pid, reg, value);
    p.regs[reg & 15] = value;
    break;
  }
  return true;
}

bool CEs47Cmm::gio_read(u32 pid, u32 reg, u64 *value) {
  std::lock_guard<std::mutex> g(m_lock);
  const u32 n = pid & 1;
  Port &p = m_port[n];
  switch (reg) {
  case R_STATUS: {
    u64 v = p.status | (n ? ST_CPU1 : 0);
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

/// A byte-window access (read_dma/write_dma): R_ADDRESS names a CMM byte,
/// R_DATA carries the 16-bit word holding it, the byte at an odd address in
/// <15:8>; a write stores the lanes ST_LOW/ST_HIGH select.
void CEs47Cmm::window_op(u32 n, Port &p, u64 control) {
  const u32 a = (u32)p.address & ~1u;
  if (!(control & (ST_LOW | ST_HIGH))) {
    const u32 toy = area(n) + A_TOY;
    if (a + 1 >= toy && a < toy + 12)
      refresh_toy(n);
    p.data = mem_read(a) | (u16)mem_read(a + 1) << 8;
    return;
  }
  if (control & ST_LOW)
    mem_write(a, (u8)p.data);
  if (control & ST_HIGH)
    mem_write(a + 1, (u8)(p.data >> 8));
  for (u32 m = 0; m < 2; m++) {
    const u32 toy = area(m) + A_TOY;
    if (a + 1 >= toy && a < toy + 12)
      toy_written(m, a < toy ? 0 : a - toy);
    for (u32 s = 0; s < kSlots; s++)
      if (a == area(m) + A_REQ + s * kSlotSize && (control & ST_LOW) &&
          m_mem[a - kMemBase] == 0)
        request_posted(m, s);
  }
}

// --- The TOY -----------------------------------------------------------------

/// The MC146818 registers 0-9 in the CMM's memory, from the host clock and
/// what the console set: binary, 24-hour (register B = 0x06).
void CEs47Cmm::refresh_toy(u32 n) {
  u8 *t = &m_mem[area(n) + A_TOY - kMemBase];
  if (t[11] & 0x80)
    return; // SET: the console is writing the time
  const time_t now = time(nullptr) + (time_t)m_toy_offset;
  struct tm tm;
  gmtime_r(&now, &tm);
  t[0] = (u8)tm.tm_sec;
  t[2] = (u8)tm.tm_min;
  t[4] = (u8)tm.tm_hour;
  t[6] = (u8)(tm.tm_wday + 1);
  t[7] = (u8)tm.tm_mday;
  t[8] = (u8)(tm.tm_mon + 1);
  t[9] = (u8)(tm.tm_year % 100);
  t[10] = 0x26;
  t[11] = (t[11] & 0x70) | 0x06;
  t[12] = 0;
  t[13] = 0x80; // register D: the battery is good
}

/// The console wrote TOY register `idx`; when it releases SET, what it wrote
/// becomes the time, kept as an offset from the host's.
void CEs47Cmm::toy_written(u32 n, u32 idx) {
  if (idx != 11)
    return;
  u8 *t = &m_mem[area(n) + A_TOY - kMemBase];
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
void CEs47Cmm::request_posted(u32 n, u32 s) {
  const u32 slot = area(n) + A_REQ + s * kSlotSize - kMemBase;
  u32 len = get16(&m_mem[slot + 2]);
  if (len > kMsgMax)
    len = kMsgMax;
  std::vector<u8> req(&m_mem[slot + 4], &m_mem[slot + 4] + kMsgMax);
  answer(n, req.data(), len);
  m_mem[slot] = REQ_FREE;
}

/// Put an answer to `req` into a free response slot of processor `n`.
void CEs47Cmm::respond(u32 n, const u8 *req, u16 status, const u8 *data,
                       u32 len) {
  if (len > kMsgMax - M_DATA)
    len = kMsgMax - M_DATA;
  for (u32 s = 0; s < kSlots; s++) {
    const u32 slot = area(n) + A_RSP + s * kSlotSize - kMemBase;
    if (m_mem[slot] & RSP_FULL)
      continue;
    u8 *msg = &m_mem[slot + 4];
    memset(msg, 0, kMsgMax);
    put32(msg + M_DEST, get32(req + M_ORIG));
    put32(msg + M_ORIG, get32(req + M_DEST));
    put32(msg + M_ID, get32(req + M_ID));
    put16(msg + M_CMD, get16(req + M_CMD));
    put16(msg + M_STATUS, status);
    if (len)
      memcpy(msg + M_DATA, data, len);
    put16(&m_mem[slot + 2], (u16)(M_DATA + len));
    m_mem[slot + 1] = 0;
    m_mem[slot] = RSP_FULL;
    return;
  }
  note("PID %u: no free response slot for command %04x", n, get16(req + M_CMD));
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
 *   processors, 12 each:        <0> 0x80 (assigned), <3> PID,
 *                               <4> hard partition, <6> sub partition
 *   I/O, 8 each:                <3> E/W, <4> N/S of the IO7's EV7,
 *                               <5> present
 *
 * One hard and one sub partition holding every processor; the IO7 on PID 0
 * (at 0,0). [guess] the meaning of 0xff and 4, copied from the console's
 * built-in database.
 */
void CEs47Cmm::partition_database(u8 *db) {
  memset(db, 0, kNvramSize);
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
  const int cpus = m_sys->get_cpu_num();
  *p = (u8)cpus; // processors
  p += 4;
  for (int i = 0; i < cpus; i++, p += 12) {
    p[0] = 0x80;
    p[3] = (u8)i;
    p[4] = 0;
    p[6] = 0;
  }
  *p = 1; // I/O: the IO7 on PID 0
  p += 4;
  p[3] = 0;
  p[4] = 0;
  p[5] = 1;
}

/**
 * The MBM's configuration (SMLAN 0x0321, 0xd8 bytes): four CPU modules of
 * 0x34 bytes, as the console's built-in answer for its simulator fills it
 * (get_mbm_configuration at 0x30a210): <4> u16 0xffff for a module that
 * is not there and 0 for one that is; then the module's two processors at
 * <8> and <0x20>, 0x18 bytes each, <0> u16 1 for one that is there and
 * the rest -- per Zbox, RIMM words that memconfig copies and
 * mem_config_get_rimm_size adds up -- zero, as the simulator leaves them.
 * [guess] what the RIMM words mean: the console takes the memory sizes
 * from the partition's memory assignment (0x0418) instead.
 */
void CEs47Cmm::mbm_configuration(u8 *c) {
  memset(c, 0, kMbmConfigSize);
  const int cpus = m_sys->get_cpu_num();
  for (int m = 0; m < 4; m++) {
    u8 *mod = c + m * 0x34;
    const bool here = m * 2 < cpus;
    put16(mod + 4, here ? 0 : 0xffff);
    for (int k = 0; k < 2; k++)
      if (m * 2 + k < cpus)
        put16(mod + 8 + k * 0x18, 1);
  }
}

/**
 * The hard partition's memory (SMLAN 0x0418, 0x800 bytes), as
 * build_memory_chunks reads it and the console's simulator answer
 * (0x309f38) fills it: <0> u64 [guess] the partition's total, <8> u32 the
 * number of groups; then each group: <0> u8 sub partition, <1> u8 its
 * chunks, two bytes, and the chunks, 16 bytes each: u64 base, u64 size.
 * Each processor's own memory is one chunk.
 */
void CEs47Cmm::memory_assignment(u8 *a) {
  memset(a, 0, kNvramSize);
  const int cpus = m_sys->get_cpu_num();
  CMarvel *marvel = dynamic_cast<CMarvel *>(m_sys->chipset());
  const u64 each = marvel ? marvel->memory_per_pid() : 0;
  const u64 total = each * (u64)cpus;
  put32(a, (u32)total);
  put32(a + 4, (u32)(total >> 32));
  put32(a + 8, 1);
  u8 *g = a + 0xc;
  g[0] = 0;
  g[1] = (u8)cpus;
  u8 *chunk = g + 4;
  for (int i = 0; i < cpus; i++, chunk += 16) {
    const u64 base = ev7::memory_base((u32)i);
    put32(chunk, (u32)base);
    put32(chunk + 4, (u32)(base >> 32));
    put32(chunk + 8, (u32)each);
    put32(chunk + 12, (u32)(each >> 32));
  }
}

/// The SMLAN commands the console sends (docs/platforms/marvel.md, M4).
void CEs47Cmm::answer(u32 n, const u8 *req, u32 len) {
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
    mbm_configuration(c);
    note("PID %u: SMLAN %04x (MBM configuration) id %u", n, cmd, id);
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
         get32(req + M_ORIG), id);
    respond(n, req, 3, nullptr, 0);
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
