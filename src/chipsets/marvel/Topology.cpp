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
 * The processor mesh of a Marvel machine (Topology.hpp).
 **/
#include "StdAfx.hpp"

#include "Topology.hpp"

#include <algorithm>

namespace {

/// The ES47's two processors, one module: NS 0 and 1 (a real ES47's show
/// config: "NS,EW (0,0)", "(1,0)").
void es47_default_coordinates(int index, u8 *ns, u8 *ew) {
  *ns = (u8)index;
  *ew = 0;
}

/**
 * The fields of a route entry as OpenVMS reads them (mvcpu_get_numa_distances
 * in SYS$CPU_ROUTINES for Marvel, ffffffff800178f0 in the 8.4 CD's boot;
 * docs/platforms/marvel.md, "The routes"). It takes bits <27:4> of what the
 * PALcode returns, then:
 *
 *   <4>      the entry is valid: a processor sits at that PID
 *   <8>      required, with <4> and <10>, in a processor's route to itself,
 *            or OpenVMS prints "bad route IPR for self" [guess: the route
 *            ends in the local Cbox]; set too in the route between two TPUs
 *            of one EV8 chip, which share their Cbox [decision]
 *   <10>     the route is enabled (without it the destination is
 *            unreachable for OpenVMS)
 *   <12>     with <13>: the E/W hop goes to the higher coordinate (1) or
 *            the lower (0)
 *   <13>     the route makes E/W hops
 *   <14>     with <15>: the N/S hop goes to the lower coordinate (1) or
 *            the higher (0)
 *   <15>     the route makes N/S hops
 *   <19:16>  the destination's E/W coordinate
 *   <23:20>  the destination's N/S coordinate
 *   <25:24>, <26>  a first hop in a fixed direction before the others
 *            (not used here)
 *
 * OpenVMS walks each entry from the processor's own coordinates, one
 * direction at a time, wrapping between the lowest and highest coordinates
 * present and skipping coordinates where no processor is, and counts the
 * hops. Which of the two coordinates is N/S is a [guess]: the walk is the
 * same either way.
 */
constexpr u32 R_VALID = 1u << 4;
constexpr u32 R_LOCAL = 1u << 8;
constexpr u32 R_ENABLED = 1u << 10;
constexpr u32 R_EW_UP = 1u << 12;
constexpr u32 R_EW = 1u << 13;
constexpr u32 R_NS_DOWN = 1u << 14;
constexpr u32 R_NS = 1u << 15;

/// The shortest step from `from` to `to` on a ring of the coordinates in
/// `present` (sorted, distinct): +1, -1, or 0 when they are equal. Ties go
/// up.
int ring_step(const std::vector<u8> &present, u8 from, u8 to) {
  if (from == to)
    return 0;
  int a = -1, b = -1;
  for (size_t i = 0; i < present.size(); i++) {
    if (present[i] == from)
      a = (int)i;
    if (present[i] == to)
      b = (int)i;
  }
  const int n = (int)present.size();
  if (a < 0 || b < 0 || n == 0)
    return to > from ? 1 : -1;
  const int up = ((b - a) % n + n) % n;
  return up <= n - up ? 1 : -1;
}

} // namespace

u32 CMarvelTopology::chip_pid(const marvel_layout *layout, int index) {
  u8 ns, ew;
  if (layout)
    layout->coordinates(index, &ns, &ew);
  else
    es47_default_coordinates(index, &ns, &ew);
  const u32 systype = layout ? layout->cmm_systype : 0x11;
  return coord2id((systype & 0xff) == 1, ns, ew);
}

/**
 * The bits the chips' PIDs leave free (docs/platforms/ev8.md, S4: the
 * owner's numbering of 2026-10-25). TPU 0 keeps the chip's PID; the others
 * go where no chip's PID has a bit, so each TPU's number names a processor
 * the console's PID fields can hold and no chip's: on the ES47 and the ES80
 * the module field <2:1>, which a 2P drawer leaves 0; on a GS1280, whose
 * drawers use every PID of their 8, the bits above the configured chips'.
 */
int CMarvelTopology::tpu_shift(const marvel_layout *layout, int nchips) {
  u32 used = 0;
  for (int c = 0; c < nchips; c++)
    used |= chip_pid(layout, c);
  for (int s = 0; s + 1 < kPidBits; s++)
    if (!((used >> s) & 3))
      return s;
  return -1;
}

