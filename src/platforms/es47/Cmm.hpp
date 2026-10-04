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
 * The ES47's CMM (CPU Module Manager): the management processor on the
 * CPU module, at the far end of both EV7s' GIO ports (chipsets/marvel/
 * Gio.hpp). On a real machine it is an Intel 386EX (CMM3_V2_7_5.BIN says
 * "CPQ CMM3 X86", "CMM Hardware (25Mhz)") behind an FPGA; the EV7 reaches
 * the FPGA's registers through GIO, and through them a window into the
 * CMM's memory.
 *
 * Everything here was read off the console (SRM V7.3-1) and is written up
 * in docs/platforms/marvel.md, "M4: the CMM": the far-side registers
 * (status and control, the byte window, the console's terminal), the CMM
 * memory each processor has a block of (TOY, mailboxes), and the SMLAN
 * messages the console sends through the mailboxes, which on a real
 * machine the CMM answers or passes on to the MBM. The model answers them
 * itself, for the whole machine: a CMM per dual-processor module (its own
 * memory, its two processors' areas), an MBM per drawer (the configuration
 * of the drawer's modules), and one partition holding every processor and
 * IO7 the topology has (chipsets/marvel/Topology.hpp). The ES80 and the
 * GS1280 use it as the ES47 does.
 **/
#if !defined(INCLUDED_ES47_CMM_H_)
#define INCLUDED_ES47_CMM_H_

#include "StdAfx.hpp"

#include "Gio.hpp"

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class CMarvel;
class CSystem;
class CSerial;

class CEs47Cmm : public GioManagement {
public:
  /// `nvram` is the file the console's NVRAM image is kept in (the MBM's
  /// copy on a real machine).
  CEs47Cmm(CSystem *sys, const char *nvram);
  ~CEs47Cmm() override;

  bool gio_write(u32 pid, u32 reg, u64 value) override;
  bool gio_read(u32 pid, u32 reg, u64 *value) override;
  void tick() override;

  // The CMM's memory as the processors see it through the byte window.
  static constexpr u32 kMemBase = 0x40000;
  static constexpr u32 kMemSize = 0x10000;
  /// Processor n's block (n = its place on the module, PID<0>).
  static constexpr u32 block(u32 n) { return 0x40008 + n * 0x6000; }

private:
  /// The far-side registers of one processor's port.
  struct Port {
    u64 status = 0;  ///< reg 0: the bits the processor sets (<5>, <3>)
    u64 data = 0;    ///< reg 1: the byte window's 16-bit data
    u64 address = 0; ///< reg 2: the byte window's address
    u64 regs[16] = {};
    u64 reason = 0;       ///< reg 9: the terminal interrupts not yet taken
    bool tx_kick = false; ///< the transmitter became ready while enabled
  };

  /// One dual-processor module's CMM: its memory, as both of its
  /// processors see it through their byte windows.
  struct Module {
    std::vector<u8> mem;
    std::vector<bool> read_seen;
  };

  void late_init();
  Module &module_of(u32 pid);
  u8 mem_read(u32 pid, u32 a);
  void mem_write(u32 pid, u32 a, u8 v);
  u8 *mem_at(u32 pid, u32 a); ///< a byte of the module's memory, unchecked
  void window_op(u32 pid, Port &p, u64 control);
  void request_posted(u32 pid, u32 slot);
  void answer(u32 pid, const u8 *req, u32 req_len);
  void respond(u32 pid, const u8 *req, u16 status, const u8 *data, u32 len);
  void partition_database(u8 *db);
  void mbm_configuration(u32 mbm_ip, u8 *c);
  void memory_assignment(u8 *a);
  static bool is_mbm(u32 ip);
  static bool is_pbm(u32 ip);
  static const char *micro_name(u32 ip);
  int sensor_readings(u32 ip, bool volts, u8 *r);
  void nvram_load();
  void nvram_save();
  void refresh_toy(u32 pid);
  void toy_written(u32 pid, u32 idx);
  CSerial *terminal();
  void note(const char *fmt, ...);
  CMarvel *marvel() const;
  /// The processors configured, and the partition's primary (the first).
  int present() const;
  u32 primary() const;

  CSystem *m_sys;
  std::mutex m_lock;
  std::map<u32, Module> m_modules; ///< by module (PID >> 1)
  std::map<u32, Port> m_port;      ///< by PID
  CSerial *m_terminal = nullptr;
  bool m_terminal_looked = false;
  FILE *m_log = nullptr;
  bool m_trace = false;
  bool m_ready = false; ///< late_init has run
  s64 m_toy_offset = 0; ///< seconds the console set the TOY away from host
  std::string m_nvram_file;
  std::vector<u8> m_nvram; ///< the console's NVRAM image, 2 KB
};

#endif // !defined(INCLUDED_ES47_CMM_H_)
