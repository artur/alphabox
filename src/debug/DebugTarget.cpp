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

#include "DebugTarget.hpp"
#include "AlphaCPU.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#ifdef ES40_JIT
#include "jit/jitengine.hpp"
#endif
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

static constexpr u64 kNone = ~U64(0);

CDebugTarget::CDebugTarget(CSystem *sys) : m_sys(sys) {
  m_cpus.resize(sys->get_cpu_num());
  for (int i = 0; i < ncpus(); i++) {
    m_cpus[i].c = sys->get_cpu(i);
    m_cpus[i].c->m_debug = this;
  }
#if !defined(_WIN32)
  // The processors wake the debugger's thread through this pipe when they
  // park; it polls the reading end beside its socket. Both ends
  // non-blocking: a processor must never wait on it.
  if (pipe(m_wake) == 0) {
    fcntl(m_wake[0], F_SETFL, O_NONBLOCK);
    fcntl(m_wake[1], F_SETFL, O_NONBLOCK);
  }
#endif
}

CDebugTarget::~CDebugTarget() {
  detach();
  for (auto &k : m_cpus)
    k.c->m_debug = nullptr;
#if !defined(_WIN32)
  for (int fd : m_wake)
    if (fd >= 0)
      close(fd);
#endif
}

void CDebugTarget::wake_debugger() {
#if !defined(_WIN32)
  const char b = 1;
  if (m_wake[1] >= 0 && write(m_wake[1], &b, 1) < 0) {
    // full: the debugger has a wake-up pending already
  }
#endif
}

// --- the debugger's side ---------------------------------------------------

void CDebugTarget::attach(bool stop) {
  for (auto &k : m_cpus)
    k.c->m_debug_on.store(true, std::memory_order_release);
  if (stop)
    request_stop(0, kSigTrap);
}

void CDebugTarget::detach() {
  {
    std::lock_guard<std::mutex> g(m_mx);
    m_bps.clear();
    m_bp_gen.fetch_add(1, std::memory_order_release);
    for (auto &k : m_cpus) {
      k.c->m_debug_on.store(false, std::memory_order_release);
      k.action = Action::Continue;
      k.reported_bp = kNone;
    }
    m_stop_req.store(false, std::memory_order_release);
    ++m_run_gen;
  }
  m_cv.notify_all();
}

void CDebugTarget::request_stop(int cpu, int signal, bool swbreak) {
  {
    std::lock_guard<std::mutex> g(m_mx);
    if (!m_stop_req.load(std::memory_order_relaxed)) {
      m_stop.cpu = cpu;
      m_stop.signal = signal;
      m_stop.swbreak = swbreak;
      m_stop_req.store(true, std::memory_order_release);
    }
  }
  wake_debugger();
}

// A processor is out of the way when it has parked since the last resume
// (or was told to stay parked), or when the console has not started it yet:
// a secondary waiting to be released has no instruction boundary to park
// at, and its first one will park it, because the stop is still asked for.
bool CDebugTarget::quiescent(const Cpu &k) const {
  if (k.parked && (k.seen_gen == m_run_gen || k.action == Action::Stay))
    return true;
  return *const_cast<volatile bool *>(&k.c->state.wait_for_start);
}

bool CDebugTarget::stopped() {
  std::lock_guard<std::mutex> g(m_mx);
  if (!m_stop_req.load(std::memory_order_relaxed))
    return false;
  for (const auto &k : m_cpus)
    if (!quiescent(k))
      return false;
  return true;
}

CDebugTarget::StopInfo CDebugTarget::stop_info() {
  std::lock_guard<std::mutex> g(m_mx);
  return m_stop;
}

void CDebugTarget::resume(const std::vector<Action> &actions) {
  {
    std::lock_guard<std::mutex> g(m_mx);
    int unstartable = -1;
    for (int i = 0; i < ncpus(); i++) {
      Cpu &k = m_cpus[i];
      k.action = i < (int)actions.size() ? actions[i] : Action::Continue;
      // A step asked of a processor the console has not started cannot be
      // taken: report it as done, at once, rather than wait for ever.
      if (k.action == Action::Step && !k.parked)
        unstartable = i;
    }
    ++m_run_gen;
    if (unstartable >= 0) {
      m_stop.cpu = unstartable;
      m_stop.signal = kSigTrap;
      m_stop.swbreak = false;
    } else {
      m_stop_req.store(false, std::memory_order_release);
    }
  }
  m_cv.notify_all();
  wake_debugger();
}

void CDebugTarget::set_breakpoint(u64 addr, bool on) {
  std::lock_guard<std::mutex> g(m_mx);
  addr &= ~U64(3);
  if (on)
    m_bps.insert(addr);
  else
    m_bps.erase(addr);
  m_bp_gen.fetch_add(1, std::memory_order_release);
}

