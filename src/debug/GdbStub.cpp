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
 * The gdb remote serial protocol, as documented in gdb's manual (appendix
 * "GDB Remote Serial Protocol"), for the Alpha: gdb's alpha-tdep register
 * layout is 67 registers of 8 bytes, little-endian.
 **/

#include "GdbStub.hpp"
#include "AlphaCPU.hpp"
#include "HostSocket.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include <csignal>
#include <cstring>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

const char kHex[] = "0123456789abcdef";

std::string hex_bytes(const u8 *p, size_t n) {
  std::string s;
  s.reserve(2 * n);
  for (size_t i = 0; i < n; i++) {
    s += kHex[p[i] >> 4];
    s += kHex[p[i] & 15];
  }
  return s;
}

std::string hex_text(const std::string &t) {
  return hex_bytes((const u8 *)t.data(), t.size());
}

int nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/// Hex digits to bytes; false on anything else.
bool unhex(const std::string &s, std::vector<u8> *out) {
  if (s.size() % 2)
    return false;
  out->clear();
  for (size_t i = 0; i < s.size(); i += 2) {
    const int a = nibble(s[i]), b = nibble(s[i + 1]);
    if (a < 0 || b < 0)
      return false;
    out->push_back((u8)(a << 4 | b));
  }
  return true;
}

/// A hex number at s[*i], advancing *i past it.
u64 hexnum(const std::string &s, size_t *i) {
  u64 v = 0;
  while (*i < s.size() && nibble(s[*i]) >= 0)
    v = v << 4 | (u64)nibble(s[(*i)++]);
  return v;
}

/// A register as gdb wants it: target byte order (little-endian).
std::string reg_hex(u64 v) {
  u8 b[8];
  for (int i = 0; i < 8; i++)
    b[i] = (u8)(v >> (8 * i));
  return hex_bytes(b, 8);
}

bool starts(const std::string &s, const char *p) {
  return s.compare(0, strlen(p), p) == 0;
}

// The target description: the architecture and the register set, in the
// feature gdb's alpha-tdep looks for (gdb 17 insists on one with
// registers; older ones take the architecture from it). The names and
// order are gdb's Alpha numbering, which the 'g' packet follows.
std::string target_xml() {
  static const char *const names[CDebugTarget::kNumRegs] = {
      "v0", "t0", "t1",  "t2",  "t3", "t4",  "t5", "t6", "t7", "s0",  "s1",
      "s2", "s3", "s4",  "s5",  "fp", "a0",  "a1", "a2", "a3", "a4",  "a5",
      "t8", "t9", "t10", "t11", "ra", "t12", "at", "gp", "sp", "zero"};
  std::string x = "<?xml version=\"1.0\"?>\n"
                  "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
                  "<target version=\"1.0\">\n"
                  "<architecture>alpha</architecture>\n"
                  "<feature name=\"org.gnu.gdb.alpha.core\">\n"
                  "<flags id=\"fpcr_flags\" size=\"8\">\n";
  // FPCR (ARM 4.7.8): the trap disables, the sticky flags, the rounding mode.
  static const struct {
    const char *name;
    int lo, hi;
  } fpcr[] = {
      {"DNOD", 47, 47}, {"DNZ", 48, 48},  {"INVD", 49, 49}, {"DZED", 50, 50},
      {"OVFD", 51, 51}, {"INV", 52, 52},  {"DZE", 53, 53},  {"OVF", 54, 54},
      {"UNF", 55, 55},  {"INE", 56, 56},  {"IOV", 57, 57},  {"DYN", 58, 59},
      {"UNDZ", 60, 60}, {"UNFD", 61, 61}, {"INED", 62, 62}, {"SUM", 63, 63}};
  for (const auto &f : fpcr) {
    char b[96];
    snprintf(b, sizeof b, "<field name=\"%s\" start=\"%d\" end=\"%d\"/>\n",
             f.name, f.lo, f.hi);
    x += b;
  }
  x += "</flags>\n";
  for (int r = 0; r < 32; r++)
    x += std::string("<reg name=\"") + names[r] + "\" bitsize=\"64\" type=\"" +
         (r == 29 || r == 30 ? "data_ptr" : "int64") + "\"/>\n";
  for (int r = 0; r < 31; r++)
    x += "<reg name=\"f" + std::to_string(r) +
         "\" bitsize=\"64\" type=\"ieee_double\" group=\"float\"/>\n";
  x += "<reg name=\"fpcr\" bitsize=\"64\" type=\"fpcr_flags\" "
       "group=\"float\"/>\n"
       "<reg name=\"pc\" bitsize=\"64\" type=\"code_ptr\"/>\n"
       "<reg name=\"\" bitsize=\"64\" type=\"int64\" save-restore=\"no\"/>\n"
       "<reg name=\"unique\" bitsize=\"64\" type=\"int64\" "
       "group=\"system\"/>\n"
       "</feature>\n</target>\n";
  return x;
}

} // namespace

