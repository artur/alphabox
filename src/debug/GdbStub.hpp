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

#if !defined(INCLUDED_GDBSTUB_H)
#define INCLUDED_GDBSTUB_H

#include "DebugTarget.hpp"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class CSystem;
namespace hostsock {
class TcpListener;
}

/**
 * \brief A gdb remote serial protocol server for the guest
 * (docs/debugging.md).
 *
 * Listens on the loopback interface, 127.0.0.1:<port> and [::1]:<port>
 * (either where the host has only one), and serves one gdb at a time, the
 * way QEMU's `-gdb tcp::<port>` does: all-stop, one thread per processor,
 * software
 * breakpoints by address (Z0, and Z1 taken as the same), single step,
 * registers in gdb's Alpha numbering, memory through the selected
 * processor's mapping, and `monitor` commands for the Alpha state gdb has
 * no register for. Attaching stops the machine and detaching lets it run;
 * `kill` ends the emulator as an orderly exit would.
 *
 * Everything runs on the stub's own thread; the processors meet it only
 * through CDebugTarget.
 **/
class CGdbStub {
public:
  /// With `wait`, every processor stops before its first instruction and
  /// stays stopped until a gdb attaches and lets it go.
  CGdbStub(CSystem *sys, int port, bool wait);
  ~CGdbStub();

private:
  void thread_main();
  /// One gdb's connection, from `peer`.
  void session(const std::string &peer);
  bool get_packet(std::string *out);
  void put_packet(const std::string &data);
  void send_raw(const std::string &s);
  /// Returns false to end the session (detach, kill, lost connection).
  bool handle(const std::string &p);
  bool resume(const std::vector<CDebugTarget::Action> &actions);
  bool wait_stopped();
  bool read_more(int timeout_ms);
  std::string stop_reply();
  std::string read_regs(int cpu);
  bool vcont(const std::string &p);
  int cpu_of(const std::string &tid, bool *all);

  std::unique_ptr<CDebugTarget> m_target;
  /// 127.0.0.1:<port> and [::1]:<port>.
  std::unique_ptr<hostsock::TcpListener> m_listen;
  int m_fd = -1;
  std::thread m_thread;
  std::atomic<bool> m_quit{false};
  bool m_noack = false;
  std::string m_in;   ///< bytes received and not yet consumed
  std::string m_last; ///< the last packet sent, for a retransmit
  int m_gcpu = 0;     ///< Hg: the processor registers and memory are read on
  int m_ccpu = -1;    ///< Hc: the one a plain `s` steps (-1: the Hg one)
};

#endif // !defined(INCLUDED_GDBSTUB_H)
