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
 * The EV7's GIO port: the link between a processor and its CMM (the
 * management processor on its module), over which the console reaches its
 * terminal, the TOY, its configuration and its NVRAM.
 *
 * Three registers in the processor's CSR block and a lock beside them,
 * worked out from what the console does with them (docs/platforms/marvel.md,
 * "M2: the GIO transport"); no document describes them:
 *
 *   GIO_CTL (0x120)  <n:1> a register number on the far side, <0> go.
 *   GIO_DAT (0x110)  the data; <63> set when a transaction is done.
 *   GIO_CFG (0x100)  not touched by the console before its conversation.
 *   lock  (0x80000)  reads 0 when free and takes it; written 0 to free it.
 *
 *   write: GIO_CTL = n << 1, then GIO_DAT = value; poll GIO_DAT<63>.
 *   read:  GIO_CTL = n << 1, then GIO_CTL = n << 1 | 1; poll GIO_DAT<63>,
 *          the answer is in GIO_DAT.
 *
 * The console uses register 2 for an address and register 0 for data and
 * status (a byte of the CMM's memory: send its address, read the answer,
 * write an acknowledgement); register 8 carries a word it sends before it
 * stops the processor. Everything beyond the transport -- what the CMM
 * answers -- is the management side's business, behind GioManagement.
 **/
#if !defined(INCLUDED_GIO_H_)
#define INCLUDED_GIO_H_

#include "StdAfx.hpp"

#include <cstdio>
#include <mutex>

/**
 * The far side of every processor's GIO port: the CMM, or whatever stands
 * in for it. Calls arrive from processor threads, one transaction at a time
 * per port.
 */
class GioManagement {
public:
  virtual ~GioManagement() = default;
  /// Processor `pid` writes `value` to far-side register `reg`. Returns
  /// whether it was taken (GIO_DAT<63> then reads set).
  virtual bool gio_write(u32 pid, u32 reg, u64 value) = 0;
  /// Processor `pid` starts a read of far-side register `reg`. Returns true
  /// with the answer in *value, or false when there is none yet.
  virtual bool gio_read(u32 pid, u32 reg, u64 *value) = 0;
  /// The processor polls a read that had no answer: true once there is one.
  /// Called on every poll, so it must be cheap.
  virtual bool gio_poll(u32 pid, u32 reg, u64 *value) {
    (void)pid;
    (void)reg;
    (void)value;
    return false;
  }
};

/**
 * A management side that takes every write and answers no read, and
 * records the console's transactions in order: what the console says to
 * its CMM before anything answers it, the input to packet M4. Printed as
 * %MVL-I-GIO lines, and written to the file ALPHABOX_GIO_LOG names.
 */
class GioRecorder : public GioManagement {
public:
  GioRecorder();
  ~GioRecorder() override;
  bool gio_write(u32 pid, u32 reg, u64 value) override;
  bool gio_read(u32 pid, u32 reg, u64 *value) override;

private:
  void note(u32 pid, const char *what, u32 reg, u64 value, bool has_value);
  std::mutex m_lock;
  FILE *m_file = nullptr;
  u64 m_seq = 0;
};

/// One processor's end of the link: the three registers and the lock.
class CGioPort {
public:
  CGioPort(u32 pid, GioManagement *far) : m_pid(pid), m_far(far) {}
  /// The board's management side replaces the recorder (CMarvel).
  void set_far(GioManagement *far) { m_far = far; }
  u64 read_cfg() const { return m_cfg; }
  void write_cfg(u64 v) { m_cfg = v; }
  u64 read_ctl() const { return m_ctl; }
  void write_ctl(u64 v);
  u64 read_dat();
  void write_dat(u64 v);
  /// The lock: a read returns what it held and leaves it taken.
  u64 read_lock();
  void write_lock(u64 v) { m_lock_word = v; }
  void reset();

  struct State {
    u64 cfg, ctl, dat, lock;
    u32 pending_reg;
    bool pending;
  };
  State save() const;
  void restore(const State &s);

private:
  u32 m_pid;
  GioManagement *m_far;
  u64 m_cfg = 0, m_ctl = 0, m_dat = 0, m_lock_word = 0;
  bool m_pending = false; ///< a read with no answer yet
  u32 m_pending_reg = 0;
};

#endif // !defined(INCLUDED_GIO_H_)