// gdb's Alpha numbering: r0-r31, f0-f30, fpcr (in f31's place), pc, an
// unused slot, unique. A GPR is the one the instruction at the PC would
// name: in PALmode with I_CTL[SDE], r4-r7 and r20-r23 are the PALshadow
// bank.
static int gpr_slot(const CAlphaCPU *c, int n) {
  return n +
         (((c->state.pc & 1) && ((n & 0xc) == 0x4) && c->state.sde) ? 32 : 0);
}

bool CDebugTarget::read_reg(int cpu, int n, u64 *v) {
  CAlphaCPU *c = m_cpus[cpu].c;
  if (n >= 0 && n < 31)
    *v = c->state.r[gpr_slot(c, n)];
  else if (n == 31 || n == 65)
    *v = 0;
  else if (n >= 32 && n < 63)
    *v = c->state.f[n - 32];
  else if (n == 63)
    *v = c->read_fpcr_arch();
  else if (n == 64)
    *v = c->state.pc & ~U64(3);
  else
    return false; // unique lives where the PALcode keeps it, not in the chip
  return true;
}

bool CDebugTarget::write_reg(int cpu, int n, u64 v) {
  CAlphaCPU *c = m_cpus[cpu].c;
  if (n >= 0 && n < 31)
    c->state.r[gpr_slot(c, n)] = v;
  else if (n == 31 || n == 65)
    ; // the zero register and the placeholder take nothing
  else if (n >= 32 && n < 63)
    c->state.f[n - 32] = v;
  else if (n == 63)
    c->write_fpcr_arch(v);
  else if (n == 64) {
    // The PALmode bit is the processor's, not an address bit: keep it.
    c->state.pc = (v & ~U64(3)) | (c->state.pc & 1);
    c->break_seq_icache();
  } else
    return false;
  return true;
}

size_t CDebugTarget::read_phys(u64 pa, u8 *buf, size_t n) {
  const CAlphaCPU *c = m_cpus[0].c;
  if (pa >= c->dram_size)
    return 0;
  const size_t m = (size_t)std::min<u64>(n, c->dram_size - pa);
  memcpy(buf, c->dram_ptr + pa, m);
  return m;
}

size_t CDebugTarget::write_phys(u64 pa, const u8 *buf, size_t n) {
  const CAlphaCPU *c = m_cpus[0].c;
  if (pa >= c->dram_size)
    return 0;
  const size_t m = (size_t)std::min<u64>(n, c->dram_size - pa);
  memcpy(c->dram_ptr + pa, buf, m);
  // Code may have been patched: tell the code-page map, and have every
  // processor drop its instruction cache when it resumes, as an IMB would.
  m_sys->code_pages()->note_write_range(pa, m);
  std::lock_guard<std::mutex> g(m_mx);
  for (auto &k : m_cpus)
    k.flush = true;
  return m;
}

bool CDebugTarget::translate(int cpu, u64 va, u64 *pa, std::string *how) {
  const CAlphaCPU *c = m_cpus[cpu].c;
  const auto &s = c->state;
  // The superpages, either stream's enable (HRM 5.3.9). On the chip they
  // map kernel-mode accesses only; a debugger reads as the kernel.
  for (int stream = 0; stream < 2; stream++) {
    // The native EV8's superpages are Appendix C's (CAlphaCPU::ev8_superpage);
    // its TB entries are valid in their TPU group only, as read below.
    if (c->is_ev8n()) {
      if (c->ev8_superpage(va, stream != 0, 0, pa) == 1) {
        if (how)
          *how = stream ? "I-stream superpage" : "D-stream superpage";
        return true;
      }
      continue;
    }
    const int spe = stream ? s.i_ctl_spe : s.m_ctl_spe;
    const bool va48 = stream ? (s.i_ctl_va_mode & 1) : (s.va_ctl_va_mode & 1);
    if (!spe || !alpha_valid_va_form(va, va48))
      continue;
    bool hit = true;
    if (((va & SPE_2_MASK) == SPE_2_MATCH) && (spe & 4))
      *pa = va & SPE_2_MAP;
    else if (((va & SPE_1_MASK) == SPE_1_MATCH) && (spe & 2))
      *pa = (va & SPE_1_MAP) | ((va & SPE_1_TEST) ? SPE_1_ADD : 0);
    else if (((va & SPE_0_MASK) == SPE_0_MATCH) && (spe & 1))
      *pa = va & SPE_0_MAP;
    else
      hit = false;
    if (hit) {
      if (how)
        *how = stream ? "I-stream superpage" : "D-stream superpage";
      return true;
    }
  }
  // The TBs, read, never filled: the data TB under DTB_ASN0, the
  // instruction TB under the process's ASN.
  for (int t : {TB_INDEX_DATA, TB_INDEX_ITB}) {
    const int asn = (t == TB_INDEX_ITB) ? s.asn : s.asn0;
    for (int i = 0; i < TB_ENTRIES; i++) {
      const auto &e = s.tb[t][i];
      if (e.valid && !((e.virt ^ va) & e.match_mask) &&
          (e.asm_bit || e.asn == asn)) {
        *pa = e.phys | (va & e.keep_mask);
        if (how) {
          char b[48];
          snprintf(b, sizeof b, "%s entry %d",
                   t == TB_INDEX_ITB ? "ITB" : "DTB", i);
          *how = b;
        }
        return true;
      }
    }
  }
  // A translation the data TB evicted and would refill without PALcode.
  const auto &sh = c->m_tb_shadow[CAlphaCPU::tb_shadow_index(va)];
  if (sh.valid && sh.virt == (va & sh.match_mask) &&
      (sh.asm_bit || sh.asn == s.asn0)) {
    *pa = sh.phys | (va & sh.keep_mask);
    if (how)
      *how = "DTB shadow";
    return true;
  }
  return false;
}

