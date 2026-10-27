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

#if !defined(INCLUDED_DEBUGTARGET_H)
#define INCLUDED_DEBUGTARGET_H

#include <cstdint> // datatypes.hpp builds on it

#include "datatypes.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class CAlphaCPU;
class CSystem;

/**
 * \brief The machine as a debugger sees it: run control, breakpoints,
 * registers and memory.
 *
 * The processors run on their own threads and are only ever stopped by
 * themselves, at an instruction boundary: while a debugger is attached
 * (CAlphaCPU::m_debug_on), the dispatcher calls boundary() before it runs
 * the next block, and that is where a processor parks, takes a single step
 * or learns how far it may run before the next breakpoint. The debugger's
 * own thread (CGdbStub) asks for a stop, waits until every processor has
 * parked, reads and writes their state, and lets them go again. All-stop:
 * one processor stopping stops them all.
 *
 * Guest time stands still while a processor is parked: when it resumes,
 * its cycle counter and interval-timer schedule are moved on by the time
 * it spent parked (docs/debugging.md).
 *
 * Everything that reads guest state here is free of side effects: no TB
 * fill, no fault, no device register read.
 **/
class CDebugTarget {
public:
  explicit CDebugTarget(CSystem *sys);
  ~CDebugTarget();

  /// What a resumed processor does.
  enum class Action { Stay, Continue, Step };
  /// Why the machine stopped, as a signal number for gdb.
  enum { kSigInt = 2, kSigTrap = 5 };
  struct StopInfo {
    int cpu = 0;          ///< the processor that caused it
    int signal = 5;       ///< kSigTrap or kSigInt
    bool swbreak = false; ///< it reached a breakpoint
  };
  /// How gdb's memory accesses are addressed.
  enum class AddrMode { Auto, Virtual, Physical };

  int ncpus() const { return (int)m_cpus.size(); }

  // --- the debugger's side (one thread) -----------------------------------

  /// Start watching every processor; with `stop`, ask them all to stop.
  void attach(bool stop);
  /// Remove every breakpoint, stop watching, and let everything run.
  void detach();
  /// Ask the machine to stop on behalf of processor `cpu`; the first
  /// request since the last resume names the stop.
  void request_stop(int cpu, int signal, bool swbreak = false);
  /// A stop was asked for and every processor has parked (or has not been
  /// started by the console yet).
  bool stopped();
  StopInfo stop_info();
  /// Let the processors go, each with its own action. Only while stopped.
  void resume(const std::vector<Action> &actions);
  /// What the debugger's thread polls beside its socket: the processors
  /// write a byte to the other end when they park or report a stop.
  int wake_fd() const { return m_wake[0]; }

  void set_breakpoint(u64 addr, bool on);

  /// A register in gdb's numbering for the Alpha (r0-r31, f0-f30, fpcr,
  /// pc, a placeholder, unique). False when it cannot be had.
  bool read_reg(int cpu, int n, u64 *v);
  bool write_reg(int cpu, int n, u64 v);
  static constexpr int kNumRegs = 67;

  /// Memory as processor `cpu` would address it (see AddrMode). Reads and
  /// writes stop at the first byte that cannot be reached; they return how
  /// many were done. Only DRAM is reached: a device register is never read
  /// on a debugger's behalf.
  size_t read_mem(int cpu, u64 addr, u8 *buf, size_t n);
  size_t write_mem(int cpu, u64 addr, const u8 *buf, size_t n);
  size_t read_phys(u64 pa, u8 *buf, size_t n);
  size_t write_phys(u64 pa, const u8 *buf, size_t n);
  void set_addr_mode(AddrMode m) { m_addr_mode = m; }
  AddrMode addr_mode() const { return m_addr_mode; }

  /// A virtual address through processor `cpu`'s current mapping, without
  /// touching it: the superpages, both TBs and the data TB's shadow. `how`
  /// says which of them answered.
  bool translate(int cpu, u64 va, u64 *pa, std::string *how = nullptr);

  /// `monitor` commands: the text they print (DebugMonitor.cpp).
  std::string monitor(int cpu, const std::string &cmd);
  /// One line about a processor, for gdb's thread list.
  std::string thread_info(int cpu);

  // --- the processors' side (each on its own thread) ----------------------

  /// At an instruction boundary with no compiled frame live: park while the
  /// machine is stopped, take a pending single step, report a breakpoint at
  /// the PC. False when the caller must leave its dispatch loop (its thread
  /// is being stopped). Otherwise *span is how many instructions may run
  /// from the PC before the next breakpoint (at least one, the one at the PC)
  /// and *whole the longest block that may run compiled (0 when a breakpoint
  /// is at the PC).
  bool boundary(CAlphaCPU *c, int *span, int *whole);
  /// May compiled code chain into the block at `pc` of `len` instructions?
  /// Not if a breakpoint is in it (CAlphaCPU::jit_indirect).
  bool may_enter(CAlphaCPU *c, u64 pc, u32 len);
  /// Interpret up to `budget` instructions with a boundary before each (the
  /// interpreter-only dispatch, which has no blocks to measure).
  void interp_run(CAlphaCPU *c, int budget);

private:
  struct Cpu {
    CAlphaCPU *c = nullptr;
    // Under m_mx.
    bool parked = false;
    u64 parked_ticks = 0; ///< host_ticks() when it parked
    u64 seen_gen = 0;     ///< the m_run_gen it last resumed at
    Action action = Action::Continue;
    u64 reported_bp = ~U64(0); ///< where it reported a breakpoint
    u64 bp_hits = 0;           ///< breakpoint hits it has recorded
    bool flush = false;        ///< guest memory was written: flush on resume
    // The processor's own thread only.
    u64 bp_gen = ~U64(0);  ///< m_bp_gen its copy was taken at
    std::vector<u64> bps;  ///< sorted copy of the breakpoints
    u64 skip_pc = ~U64(0); ///< resume past a breakpoint here once
  };

  bool park(CAlphaCPU *c, Cpu &k);
  void step_one(CAlphaCPU *c);
  void shift_clock(CAlphaCPU *c, u64 ns);
  bool quiescent(const Cpu &k) const;
  void wake_debugger();
  static bool has_bp(const Cpu &k, u64 pc);
  static u64 next_bp(const Cpu &k, u64 pc);

  std::string mon_cpu();
  std::string mon_ipr(int cpu);
  std::string mon_tlb(int cpu, const std::string &which);
  std::string mon_xlate(int cpu, u64 va);
  std::string mon_pr(u64 pa, u64 n);
  std::string mon_irq();
  std::string mon_jit(int cpu, u64 va);

  CSystem *m_sys;
  std::vector<Cpu> m_cpus;
  std::mutex m_mx;
  std::condition_variable m_cv; ///< parked processors wait on it
  std::atomic<bool> m_stop_req{false};
  StopInfo m_stop;              ///< under m_mx, valid while m_stop_req
  u64 m_run_gen = 0;            ///< under m_mx: bumped by every resume
  std::set<u64> m_bps;          ///< under m_mx; changed only while stopped
  std::atomic<u64> m_bp_gen{0}; ///< bumped (under m_mx) when m_bps changes
  AddrMode m_addr_mode = AddrMode::Auto;
  int m_wake[2] = {-1, -1};
};

#endif // !defined(INCLUDED_DEBUGTARGET_H)
