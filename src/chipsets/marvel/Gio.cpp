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
 * The GIO transport (Gio.hpp) and the recording management side.
 **/
#include "StdAfx.hpp"

#include "AlphaCPU.hpp"
#include "Gio.hpp"

static constexpr u64 kGioDone = U64(1) << 63;

// --- The recorder ------------------------------------------------------------

GioRecorder::GioRecorder() {
  if (const char *fn = getenv("ALPHABOX_GIO_LOG"))
    m_file = fopen(fn, "w");
}

GioRecorder::~GioRecorder() {
  if (m_file)
    fclose(m_file);
}

void GioRecorder::note(u32 pid, const char *what, u32 reg, u64 value,
                       bool has_value) {
  std::lock_guard<std::mutex> g(m_lock);
  char line[200];
  const u64 pc = t_running_cpu ? t_running_cpu->get_pc() : 0;
  if (has_value)
    snprintf(line, sizeof(line),
             "%%MVL-I-GIO: #%" PRIu64 " pid %u %s reg %u = %016" PRIx64
             " pc=%" PRIx64 "\n",
             m_seq, pid, what, reg, value, pc);
  else
    snprintf(line, sizeof(line),
             "%%MVL-I-GIO: #%" PRIu64
             " pid %u %s reg %u (no answer) pc=%" PRIx64 "\n",
             m_seq, pid, what, reg, pc);
  m_seq++;
  fputs(line, stdout);
  if (m_file) {
    fputs(line, m_file);
    fflush(m_file);
  }
}

bool GioRecorder::gio_write(u32 pid, u32 reg, u64 value) {
  note(pid, "write", reg, value, true);
  return true;
}

bool GioRecorder::gio_read(u32 pid, u32 reg, u64 *value) {
  (void)value;
  note(pid, "read ", reg, 0, false);
  return false;
}

// --- The transport -----------------------------------------------------------

void CGioPort::write_ctl(u64 v) {
  m_ctl = v;
  const u32 reg = (u32)(v >> 1) & 0x7f;
  if (!(v & 1))
    return; // the register for the next transaction, no go yet
  // A read: the answer, or a pending read the processor polls for.
  u64 answer = 0;
  if (m_far && m_far->gio_read(m_pid, reg, &answer)) {
    m_dat = answer | kGioDone;
    m_pending = false;
  } else {
    m_dat = 0;
    m_pending = true;
    m_pending_reg = reg;
  }
}

void CGioPort::write_dat(u64 v) {
  // A write to the register GIO_CTL named: done once the far side takes it.
  const u32 reg = (u32)(m_ctl >> 1) & 0x7f;
  m_pending = false;
  const bool taken = m_far && m_far->gio_write(m_pid, reg, v);
  m_dat = taken ? (v | kGioDone) : (v & ~kGioDone);
}

u64 CGioPort::read_dat() {
  if (m_pending) {
    u64 answer = 0;
    if (m_far && m_far->gio_poll(m_pid, m_pending_reg, &answer)) {
      m_dat = answer | kGioDone;
      m_pending = false;
    }
  }
  return m_dat;
}

u64 CGioPort::read_lock() {
  const u64 was = m_lock_word;
  m_lock_word = 1;
  return was;
}

void CGioPort::reset() {
  m_cfg = m_ctl = m_dat = m_lock_word = 0;
  m_pending = false;
  m_pending_reg = 0;
}

CGioPort::State CGioPort::save() const {
  return State{m_cfg, m_ctl, m_dat, m_lock_word, m_pending_reg, m_pending};
}

void CGioPort::restore(const State &s) {
  m_cfg = s.cfg;
  m_ctl = s.ctl;
  m_dat = s.dat;
  m_lock_word = s.lock;
  m_pending_reg = s.pending_reg;
  m_pending = s.pending;
}