size_t CDebugTarget::read_mem(int cpu, u64 addr, u8 *buf, size_t n) {
  AddrMode mode = m_addr_mode;
  if (mode == AddrMode::Auto) // PALmode fetches physically, and so do we
    mode =
        (m_cpus[cpu].c->state.pc & 1) ? AddrMode::Physical : AddrMode::Virtual;
  size_t done = 0;
  while (done < n) {
    const u64 a = addr + done;
    const size_t chunk = (size_t)std::min<u64>(n - done, 0x2000 - (a & 0x1fff));
    u64 pa = a;
    if (mode == AddrMode::Virtual && !translate(cpu, a, &pa))
      break;
    const size_t got = read_phys(pa, buf + done, chunk);
    done += got;
    if (got < chunk)
      break;
  }
  return done;
}

size_t CDebugTarget::write_mem(int cpu, u64 addr, const u8 *buf, size_t n) {
  AddrMode mode = m_addr_mode;
  if (mode == AddrMode::Auto)
    mode =
        (m_cpus[cpu].c->state.pc & 1) ? AddrMode::Physical : AddrMode::Virtual;
  size_t done = 0;
  while (done < n) {
    const u64 a = addr + done;
    const size_t chunk = (size_t)std::min<u64>(n - done, 0x2000 - (a & 0x1fff));
    u64 pa = a;
    if (mode == AddrMode::Virtual && !translate(cpu, a, &pa))
      break;
    const size_t put = write_phys(pa, buf + done, chunk);
    done += put;
    if (put < chunk)
      break;
  }
  return done;
}

// --- the processors' side --------------------------------------------------

bool CDebugTarget::has_bp(const Cpu &k, u64 pc) {
  return std::binary_search(k.bps.begin(), k.bps.end(), pc);
}

u64 CDebugTarget::next_bp(const Cpu &k, u64 pc) {
  auto it = std::upper_bound(k.bps.begin(), k.bps.end(), pc);
  return it == k.bps.end() ? kNone : *it;
}

// Guest time does not pass while a processor is parked: move its cycle
// counter's last sync and its interval-timer schedule on by the time it
// spent there, so it resumes where it left off rather than a stop's worth
// of ticks behind.
void CDebugTarget::shift_clock(CAlphaCPU *c, u64 ns) {
  c->cc_last_sync += CAlphaCPU::ns_to_host_ticks(ns);
  const auto d = std::chrono::nanoseconds(ns);
  c->next_timer_fire += d;
  c->tick_last_fire += d;
  c->start_time += d;
}

// One instruction, interpreted, with interrupts held off for it as a
// debugger's step expects: a step that lands in the interrupt handler
// instead of on the next instruction is no use. A pending interrupt is
// still pending afterwards.
void CDebugTarget::step_one(CAlphaCPU *c) {
  const bool irq = c->state.check_int.exchange(false);
#ifdef ES40_JIT
  c->execute(); // one instruction in a build with the compiler
#else
  c->m_exec_batch = 2; // the batch runs one instruction fewer than this
  c->execute();
  c->m_exec_batch = 512;
#endif
  if (irq)
    c->state.check_int.store(true);
}

