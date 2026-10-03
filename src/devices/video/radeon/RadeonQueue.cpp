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
 * The Radeon's command FIFO and the engine that drains it.
 *
 * On the chip a write to a rendering-engine register -- 0x1400 and up,
 * the "Rendering Engine (GUI) Registers (FIFOed)" of the Rage 128 Pro
 * guide's register map (RRG 3-149), which the R100 kept -- goes into the
 * RBBM's command FIFO, 64 entries deep, and the engine takes the entries
 * in order at its own pace. The command processor feeds the same engine
 * from the ring buffer, indirect buffers or its PIO queue. A driver sees
 * this through RBBM_STATUS: CMDFIFO_AVAIL <6:0> (the free entries),
 * GUI_ACTIVE <31> and the per-block busy bits <24:14> (Linux r100d.h),
 * and paces itself: r100_rbbm_fifo_wait_for_entry, r100_gui_wait_for_idle,
 * X.org's RADEONWaitForFifo/RADEONWaitForIdleMMIO, nada's dd_wait_fifo.
 *
 * Here:
 *   - a CPU write to such a register is queued (the bytes it writes and
 *     their mask, merged when the engine takes it); with the FIFO full the
 *     write waits until an entry is free -- the chip holds the bus write
 *     (the RRG documents no overflow behaviour other than a full FIFO;
 *     nothing is ever dropped);
 *   - one thread is the engine: it takes the FIFO's entries in order, and
 *     when the FIFO is empty reads the CP's ring (RadeonCP.cpp), a packet
 *     at a time, so the read pointer advances while the CPU goes on;
 *   - the engine's time is modelled: every command is charged a clock and
 *     every pixel written 1/pixels_per_clock clocks of the engine clock
 *     (RadeonChip.hpp), on a timeline that starts each command when the
 *     previous one ended; the engine reads busy until the timeline has
 *     caught up with the wall clock, however fast the host drew it. The
 *     rate is the chip's nominal fill rate [inference: the R100's
 *     per-command overheads are not documented];
 *   - a CPU read of a queued register waits until the writes queued
 *     before it have been taken. The guide says reads are not queued (RRG
 *     3-149, HOST_PATH_CNTL: MREG_* "reads to all registers"); on the chip
 *     the engine takes a register write within a few clocks, long before
 *     the next bus read, and drivers rely on it (OpenVMS's server writes
 *     DEFAULT_PITCH_OFFSET and reads it back at once), so the model keeps
 *     that order rather than letting a host thread's latency show
 *     [inference];
 *   - WAIT_UNTIL (RRG 3-238): the engine stalls the FIFO until the
 *     conditions hold. The idle conditions hold by construction (the
 *     engine runs one command at a time, and the timeline orders what
 *     follows); the display conditions (CRTC page flip, VLINE) wait for
 *     the next vertical blank [inference: CRTC_GUI_TRIG_VLINE is not
 *     modelled, so any VLINE wait is a wait for the blank];
 *   - the destination caches (RB2D_DSTCACHE_CTLSTAT, RB3D_DSTCACHE_CTLSTAT,
 *     RB3D_ZCACHE_CTLSTAT; DC_FLUSH <1:0>, DC_FREE <3:2>, DC_BUSY <31>):
 *     the engine writes memory directly, so there is nothing to flush or
 *     purge, but a flush is a point in the FIFO: DC_BUSY reads set from
 *     the write until the engine has reached it and the work before it is
 *     over on the timeline. ISYNC_CNTL and RBBM_GUICNTL have nothing to
 *     order in an engine that runs one command at a time;
 *   - GEN_INT_STATUS GUI_IDLE <19> latches when the engine goes idle,
 *     and interrupts with GEN_INT_CNTL<19> (Linux radeon_drv.h
 *     RADEON_GUI_IDLE_INT_ENABLE);
 *   - RBBM_SOFT_RESET's engine bits (CP, HI, SE, RE, PP, E2, RB) hold the
 *     engine while they are set: the command running is finished, the
 *     engine's state reset, and the FIFO keeps its entries for after.
 *
 * ALPHABOX_RADEON_SYNC=1 runs every command inside the write that starts
 * it and reports the engine idle, as the model did before the FIFO.
 **/

#include "Radeon.hpp"
#include "System.hpp"

#include <algorithm>

using namespace radeon;