bool CMarvelTopology::build(const marvel_layout *layout, int nchips, int tpus) {
  m_layout = layout;
  m_systype = layout ? layout->cmm_systype : 0x11;
  m_tpus = tpus < 1 ? 1 : tpus > kMaxTpus ? kMaxTpus : tpus;
  m_shift = m_tpus > 1 ? tpu_shift(layout, nchips) : 0;
  m_nodes.clear();
  if (m_shift < 0)
    return false;
  const bool gs = (m_systype & 0xff) == 1;
  for (int c = 0; c < nchips; c++) {
    ev7_node n;
    if (layout)
      layout->coordinates(c, &n.ns, &n.ew);
    else
      es47_default_coordinates(c, &n.ns, &n.ew);
    n.chip = (u32)c;
    n.chip_pid = coord2id(gs, n.ns, n.ew);
    // The TPUs sit where their chip sits: one router, one Cbox [IDS p. 46].
    for (int t = 0; t < m_tpus; t++) {
      n.tpu = (u32)t;
      n.pid = tpu_pid(n.chip_pid, n.tpu, m_shift);
      m_nodes.push_back(n);
    }
  }
  return true;
}

const ev7_node *CMarvelTopology::by_pid(u32 pid, int present) const {
  for (int i = 0; i < present && i < count(); i++)
    if (m_nodes[(size_t)i].pid == pid)
      return &m_nodes[(size_t)i];
  return nullptr;
}

u32 CMarvelTopology::max_pid(int present) const {
  u32 m = 0;
  for (int i = 0; i < present && i < count(); i++)
    if (m_nodes[(size_t)i].pid > m)
      m = m_nodes[(size_t)i].pid;
  return m;
}

bool CMarvelTopology::has_io7(u32 pid) const {
  const ev7_node *n = by_pid(pid, count());
  if (!n || n->tpu)
    return false;
  if (m_layout)
    return m_layout->has_io7(n->chip_pid);
  return n->chip_pid == 0; // the ES47: its one IO7, on PID 0
}

bool CMarvelTopology::has_memory(u32 pid, int present) const {
  const ev7_node *n = by_pid(pid, present);
  return n && !n->tpu;
}

const ev7_node *CMarvelTopology::chip_by_pid(u32 chip_pid, int present) const {
  for (int i = 0; i < present && i < count(); i++)
    if (m_nodes[(size_t)i].chip_pid == chip_pid && !m_nodes[(size_t)i].tpu)
      return &m_nodes[(size_t)i];
  return nullptr;
}

/**
 * coord2id (0x2e11b0): on an ES47/ES80 (system type 0x11) the N/S
 * coordinate alone, its bit 0 the place on the module and bits 2:1 the
 * drawer; on a GS1280 both coordinates interleaved.
 */
u32 CMarvelTopology::coord2id(bool gs1280, u8 ns, u8 ew) {
  const u32 n = ns & 0xf, e = ew & 0xf;
  if (!gs1280)
    return (n & 1) | (n & 2) << 2 | (n & 4) << 2;
  return (n & 1) | (e & 3) << 1 | (n & 2) << 2 | (e & 4) << 2 | (n & 0xc) << 3 |
         (e & 8) << 4;
}

u32 CMarvelTopology::route(u32 from, u32 to, int present) const {
  const ev7_node *a = by_pid(from, present);
  const ev7_node *b = by_pid(to, present);
  if (!a || !b)
    return 0;
  u32 r = R_VALID | R_ENABLED | (u32)b->ns << 20 | (u32)b->ew << 16;
  if (a == b || (a->ns == b->ns && a->ew == b->ew))
    return r | R_LOCAL; // itself, or another TPU of its chip
  // The coordinates that have processors, per axis: the cables close each
  // row and column into a ring (a torus on a GS1280), and OpenVMS's walk
  // wraps the same way, between the lowest and highest coordinate present.
  std::vector<u8> nss, ews;
  for (int i = 0; i < present && i < count(); i++) {
    nss.push_back(m_nodes[(size_t)i].ns);
    ews.push_back(m_nodes[(size_t)i].ew);
  }
  std::sort(nss.begin(), nss.end());
  nss.erase(std::unique(nss.begin(), nss.end()), nss.end());
  std::sort(ews.begin(), ews.end());
  ews.erase(std::unique(ews.begin(), ews.end()), ews.end());
  // N/S first, then E/W in the destination's row.
  const int dns = ring_step(nss, a->ns, b->ns);
  const int dew = ring_step(ews, a->ew, b->ew);
  if (dns)
    r |= R_NS | (dns < 0 ? R_NS_DOWN : 0);
  if (dew)
    r |= R_EW | (dew > 0 ? R_EW_UP : 0);
  return r;
}