bool CDebugTarget::park(CAlphaCPU *c, Cpu &k) {
  const auto t0 = std::chrono::steady_clock::now();
  Action act;
  bool flush;
  {
    std::unique_lock<std::mutex> lk(m_mx);
    k.parked = true;
    k.parked_ticks = CAlphaCPU::host_ticks();
    k.seen_gen = m_run_gen;
    wake_debugger();
    for (;;) {
      if (c->StopThread) {
        k.parked = false;
        return false;
      }
      if (k.seen_gen != m_run_gen) {
        k.seen_gen = m_run_gen;
        if (k.action != Action::Stay)
          break;
      }
      // Woken by resume(); the timeout only lets a thread that is being
      // stopped notice StopThread. No deadline: a stop lasts as long as the
      // person at the debugger wants.
      m_cv.wait_for(lk, std::chrono::milliseconds(100));
    }
    k.parked = false;
    act = k.action;
    // Resuming from a breakpoint this processor reported: run the
    // instruction under it once instead of reporting it again.
    k.skip_pc =
        (k.reported_bp == (c->state.pc & ~U64(3))) ? k.reported_bp : kNone;
    k.reported_bp = kNone;
    flush = k.flush;
    k.flush = false;
  }
  shift_clock(c, (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count());
  if (flush)
    c->flush_icache();
  if (act == Action::Step) {
    step_one(c);
    request_stop(c->get_cpuid(), kSigTrap);
  }
  return true;
}

bool CDebugTarget::boundary(CAlphaCPU *c, int *span, int *whole) {
  Cpu &k = m_cpus[c->get_cpuid()];
  for (;;) {
    if (c->StopThread)
      return false;
    // The breakpoints changed (they only do while everything is stopped):
    // take a copy, and make every compiled block go back through the
    // dispatcher before it runs again, so that none holding a breakpoint
    // is entered by a chain or a computed jump.
    const u64 gen = m_bp_gen.load(std::memory_order_acquire);
    if (k.bp_gen != gen) {
      {
        std::lock_guard<std::mutex> g(m_mx);
        k.bps.assign(m_bps.begin(), m_bps.end());
        k.bp_gen = m_bp_gen.load(std::memory_order_relaxed);
      }
#ifdef ES40_JIT
      if (c->m_jit)
        c->m_jit->note_itb_invalidate(CJitEngine::EPOCH_OTHER);
#endif
    }
    if (m_stop_req.load(std::memory_order_acquire)) {
      if (!park(c, k))
        return false;
      continue;
    }
    const u64 pc = c->state.pc & ~U64(3);
    const bool at = !k.bps.empty() && has_bp(k, pc);
    if (at && pc != k.skip_pc) {
      {
        std::lock_guard<std::mutex> g(m_mx);
        k.reported_bp = pc;
        if (!m_stop_req.load(std::memory_order_relaxed)) {
          m_stop.cpu = c->get_cpuid();
          m_stop.signal = kSigTrap;
          m_stop.swbreak = true;
          m_stop_req.store(true, std::memory_order_release);
        }
      }
      wake_debugger();
      continue;
    }
    k.skip_pc = kNone;
    // Sequential code reaches the next breakpoint after this many
    // instructions; anything else comes back here first (a jump ends an
    // interpreted span and a compiled block, an exception or interrupt is
    // taken by the interpreter).
    const u64 nb = k.bps.empty() ? kNone : next_bp(k, pc);
    const u64 room = (nb == kNone) ? (u64)INT_MAX : (nb - pc) / 4;
    *span = (int)std::min<u64>(room, (u64)INT_MAX);
    *whole = at ? 0 : *span;
    return true;
  }
}

bool CDebugTarget::may_enter(CAlphaCPU *c, u64 pc, u32 len) {
  const Cpu &k = m_cpus[c->get_cpuid()];
  if (k.bps.empty())
    return true;
  pc &= ~U64(3);
  auto it = std::lower_bound(k.bps.begin(), k.bps.end(), pc);
  return it == k.bps.end() || *it >= pc + 4 * (u64)len;
}

void CDebugTarget::interp_run(CAlphaCPU *c, int budget) {
  int span, whole;
#ifdef ES40_JIT
  // execute() is one instruction here.
  while (budget-- > 0) {
    if (!boundary(c, &span, &whole) || c->state.pc < c->m_stop_below)
      return;
    c->execute();
  }
#else
  if (!boundary(c, &span, &whole))
    return;
  if (span == INT_MAX) {
    c->execute(); // no breakpoint to watch for: an ordinary batch
    return;
  }
  // A batch checks nothing between its instructions, so take it down to
  // one instruction and ask before each (see run_until_below).
  c->m_exec_batch = 2;
  while (budget-- > 0) {
    c->execute();
    if (!boundary(c, &span, &whole))
      break;
  }
  c->m_exec_batch = 512;
#endif
}