namespace {
thread_local bool t_in_engine = false;

constexpr u32 DSTCACHE_CTLSTAT = 0x1714;
constexpr u32 WAIT_UNTIL = 0x1720;
constexpr u32 RB3D_ZCACHE_CTLSTAT = 0x3254;
constexpr u32 RB3D_DSTCACHE_CTLSTAT = 0x325c;
constexpr u32 RB2D_DSTCACHE_CTLSTAT = 0x342c;
constexpr u32 SCRATCH_REG0 = 0x15e0;
constexpr u32 SCRATCH_REG5 = 0x15f4;
constexpr u32 DC_BUSY = 1u << 31;

// WAIT_UNTIL (radeon_reg.h)
constexpr u32 WAIT_CRTC_PFLIP = 1u << 0;
constexpr u32 WAIT_RE_CRTC_VLINE = 1u << 1;
constexpr u32 WAIT_FE_CRTC_VLINE = 1u << 2;
constexpr u32 WAIT_CRTC_VLINE = 1u << 3;

// RBBM_STATUS (r100d.h)
constexpr u32 RBBM_CP_CMDSTRM_BUSY = 1u << 16;
constexpr u32 RBBM_2D_BUSY = (1u << 17) | (1u << 18); // E2, RB2D
constexpr u32 RBBM_3D_BUSY =
    (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22) | (1u << 23) | (1u << 24);
constexpr u32 RBBM_GUI_ACTIVE = 1u << 31;

long long now_ns() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0)
      .count();
}

} // namespace

bool CRadeon::in_engine() { return t_in_engine; }
CRadeon::engine_scope::engine_scope() : saved(t_in_engine) {
  t_in_engine = true;
}
CRadeon::engine_scope::~engine_scope() { t_in_engine = saved; }

bool CRadeon::is_fifo_reg(u32 reg) {
  return reg >= 0x1400 && reg < 0x4000 && reg != RBBM_STATUS_ALT;
}

bool CRadeon::is_status_reg(u32 reg) {
  return reg == RBBM_STATUS_ALT || reg == DSTCACHE_CTLSTAT ||
         reg == RB2D_DSTCACHE_CTLSTAT || reg == RB3D_DSTCACHE_CTLSTAT ||
         reg == RB3D_ZCACHE_CTLSTAT ||
         (reg >= SCRATCH_REG0 && reg <= SCRATCH_REG5);
}

/**
 * A CPU's write to a queued register: into the FIFO, waiting for room.
 **/
void CRadeon::queue_write(u32 reg, u32 data, u32 mask) {
  if (m_sync || in_engine()) {
    std::lock_guard<std::mutex> x(m_exec_mx);
    engine_scope scope;
    queue_apply({reg, data, mask});
    return;
  }
  std::unique_lock<std::mutex> l(m_q_mx);
  if (!m_eng_thread) {
    l.unlock();
    engine_thread_start();
    l.lock();
  }
  const auto start = std::chrono::steady_clock::now();
  while (m_fifo.size() >= m_chip->cmdfifo_entries && !m_eng_stop) {
    m_q_space.wait_for(l, std::chrono::milliseconds(100));
    if (!m_fifo_warned &&
        std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
      m_fifo_warned = true;
      printf("%s: the command FIFO has been full for 10 s (engine %s); the "
             "write waits\n",
             devid_string, m_eng_held ? "held in reset" : "stalled");
    }
  }
  if (m_eng_stop) { // shutting down: no engine to wait for
    l.unlock();
    std::lock_guard<std::mutex> x(m_exec_mx);
    engine_scope scope;
    queue_apply({reg, data, mask});
    return;
  }
  m_fifo.push_back({reg, data, mask});
  m_enq_seq++;
  m_was_busy = true;
  if (reg == DSTCACHE_CTLSTAT || reg == RB2D_DSTCACHE_CTLSTAT ||
      reg == RB3D_DSTCACHE_CTLSTAT || reg == RB3D_ZCACHE_CTLSTAT)
    if (data & mask & 0xf)
      m_flush_pending++;
  if (m_eng_waiting)
    m_q_work.notify_one();
}

/**
 * The engine takes an entry: the bytes merged into the register, which
 * then acts as a write of the whole register would. Charged one clock
 * plus the pixels it wrote.
 **/
void CRadeon::queue_apply(const fifo_entry &e) {
  const u64 px0 = m_eng_pixels, px3 = m_3d->m_pixels;
  const u32 old = R(e.reg);
  reg_write32(e.reg, (old & ~e.mask) | (e.data & e.mask), old, e.mask);
  const u64 px = (m_eng_pixels - px0) + (m_3d->m_pixels - px3);
  if (m_eng_pixels != px0)
    m_last_unit = 1;
  else if (m_3d->m_pixels != px3)
    m_last_unit = 2;
  engine_charge(1 + px / m_chip->pixels_per_clock);
}

