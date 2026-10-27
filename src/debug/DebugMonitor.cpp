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
 * The guest debugger's `monitor` commands: the Alpha state gdb has no
 * register for (docs/debugging.md lists them).
 **/

#include "AlphaCPU.hpp"
#include "DebugTarget.hpp"
#include "StdAfx.hpp"
#ifdef ES40_JIT
#include "jit/jitengine.hpp"
#endif
#include <algorithm>
#include <cstdarg>
#include <sstream>

namespace {

std::string fmt(const char *f, ...) {
  char b[512];
  va_list ap;
  va_start(ap, f);
  vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

const char *mode_name(int cm) {
  static const char *const n[4] = {"kernel", "executive", "supervisor", "user"};
  return n[cm & 3];
}

bool parse_u64(const std::string &s, u64 *v) {
  if (s.empty())
    return false;
  char *end = nullptr;
  *v = strtoull(s.c_str(), &end, 0);
  return end && *end == '\0';
}

const char *const kHelp =
    "Alphabox monitor commands (numbers are C-style: 0x for hex)\n"
    "  cpu             every processor: PC, PALmode, mode, ASN, state\n"
    "  ipr             the selected processor's internal registers\n"
    "  tlb [i|d]       the valid entries of the ITB and/or DTB\n"
    "  xlate <va>      how the selected processor maps a virtual address\n"
    "  pr <pa> [n]     n quadwords of physical memory (DRAM only)\n"
    "  irq             interrupt requests and enables, every processor\n"
    "  jit [<va>]      the JIT's blocks at an address (default: the PC)\n"
    "  mem [auto|virt|phys]  how gdb's memory accesses are addressed\n"
    "The selected processor is gdb's current thread (thread N = cpuN-1).\n";

} // namespace

std::string CDebugTarget::thread_info(int cpu) {
  const CAlphaCPU *c = m_cpus[cpu].c;
  if (*const_cast<volatile bool *>(&c->state.wait_for_start))
    return fmt("cpu%d, not started", cpu);
  return fmt("cpu%d, %s, %s", cpu, (c->state.pc & 1) ? "PALmode" : "native",
             mode_name(c->state.cm));
}

std::string CDebugTarget::monitor(int cpu, const std::string &cmd) {
  std::istringstream in(cmd);
  std::vector<std::string> w;
  for (std::string t; in >> t;)
    w.push_back(t);
  if (w.empty() || w[0] == "help")
    return kHelp;
  u64 a = 0, n = 0;
  if (w[0] == "cpu")
    return mon_cpu();
  if (w[0] == "ipr")
    return mon_ipr(cpu);
  if (w[0] == "tlb")
    return mon_tlb(cpu, w.size() > 1 ? w[1] : "");
  if (w[0] == "xlate" && w.size() > 1 && parse_u64(w[1], &a))
    return mon_xlate(cpu, a);
  if (w[0] == "pr" && w.size() > 1 && parse_u64(w[1], &a)) {
    if (w.size() < 3 || !parse_u64(w[2], &n))
      n = 1;
    return mon_pr(a, n);
  }
  if (w[0] == "irq")
    return mon_irq();
  if (w[0] == "jit") {
    if (w.size() < 2 || !parse_u64(w[1], &a))
      a = m_cpus[cpu].c->state.pc;
    return mon_jit(cpu, a);
  }
  if (w[0] == "mem") {
    static const char *const names[] = {"auto", "virt", "phys"};
    if (w.size() > 1) {
      if (w[1] == "auto")
        m_addr_mode = AddrMode::Auto;
      else if (w[1] == "virt")
        m_addr_mode = AddrMode::Virtual;
      else if (w[1] == "phys")
        m_addr_mode = AddrMode::Physical;
      else
        return "mem: auto, virt or phys\n";
    }
    return fmt("gdb's memory accesses are %s%s\n", names[(int)m_addr_mode],
               m_addr_mode == AddrMode::Auto
                   ? " (physical in PALmode, else virtual)"
                   : "");
  }
  return "unknown command or argument; \"monitor help\" lists them\n";
}

std::string CDebugTarget::mon_cpu() {
  std::string out;
  std::lock_guard<std::mutex> g(m_mx);
  for (int i = 0; i < ncpus(); i++) {
    const Cpu &k = m_cpus[i];
    const auto &s = k.c->state;
    const char *st = *const_cast<volatile bool *>(&s.wait_for_start)
                         ? "not started"
                     : k.parked ? "stopped"
                                : "running";
    out +=
        fmt("cpu%d: %-11s pc %016llx %s %-10s asn %3d pal_base %llx "
            "instructions %llu breakpoint hits %llu%s\n",
            i, st, (unsigned long long)s.pc, (s.pc & 1) ? "PAL   " : "native",
            mode_name(s.cm), s.asn, (unsigned long long)s.pal_base,
            (unsigned long long)s.instruction_count,
            (unsigned long long)k.bp_hits,
            (m_stop_req && m_stop.cpu == i) ? "  <- stopped the machine" : "");
  }
  return out;
}

std::string CDebugTarget::mon_ipr(int cpu) {
  const CAlphaCPU *c = m_cpus[cpu].c;
  const auto &s = c->state;
  std::string o = fmt("cpu%d\n", cpu);
  o += fmt("  PC         %016llx (%s)\n", (unsigned long long)s.pc,
           (s.pc & 1) ? "PALmode" : "native");
  o += fmt("  PAL_BASE   %016llx\n", (unsigned long long)s.pal_base);
  o += fmt("  EXC_ADDR   %016llx  EXC_SUM %016llx\n",
           (unsigned long long)s.exc_addr, (unsigned long long)s.exc_sum);
  o += fmt("  VA         %016llx  MM_STAT %016llx\n",
           (unsigned long long)s.fault_va, (unsigned long long)s.mm_stat);
  o += fmt("  I_CTL      sde %d hwe %d spe %d va_mode %d vptb %016llx\n",
           (int)s.sde, (int)s.hwe, s.i_ctl_spe, s.i_ctl_va_mode,
           (unsigned long long)s.i_ctl_vptb);
  o += fmt("  VA_CTL     va_mode %d vptb %016llx\n", s.va_ctl_va_mode,
           (unsigned long long)s.va_ctl_vptb);
  o +=
      fmt("  M_CTL      spe %d smc %d  DC_CTL %llx DC_STAT %llx\n", s.m_ctl_spe,
          s.smc, (unsigned long long)s.dc_ctl, (unsigned long long)s.dc_stat);
  o += fmt("  CM         %d (%s)  DTB_ALTMODE %d\n", s.cm, mode_name(s.cm),
           s.alt_cm);
  o += fmt("  ASN        %d (ITB)  DTB_ASN0 %d  DTB_ASN1 %d\n", s.asn, s.asn0,
           s.asn1);
  o +=
      fmt("  IER        eien %02x sien %04x asten %d pcen %d cren %d slen %d\n",
          s.eien, s.sien, s.asten, s.pcen, s.cren, s.slen);
  o += fmt("  SIRR       %04x  external requests %02x\n", s.sir,
           (int)s.eir.load());
  o += fmt("  PCTX       aster %x astrr %x fpen %d ppcen %d\n", s.aster,
           s.astrr, (int)s.fpen, (int)s.ppcen);
  // The cycle counter as RPCC would read it at this moment of guest time:
  // the last sync, plus what has passed since -- up to the park, for a
  // processor that is parked (guest time stands still from there).
  u64 cc = s.cc;
  if (s.cc_ena) {
    std::lock_guard<std::mutex> g(m_mx);
    const u64 at =
        m_cpus[cpu].parked ? m_cpus[cpu].parked_ticks : CAlphaCPU::host_ticks();
    u64 d = at > c->cc_last_sync ? at - c->cc_last_sync : 0;
    if (d > c->cc_tick_hz)
      d = c->cc_tick_hz; // as sync_cc_wallclock caps it
    cc += (u64)(((__uint128_t)d * c->cc_cycles_per_tick_q32) >> 32);
  }
  o += fmt("  CC         %016llx  cc_ena %d offset %08x (last sync %llx)\n",
           (unsigned long long)cc, (int)s.cc_ena, s.cc_offset,
           (unsigned long long)s.cc);
  o += fmt("  FPCR       %016llx\n", (unsigned long long)c->read_fpcr_arch());
  o += fmt("  PCTR_CTL   %016llx  I_STAT %016llx  PMPC %016llx\n",
           (unsigned long long)s.pctr_ctl, (unsigned long long)s.i_stat,
           (unsigned long long)s.pmpc);
  o += "  PALshadow ";
  for (int r : {4, 5, 6, 7, 20, 21, 22, 23})
    o += fmt(" p%d %llx", r, (unsigned long long)s.r[32 + r]);
  o += "\n";
  return o;
}

std::string CDebugTarget::mon_tlb(int cpu, const std::string &which) {
  const auto &s = m_cpus[cpu].c->state;
  std::string o;
  for (int t : {TB_INDEX_ITB, TB_INDEX_DATA}) {
    if ((which == "i" && t != TB_INDEX_ITB) ||
        (which == "d" && t != TB_INDEX_DATA))
      continue;
    o += fmt("%s (current ASN %d)\n", t == TB_INDEX_ITB ? "ITB" : "DTB",
             t == TB_INDEX_ITB ? s.asn : s.asn0);
    o += "   #  virtual          physical         page  asn  read  write  "
         "faults\n";
    int n = 0;
    for (int i = 0; i < TB_ENTRIES; i++) {
      const auto &e = s.tb[t][i];
      if (!e.valid)
        continue;
      n++;
      auto modes = [&](int rw) {
        std::string m;
        for (int cm = 0; cm < 4; cm++)
          m += e.access[rw][cm] ? "KESU"[cm] : '-';
        return m;
      };
      const u64 page = e.keep_mask + 1;
      const std::string asn = e.asm_bit ? "ASM" : fmt("%3d", e.asn);
      o += fmt("  %3d %016llx %016llx %4llu%c %-4s %s  %s   %s%s%s\n", i,
               (unsigned long long)e.virt, (unsigned long long)e.phys,
               (unsigned long long)(page >= 0x100000 ? page >> 20 : page >> 10),
               page >= 0x100000 ? 'M' : 'K', asn.c_str(), modes(0).c_str(),
               modes(1).c_str(), e.fault[0] ? "FOR " : "",
               e.fault[1] ? "FOW " : "", e.fault[2] ? "FOE" : "");
    }
    o += fmt("  %d valid of %d\n", n, TB_ENTRIES);
  }
  return o;
}

std::string CDebugTarget::mon_xlate(int cpu, u64 va) {
  u64 pa;
  std::string how;
  if (!translate(cpu, va, &pa, &how))
    return fmt("%016llx: not mapped (no superpage, TB or shadow entry)\n",
               (unsigned long long)va);
  const bool dram = pa < m_cpus[0].c->dram_size;
  return fmt("%016llx -> %016llx via %s%s\n", (unsigned long long)va,
             (unsigned long long)pa, how.c_str(), dram ? "" : " (not DRAM)");
}

std::string CDebugTarget::mon_pr(u64 pa, u64 n) {
  std::string o;
  n = std::min<u64>(n, 512);
  for (u64 i = 0; i < n; i++) {
    u64 q;
    if (read_phys(pa + 8 * i, (u8 *)&q, 8) != 8) {
      o += fmt("%016llx: not DRAM\n", (unsigned long long)(pa + 8 * i));
      break;
    }
    if (i % 2 == 0)
      o += fmt("%016llx:", (unsigned long long)(pa + 8 * i));
    o += fmt(" %016llx", (unsigned long long)q);
    if (i % 2 == 1 || i + 1 == n)
      o += "\n";
  }
  return o;
}

std::string CDebugTarget::mon_irq() {
  std::string o;
  for (int i = 0; i < ncpus(); i++) {
    const auto &s = m_cpus[i].c->state;
    const int eir = s.eir.load();
    o +=
        fmt("cpu%d: external %02x enabled %02x | software %04x enabled %04x | "
            "AST request %x enable %x (asten %d) | %s%s\n",
            i, eir, s.eien, s.sir, s.sien, s.astrr, s.aster, s.asten,
            m_cpus[i].c->int_deliverable() ? "deliverable" : "none deliverable",
            s.check_int.load() ? ", check pending" : "");
    std::string t;
    for (int l = 0; l < 6; l++)
      if (int v = s.irq_h_timer[l].load())
        t += fmt(" irq_h%d in %d", l, v);
    if (!t.empty())
      o += "      delayed:" + t + "\n";
  }
  return o;
}

std::string CDebugTarget::mon_jit(int cpu, u64 va) {
#ifdef ES40_JIT
  const CAlphaCPU *c = m_cpus[cpu].c;
  const CJitEngine *j = c->m_jit;
  if (!j)
    return "this processor has no JIT engine\n";
  std::string o =
      fmt("cpu%d JIT: epoch %llu, flush generation %llu, compiles "
          "after %u interpreted runs%s\n",
          cpu, (unsigned long long)j->vgen(),
          (unsigned long long)j->flush_gen(), j->compile_after(),
          c->m_interp_only ? " (interpreting only: ALPHABOX_INTERP)" : "");
  const u64 a = va & ~U64(3);
  std::set<u64> bps;
  {
    std::lock_guard<std::mutex> g(m_mx);
    bps = m_bps;
  }
  int n = 0;
  auto show = [&](const CJitEngine::JitBlock &b) {
    // n_instr is the last interpreted pass, which an interrupt can cut
    // short; the compiled prefix may reach further.
    const u64 lo = b.tag & ~U64(3);
    const u64 hi = lo + 4 * (u64)std::max(b.n_instr, b.prefix_len);
    if (a < lo || a >= hi)
      return;
    if (++n > 8)
      return;
    const auto bp = bps.lower_bound(lo);
    const bool held = bp != bps.end() && *bp < lo + 4 * (u64)b.prefix_len;
    o += fmt("  block %016llx..%016llx phys %016llx %s, mode %s, %u "
             "instructions\n",
             (unsigned long long)b.tag, (unsigned long long)(hi - 4),
             (unsigned long long)b.phys,
             b.asm_global ? "global" : fmt("asn %u", b.asn).c_str(),
             mode_name(b.cm), (u32)((hi - lo) / 4));
    if (!b.compiled)
      o += fmt("    not compiled yet: %u of %u interpreted runs\n", b.cold_runs,
               j->compile_after());
    else if (!b.code)
      o += "    compiled, but nothing could be emitted: always interpreted\n";
    else
      o += fmt("    compiled prefix %u instructions; %s; %s%s\n", b.prefix_len,
               b.flush_gen == j->flush_gen()
                   ? "current"
                   : "flushed, re-hashed before its next run",
               b.vgen == j->vgen() ? "chains valid in this epoch"
                                   : "chains go back through the dispatcher",
               held ? "; a breakpoint is in it: runs interpreted" : "");
  };
  j->for_each_block(show);
  if (n == 0)
    o += fmt("  no block covers %016llx (interpreted until one is "
             "recorded)\n",
             (unsigned long long)va);
  else if (n > 8)
    o += fmt("  ... %d blocks in all\n", n);
  return o;
#else
  (void)cpu;
  (void)va;
  return "this build has no JIT (configure with -DES40_DISABLE_ASMJIT=OFF)\n";
#endif
}