#if defined(_WIN32)

CGdbStub::CGdbStub(CSystem *, int, bool) {
  printf("%%GDB-W-HOST: the gdb stub is not available on this host\n");
}
CGdbStub::~CGdbStub() {}

#else

CGdbStub::CGdbStub(CSystem *sys, int port, bool wait)
    : m_target(std::make_unique<CDebugTarget>(sys)),
      m_listen(std::make_unique<hostsock::TcpListener>()) {
  // The loopback interface only, in both families: gdb's `target remote
  // localhost:<port>` connects to whichever address its resolver gives
  // first, ::1 on macOS. A host without the one has the other.
  std::vector<hostsock::Endpoint> loopback;
  std::string why;
  for (const char *lo : {"127.0.0.1", "::1"}) {
    std::vector<hostsock::Endpoint> found;
    if (hostsock::resolve(lo, std::to_string(port), SOCK_STREAM, true, &found,
                          &why))
      loopback.insert(loopback.end(), found.begin(), found.end());
  }
  if (!m_listen->open(loopback, 1, &why))
    FAILURE_1(Runtime, "gdb: %s", why.c_str());
  for (const std::string &note : m_listen->notes())
    printf("%%GDB-I-SKIP: %s\n", note.c_str());
  if (wait)
    m_target->attach(true); // stopped before the first instruction
  printf("%%GDB-I-LISTEN: gdb stub on %s%s\n", m_listen->where().c_str(),
         wait ? "; the processors wait for it" : "");
  fflush(stdout);
  m_thread = std::thread([this] { thread_main(); });
}

CGdbStub::~CGdbStub() {
  m_quit = true;
  if (m_thread.joinable())
    m_thread.join();
}

void CGdbStub::thread_main() {
  while (!m_quit) {
    std::string peer;
    const int64_t fd = m_listen->accept(200, &peer);
    if (fd < 0)
      continue;
    m_fd = (int)fd;
    int one = 1;
    setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    session(peer);
    close(m_fd);
    m_fd = -1;
  }
}

void CGdbStub::session(const std::string &peer) {
  printf("%%GDB-I-ATTACH: gdb attached from %s; the machine stops\n",
         peer.c_str());
  fflush(stdout);
  m_noack = false;
  m_in.clear();
  m_last.clear();
  m_gcpu = 0;
  m_ccpu = -1;
  m_target->set_addr_mode(CDebugTarget::AddrMode::Auto);
  m_target->attach(true);
  if (wait_stopped()) {
    m_gcpu = m_target->stop_info().cpu;
    std::string p;
    while (!m_quit && get_packet(&p) && handle(p)) {
    }
  }
  m_target->detach();
  printf("%%GDB-I-DETACH: gdb detached; the machine runs\n");
  fflush(stdout);
}

// --- framing ----------------------------------------------------------------

bool CGdbStub::read_more(int timeout_ms) {
  pollfd p[2] = {{m_fd, POLLIN, 0}, {m_target->wake_fd(), POLLIN, 0}};
  if (poll(p, 2, timeout_ms) <= 0)
    return true;
  if (p[1].revents & POLLIN) {
    char junk[64];
    while (read(p[1].fd, junk, sizeof junk) > 0) {
    }
  }
  if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
    char buf[4096];
    const ssize_t n = recv(m_fd, buf, sizeof buf, 0);
    if (n <= 0)
      return false;
    m_in.append(buf, (size_t)n);
  }
  return true;
}

void CGdbStub::send_raw(const std::string &s) {
  size_t off = 0;
  while (off < s.size()) {
    const ssize_t n = send(m_fd, s.data() + off, s.size() - off, 0);
    if (n <= 0)
      return;
    off += (size_t)n;
  }
}

void CGdbStub::put_packet(const std::string &data) {
  std::string out = "$";
  u8 sum = 0;
  for (char c : data) {
    if (c == '$' || c == '#' || c == '}' || c == '*') {
      out += '}';
      sum += '}';
      c ^= 0x20;
    }
    out += c;
    sum += (u8)c;
  }
  out += '#';
  out += kHex[sum >> 4];
  out += kHex[sum & 15];
  m_last = out;
  send_raw(out);
}