void CRadeon::engine_charge(u64 clocks) {
  if (m_sync)
    return;
  const long long ns = (long long)(clocks * 1000000ull / m_sclk_khz);
  if (ns <= 0)
    return;
  const long long now = now_ns();
  long long until = m_busy_until_ns.load(std::memory_order_relaxed);
  if (until < now)
    until = now;
  m_busy_until_ns.store(until + ns, std::memory_order_relaxed);
}

/**
 * The engine's own registers in the FIFO's order: WAIT_UNTIL and the
 * cache controls.
 **/
bool CRadeon::engine_sync_reg(u32 reg, u32 data) {
  switch (reg) {
  case WAIT_UNTIL:
    R(reg) = data;
    if (!m_sync && (data & (WAIT_CRTC_PFLIP | WAIT_RE_CRTC_VLINE |
                            WAIT_FE_CRTC_VLINE | WAIT_CRTC_VLINE))) {
      // the next vertical blank: until the CRTC is in a blank it was not
      // in when the wait began, or a frame has passed (a blank too short
      // for the polling to see), a tenth of a second at the most
      bool vb;
      long long f0, f;
      const auto give_up =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      current_vline(&vb, &f0);
      const bool vb0 = vb;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        current_vline(&vb, &f);
        if ((vb && (!vb0 || f != f0)) || f > f0 + 1 || (f > f0 && !vb0) ||
            std::chrono::steady_clock::now() >= give_up)
          break;
      }
    }
    return true;
  case DSTCACHE_CTLSTAT:
  case RB2D_DSTCACHE_CTLSTAT:
  case RB3D_DSTCACHE_CTLSTAT:
  case RB3D_ZCACHE_CTLSTAT:
    // The flush or purge itself: the engine's writes are already in
    // memory. It completes when the work before it does.
    R(reg) = data & ~DC_BUSY;
    if (data & 0xf) {
      m_flush_done_ns.store(
          std::max(m_busy_until_ns.load(std::memory_order_relaxed), now_ns()),
          std::memory_order_relaxed);
      std::lock_guard<std::mutex> l(m_q_mx);
      if (m_flush_pending > 0)
        m_flush_pending--;
    }
    return true;
  }
  return false;
}

u32 CRadeon::cache_ctlstat(u32 reg) const {
  u32 v = R(reg) & ~DC_BUSY;
  if (m_sync)
    return v;
  bool pending;
  {
    std::lock_guard<std::mutex> l(m_q_mx);
    pending = m_flush_pending > 0;
  }
  if (pending || now_ns() < m_flush_done_ns.load(std::memory_order_relaxed))
    v |= DC_BUSY;
  return v;
}

void CRadeon::engine_thread_start() {
  std::lock_guard<std::mutex> l(m_q_mx);
  if (m_eng_thread || m_sync)
    return;
  m_eng_stop = false;
  m_eng_thread = std::make_unique<std::thread>([this]() { engine_main(); });
}

void CRadeon::engine_thread_stop() {
  {
    std::lock_guard<std::mutex> l(m_q_mx);
    if (!m_eng_thread)
      return;
    m_eng_stop = true;
    m_q_work.notify_all();
    m_q_space.notify_all();
  }
  m_eng_thread->join();
  m_eng_thread.reset();
  // What the guest queued before the machine stopped still happens.
  std::lock_guard<std::mutex> x(m_exec_mx);
  engine_scope scope;
  std::deque<fifo_entry> rest;
  {
    std::lock_guard<std::mutex> l(m_q_mx);
    rest.swap(m_fifo);
    m_flush_pending = 0;
  }
  for (const fifo_entry &e : rest)
    queue_apply(e);
}

void CRadeon::stop_threads() {
  CVGACard::stop_threads();
  if (!(cSystem && cSystem->IsResetInProgress()))
    engine_thread_stop();
}

/// Call with m_q_mx held.
bool CRadeon::engine_has_work_locked() const {
  if (m_eng_held)
    return false;
  if (!m_fifo.empty())
    return true;
  if (!m_csq.empty() && cp_primary_pio() && cp_microcode_ok())
    return true;
  return cp_ring_has_work();
}

void CRadeon::engine_kick() {
  if (m_sync) {
    std::lock_guard<std::mutex> x(m_exec_mx);
    engine_scope scope;
    int guard = 0;
    while (guard++ < (1 << 22)) {
      if (!m_csq.empty() && cp_primary_pio() && cp_microcode_ok()) {
        const u32 d = m_csq.front();
        m_csq.pop_front();
        cp_feed(d);
        continue;
      }
      if (!cp_ring_has_work())
        break;
      cp_ring_step();
    }
    return;
  }
  bool start = false;
  {
    std::lock_guard<std::mutex> l(m_q_mx);
    start = !m_eng_thread;
    m_was_busy = true;
    if (m_eng_waiting)
      m_q_work.notify_one();
  }
  if (start)
    engine_thread_start();
}

