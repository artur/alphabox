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
 * Where the EV7s of a Marvel machine sit: their coordinates in the
 * processor mesh, the PID each one has, which ones carry an IO7, and the
 * routes between them (docs/platforms/marvel.md, "The topology").
 *
 * Every EV7 has four interprocessor ports, North, South, East and West;
 * North connects to another's South and East to another's West. The
 * machines differ in how they are cabled, and the board row says how
 * (marvel_layout, in platforms/<board>/):
 *
 *  - ES47 and ES80: 2P drawers, one dual-processor module each, connected
 *    through their N/S ports only, into a ring of up to eight processors
 *    (GS1280 Technical Summary, "Interprocessor Connectivity"): the
 *    coordinates are NS 0-7, EW 0;
 *  - GS1280: 8P drawers of four modules, all four ports used, a torus of
 *    up to 64 processors.
 *
 * The PID follows from the coordinates by the console's own rule
 * (coord2id, 0x2e11b0 in SRM V7.3-1), different for the two kinds, and the
 * console in turn reads the PID's fields as the place on the module <0>,
 * the module in its drawer <2:1>, the drawer <4:3> and the cabinet <7:5>
 * (coord2cpu, coord2mod, coord2drawer, coord2rack).
 **/
#if !defined(INCLUDED_MARVEL_TOPOLOGY_H_)
#define INCLUDED_MARVEL_TOPOLOGY_H_

#include "StdAfx.hpp"

#include <vector>

/// A board's processor layout: a field of its platform row (Platform.hpp).
struct marvel_layout {
  /// The system type the CMM holds at 0x40004 (Cmm.cpp): 0x11 for an ES47,
  /// 0x10011 for an ES80, 1 for a GS1280.
  u32 cmm_systype;
  /// Processor `index`'s coordinates in the mesh.
  void (*coordinates)(int index, u8 *ns, u8 *ew);
  /// Whether the EV7 with PID `pid` has an IO7 on its I/O port.
  bool (*has_io7)(u32 pid);
  /// The I/O backplane's revision, which the IO7 reports in IO_SYS_REV<3:0>
  /// and `show config` prints as "Backplane rev" (a real ES47: 2).
  u8 io_backplane_rev;
};

/// One EV7: its PID and its coordinates.
struct ev7_node {
  u32 pid;
  u8 ns, ew;
};

class CMarvelTopology {
public:
  /// The first `ncpus` processors of `layout` (nullptr: an ES47's two).
  void build(const marvel_layout *layout, int ncpus);

  int count() const { return (int)m_nodes.size(); }
  const ev7_node &node(int index) const { return m_nodes[(size_t)index]; }
  /// The node with PID `pid` among the first `present` processors, or
  /// nullptr.
  const ev7_node *by_pid(u32 pid, int present) const;
  /// The largest PID among the first `present` processors.
  u32 max_pid(int present) const;
  u32 cmm_systype() const { return m_systype; }
  bool has_io7(u32 pid) const;

  /// The console's coord2id: a GS1280 numbers the processors of a drawer
  /// across all four modules, an ES47/ES80 its 2P drawers along the ring.
  static u32 coord2id(bool gs1280, u8 ns, u8 ew);
  /// The PID's fields as the console reads them.
  static u32 place(u32 pid) { return pid & 1; }       ///< on its module
  static u32 module_key(u32 pid) { return pid >> 1; } ///< one CMM each
  static u32 module_in_drawer(u32 pid) { return (pid >> 1) & 3; }
  static u32 drawer_key(u32 pid) { return pid >> 3; } ///< one MBM each

  /**
   * The route from PID `from` to PID `to` among the first `present`
   * processors, as the console hands it to the operating system: one entry
   * of the RBOX_ROUTE table, which OpenVMS reads through the PALcode
   * (cserve 0x4f) and walks to measure the distances between processors.
   * 0 when either processor is not there.
   */
  u32 route(u32 from, u32 to, int present) const;

private:
  std::vector<ev7_node> m_nodes;
  u32 m_systype = 0x11;
  const marvel_layout *m_layout = nullptr;
};

#endif // !defined(INCLUDED_MARVEL_TOPOLOGY_H_)