bool CGdbStub::get_packet(std::string *out) {
  for (;;) {
    while (!m_in.empty()) {
      const char c = m_in[0];
      if (c == '-') { // gdb could not read our last packet
        m_in.erase(0, 1);
        send_raw(m_last);
        continue;
      }
      if (c != '$') { // acks, and a stray interrupt while already stopped
        m_in.erase(0, 1);
        continue;
      }
      const size_t hash = m_in.find('#');
      if (hash == std::string::npos || hash + 2 >= m_in.size())
        break; // incomplete
      std::string body = m_in.substr(1, hash - 1);
      const int h = nibble(m_in[hash + 1]), l = nibble(m_in[hash + 2]);
      m_in.erase(0, hash + 3);
      u8 sum = 0;
      for (char b : body)
        sum += (u8)b;
      if (h < 0 || l < 0 || sum != (u8)(h << 4 | l)) {
        if (!m_noack)
          send_raw("-");
        continue;
      }
      if (!m_noack)
        send_raw("+");
      out->clear();
      for (size_t i = 0; i < body.size(); i++) {
        if (body[i] == '}' && i + 1 < body.size())
          *out += (char)(body[++i] ^ 0x20);
        else
          *out += body[i];
      }
      return true;
    }
    if (m_quit || !read_more(200))
      return false;
  }
}

// --- running and stopping ---------------------------------------------------

// Until every processor has parked. Ctrl-C (a bare 0x03 from gdb) asks for
// the stop; a lost connection ends the wait, and the session with it.
bool CGdbStub::wait_stopped() {
  for (;;) {
    if (m_target->stopped())
      return true;
    if (m_quit || !read_more(100))
      return false;
    size_t i;
    while ((i = m_in.find('\x03')) != std::string::npos) {
      m_in.erase(i, 1);
      m_target->request_stop(m_gcpu, CDebugTarget::kSigInt);
    }
  }
}

bool CGdbStub::resume(const std::vector<CDebugTarget::Action> &actions) {
  m_target->resume(actions);
  if (!wait_stopped())
    return false;
  m_gcpu = m_target->stop_info().cpu;
  put_packet(stop_reply());
  return true;
}

std::string CGdbStub::stop_reply() {
  const CDebugTarget::StopInfo s = m_target->stop_info();
  char b[64];
  snprintf(b, sizeof b, "T%02xthread:%x;", s.signal, s.cpu + 1);
  std::string r = b;
  if (s.swbreak)
    r += "swbreak:;";
  // The PC and the stack pointer come with the stop, which saves gdb two
  // round trips before it can say where it is.
  u64 v;
  if (m_target->read_reg(s.cpu, 64, &v))
    r += "40:" + reg_hex(v) + ";";
  if (m_target->read_reg(s.cpu, 30, &v))
    r += "1e:" + reg_hex(v) + ";";
  return r;
}

// "1", "-1" (all), "0" (any): gdb's thread ids are processor numbers + 1.
int CGdbStub::cpu_of(const std::string &tid, bool *all) {
  *all = false;
  if (tid == "-1") {
    *all = true;
    return m_gcpu;
  }
  size_t i = 0;
  const u64 t = hexnum(tid, &i);
  if (i != tid.size() || t > (u64)m_target->ncpus())
    return -2;
  return t == 0 ? m_gcpu : (int)t - 1;
}

bool CGdbStub::vcont(const std::string &p) {
  using A = CDebugTarget::Action;
  const int n = m_target->ncpus();
  std::vector<A> act(n, A::Stay);
  std::vector<bool> set(n, false);
  size_t pos = 6; // past "vCont;"
  while (pos <= p.size()) {
    size_t end = p.find(';', pos);
    if (end == std::string::npos)
      end = p.size();
    const std::string a = p.substr(pos, end - pos);
    pos = end + 1;
    if (a.empty())
      continue;
    A what;
    if (a[0] == 'c' || a[0] == 'C')
      what = A::Continue;
    else if (a[0] == 's' || a[0] == 'S')
      what = A::Step;
    else
      return false; // 't' and 'r' are not offered (vCont?)
    const size_t colon = a.find(':');
    // The leftmost action naming a thread wins; one without a thread is
    // the default for the threads no action named.
    if (colon == std::string::npos) {
      for (int i = 0; i < n; i++)
        if (!set[i]) {
          act[i] = what;
          set[i] = true;
        }
    } else {
      bool all;
      const int c = cpu_of(a.substr(colon + 1), &all);
      if (c < 0)
        return false;
      for (int i = 0; i < n; i++)
        if ((all || i == c) && !set[i]) {
          act[i] = what;
          set[i] = true;
        }
    }
  }
  return resume(act);
}