/**
 * The engine: the FIFO's entries in order; with none waiting, the CP's
 * PIO queue and its ring, a packet at a time.
 **/
void CRadeon::engine_main() {
  t_in_engine = true;
  std::unique_lock<std::mutex> l(m_q_mx);
  for (;;) {
    while (!m_eng_stop && !engine_has_work_locked()) {
      l.unlock();
      engine_idle_check();
      l.lock();
      if (m_eng_stop || engine_has_work_locked())
        break;
      m_eng_waiting = true;
      m_q_work.wait_for(l, std::chrono::milliseconds(50));
      m_eng_waiting = false;
    }
    if (m_eng_stop)
      return;
    if (!m_fifo.empty()) {
      const fifo_entry e = m_fifo.front();
      m_fifo.pop_front();
      m_executing = true;
      m_q_space.notify_all();
      l.unlock();
      {
        std::lock_guard<std::mutex> x(m_exec_mx);
        queue_apply(e);
      }
      l.lock();
      m_executing = false;
      m_done_seq++;
      m_q_space.notify_all();
      continue;
    }
    if (!m_csq.empty() && cp_primary_pio()) {
      const u32 d = m_csq.front();
      m_csq.pop_front();
      m_executing = true;
      m_q_space.notify_all();
      l.unlock();
      {
        std::lock_guard<std::mutex> x(m_exec_mx);
        const u64 px0 = m_eng_pixels, px3 = m_3d->m_pixels;
        cp_feed(d);
        engine_charge(1 + ((m_eng_pixels - px0) + (m_3d->m_pixels - px3)) /
                              m_chip->pixels_per_clock);
      }
      l.lock();
      m_executing = false;
      continue;
    }
    m_executing = true;
    l.unlock();
    {
      std::lock_guard<std::mutex> x(m_exec_mx);
      const u64 px0 = m_eng_pixels, px3 = m_3d->m_pixels;
      cp_ring_step();
      engine_charge(1 + ((m_eng_pixels - px0) + (m_3d->m_pixels - px3)) /
                            m_chip->pixels_per_clock);
    }
    l.lock();
    m_executing = false;
  }
}

u32 CRadeon::read_after_queue(u32 reg) {
  if (!m_sync && !in_engine()) {
    std::unique_lock<std::mutex> l(m_q_mx);
    const u64 want = m_enq_seq;
    const auto give_up =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (m_done_seq < want && m_eng_thread && !m_eng_held &&
           std::chrono::steady_clock::now() < give_up)
      m_q_space.wait_for(l, std::chrono::milliseconds(10));
  }
  std::lock_guard<std::mutex> x(m_exec_mx);
  engine_scope scope;
  return reg_read32(reg);
}

void CRadeon::engine_drain() {
  if (m_sync || in_engine())
    return;
  const auto give_up =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    {
      std::lock_guard<std::mutex> l(m_q_mx);
      if (!m_eng_thread || m_eng_held)
        return;
      if (m_fifo.empty() && !m_executing && !engine_has_work_locked())
        return;
    }
    if (std::chrono::steady_clock::now() > give_up)
      return;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

bool CRadeon::engine_busy() const {
  if (m_sync)
    return false;
  {
    std::lock_guard<std::mutex> l(m_q_mx);
    if (!m_fifo.empty() || !m_csq.empty() || m_executing)
      return true;
  }
  if (cp_ring_pending())
    return true;
  return now_ns() < m_busy_until_ns.load(std::memory_order_relaxed);
}

u32 CRadeon::rbbm_status() const {
  if (m_sync)
    return m_chip->cmdfifo_entries;
  size_t queued;
  {
    std::lock_guard<std::mutex> l(m_q_mx);
    queued = m_fifo.size();
  }
  u32 v = m_chip->cmdfifo_entries -
          u32(std::min<size_t>(queued, m_chip->cmdfifo_entries));
  if (cp_ring_pending())
    v |= RBBM_CP_CMDSTRM_BUSY;
  if (engine_busy())
    v |= RBBM_GUI_ACTIVE | (m_last_unit == 2 ? RBBM_3D_BUSY : RBBM_2D_BUSY);
  return v;
}

void CRadeon::engine_idle_check() {
  if (m_sync)
    return;
  {
    std::lock_guard<std::mutex> l(m_q_mx);
    if (!m_was_busy)
      return;
  }
  if (engine_busy())
    return;
  {
    std::lock_guard<std::mutex> l(m_q_mx);
    m_was_busy = false;
  }
  std::lock_guard<std::mutex> l(m_int_lock);
  R(GEN_INT_STATUS) |= INT_GUI_IDLE;
  update_int_line();
}