std::string CGdbStub::read_regs(int cpu) {
  std::string r;
  for (int n = 0; n < CDebugTarget::kNumRegs; n++) {
    u64 v;
    r += m_target->read_reg(cpu, n, &v) ? reg_hex(v) : std::string(16, 'x');
  }
  return r;
}

// --- the packets ------------------------------------------------------------

bool CGdbStub::handle(const std::string &p) {
  using A = CDebugTarget::Action;
  const int n = m_target->ncpus();
  if (p.empty()) {
    put_packet("");
    return true;
  }
  switch (p[0]) {
  case '?':
    put_packet(stop_reply());
    return true;
  case 'g':
    put_packet(read_regs(m_gcpu));
    return true;
  case 'G': {
    for (int r = 0; r < CDebugTarget::kNumRegs; r++) {
      const size_t at = 1 + 16 * (size_t)r;
      if (at + 16 > p.size())
        break;
      std::vector<u8> b;
      if (!unhex(p.substr(at, 16), &b))
        continue; // "xx..": a register gdb does not have either
      u64 v = 0;
      for (int i = 0; i < 8; i++)
        v |= (u64)b[i] << (8 * i);
      m_target->write_reg(m_gcpu, r, v);
    }
    put_packet("OK");
    return true;
  }
  case 'p': {
    size_t i = 1;
    const u64 r = hexnum(p, &i);
    u64 v;
    if (r >= CDebugTarget::kNumRegs)
      put_packet("E00");
    else
      put_packet(m_target->read_reg(m_gcpu, (int)r, &v) ? reg_hex(v)
                                                        : std::string(16, 'x'));
    return true;
  }
  case 'P': {
    size_t i = 1;
    const u64 r = hexnum(p, &i);
    std::vector<u8> b;
    if (i >= p.size() || p[i] != '=' || !unhex(p.substr(i + 1), &b) ||
        b.size() != 8 || r >= CDebugTarget::kNumRegs) {
      put_packet("E00");
      return true;
    }
    u64 v = 0;
    for (int k = 0; k < 8; k++)
      v |= (u64)b[k] << (8 * k);
    put_packet(m_target->write_reg(m_gcpu, (int)r, v) ? "OK" : "E00");
    return true;
  }
  case 'm': {
    size_t i = 1;
    const u64 addr = hexnum(p, &i);
    u64 len = (i < p.size() && p[i] == ',') ? (++i, hexnum(p, &i)) : 0;
    len = std::min<u64>(len, 0x2000);
    std::vector<u8> buf(len);
    const size_t got = m_target->read_mem(m_gcpu, addr, buf.data(), len);
    put_packet(got || !len ? hex_bytes(buf.data(), got) : "E0e");
    return true;
  }
  case 'M': {
    size_t i = 1;
    const u64 addr = hexnum(p, &i);
    u64 len = (i < p.size() && p[i] == ',') ? (++i, hexnum(p, &i)) : 0;
    std::vector<u8> b;
    if (i >= p.size() || p[i] != ':' || !unhex(p.substr(i + 1), &b) ||
        b.size() != len) {
      put_packet("E00");
      return true;
    }
    put_packet(m_target->write_mem(m_gcpu, addr, b.data(), b.size()) == len
                   ? "OK"
                   : "E0e");
    return true;
  }
  case 'c':
  case 'C':
  case 's':
  case 'S': {
    // The old forms: 'c' runs every processor, 's' steps the Hc one (the Hg
    // one when Hc named none) and leaves the others stopped.
    const bool step = p[0] == 's' || p[0] == 'S';
    const int who = (m_ccpu >= 0) ? m_ccpu : m_gcpu;
    size_t i = 1;
    if (p[0] == 'C' || p[0] == 'S') { // a signal to deliver: ignored
      hexnum(p, &i);
      if (i < p.size() && p[i] == ';')
        i++;
    }
    if (i < p.size()) // resume at an address
      m_target->write_reg(who, 64, hexnum(p, &i));
    std::vector<A> act(n, step ? A::Stay : A::Continue);
    if (step)
      act[who] = A::Step;
    return resume(act);
  }
  case 'H': {
    bool all;
    const int c = (p.size() > 2) ? cpu_of(p.substr(2), &all) : -2;
    if (c < 0 || (p[1] != 'g' && p[1] != 'c')) {
      put_packet("E00");
      return true;
    }
    if (p[1] == 'g')
      m_gcpu = c;
    else
      m_ccpu = (all || p.substr(2) == "0") ? -1 : c;
    put_packet("OK");
    return true;
  }
  case 'T': {
    bool all;
    const int c = cpu_of(p.substr(1), &all);
    put_packet((c >= 0 && !all) ? "OK" : "E00");
    return true;
  }
  case 'Z':
  case 'z': {
    // Z0 (software) and Z1 (hardware) are the same thing here: the
    // dispatcher stops at the address, and guest memory is never touched.
    if (p.size() < 3 || (p[1] != '0' && p[1] != '1') || p[2] != ',') {
      put_packet(""); // watchpoints are not offered
      return true;
    }
    size_t i = 3;
    const u64 addr = hexnum(p, &i);
    m_target->set_breakpoint(addr, p[0] == 'Z');
    put_packet("OK");
    return true;
  }
  case 'D':
    put_packet("OK");
    return false;
  case 'k':
    // As QEMU does: kill ends the emulator, here the orderly way (flash and
    // DPR saved), through the main loop's SIGTERM handling.
    raise(SIGTERM);
    return false;
  case 'v':
    if (p == "vCont?") {
      put_packet("vCont;c;C;s;S");
      return true;
    }
    if (starts(p, "vCont;")) {
      if (!vcont(p)) {
        put_packet("E00");
        return true;
      }
      return true;
    }
    if (starts(p, "vKill")) {
      put_packet("OK");
      raise(SIGTERM);
      return false;
    }
    put_packet(""); // vMustReplyEmpty and everything not offered
    return true;
  case 'Q':
    if (p == "QStartNoAckMode") {
      put_packet("OK");
      m_noack = true;
      return true;
    }
    put_packet("");
    return true;
  case 'q':
    break;
  default:
    put_packet("");
    return true;
  }

  // The queries.
  if (starts(p, "qSupported")) {
    put_packet("PacketSize=4000;qXfer:features:read+;swbreak+;"
               "vContSupported+;QStartNoAckMode+");
  } else if (p == "qAttached") {
    put_packet("1"); // gdb detaches rather than kills on quit
  } else if (p == "qC") {
    char b[16];
    snprintf(b, sizeof b, "QC%x", m_gcpu + 1);
    put_packet(b);
  } else if (p == "qfThreadInfo") {
    std::string r = "m";
    for (int i = 0; i < n; i++) {
      char b[16];
      snprintf(b, sizeof b, "%s%x", i ? "," : "", i + 1);
      r += b;
    }
    put_packet(r);
  } else if (p == "qsThreadInfo") {
    put_packet("l");
  } else if (starts(p, "qThreadExtraInfo,")) {
    bool all;
    const int c = cpu_of(p.substr(17), &all);
    put_packet(c >= 0 ? hex_text(m_target->thread_info(c)) : "E00");
  } else if (starts(p, "qRcmd,")) {
    std::vector<u8> b;
    if (!unhex(p.substr(6), &b)) {
      put_packet("E00");
      return true;
    }
    const std::string out =
        m_target->monitor(m_gcpu, std::string(b.begin(), b.end()));
    for (size_t off = 0; off < out.size(); off += 1024)
      put_packet("O" + hex_text(out.substr(off, 1024)));
    put_packet("OK");
  } else if (starts(p, "qXfer:features:read:target.xml:")) {
    size_t i = strlen("qXfer:features:read:target.xml:");
    const u64 off = hexnum(p, &i);
    const u64 len = (i < p.size() && p[i] == ',') ? (++i, hexnum(p, &i)) : 0;
    static const std::string x = target_xml();
    if (off >= x.size())
      put_packet("l");
    else
      put_packet((off + len >= x.size() ? "l" : "m") + x.substr(off, len));
  } else if (starts(p, "qXfer:")) {
    put_packet("E00");
  } else if (starts(p, "qSymbol")) {
    put_packet("OK");
  } else {
    put_packet(""); // qOffsets, qTStatus and the rest: not offered
  }
  return true;
}

#endif // !defined(_WIN32)
