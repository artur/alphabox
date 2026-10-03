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
 * The Radeon's 3D engine, front half: the register ports, the 3D packets,
 * vertex fetch and decode, primitive assembly, the TCL-bypass setup and
 * clipping. See RadeonR100_3D.hpp for where the meanings come from.
 *
 * Vertices reach the engine four ways:
 *   - in the packet: 3D_DRAW_IMMD (0x29) and 3D_DRAW_IMMD_2 (0x35);
 *   - from vertex arrays: 3D_LOAD_VBPNTR (0x2f) loads up to 16 arrays
 *     (components and stride in dwords, address), 3D_DRAW_VBUF (0x28,
 *     0x34) walks them in order and 3D_DRAW_INDX (0x2a, 0x36) by the
 *     16- or 32-bit indices in the packet. With several arrays a vertex
 *     is the concatenation of each array's components (Mesa's TCL path
 *     puts position, normal, colour, ... in separate arrays);
 *   - 3D_RNDR_GEN_INDX_PRIM (0x23), the older packet: one vertex buffer
 *     at an address in the packet, walked in order or by index;
 *   - through the registers: SE_VF_CNTL with walk 3 (data), then the
 *     vertex dwords written to SE_PORT_DATA0-15 [inference: the register
 *     form of 3D_DRAW_IMMD, not used by any driver collected].
 * The packets' VF_CNTL (SE_VF_CNTL's layout): primitive <3:0>, walk
 * <5:4>, colour order RGBA <6>, Radeon mode <8>, TCL enable <9>, 32-bit
 * indices <11>, vertex count <31:16>. The vertex runs through TCL when
 * VF_CNTL<9> is set and SE_CNTL_STATUS<8> (TCL_BYPASS) is not (Mesa sets
 * <9> on its TCL draws only; its swtcl and blit draws leave it clear).
 *
 * The vertex layout (SE_VTX_FMT bits): X, Y, Z <31>, W0 <0>, the normal
 * N0 <18>, float diffuse RGB <1> and alpha <2>, packed diffuse <3>, float
 * specular RGB <4>, float fog <5>, packed specular with the fog factor in
 * its alpha <6>, then the texture coordinate sets S,T <7,8,10,12> each
 * followed by its Q <14,9,11,13>. The order is Mesa's (radeon_swtcl.c,
 * radeon_maos_vbtmp.h, radeon_maos_arrays.c); the dword count is Linux's
 * r100_get_vtx_size. The second position, blend weights and N1 are
 * counted but not used.
 **/

#include "RadeonR100_3D.hpp"
#include "Radeon.hpp" // CRadeonEngineBus's definitions

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace radeon::r100;

namespace {
enum : u8 {
  OP_3D_RNDR_GEN_INDX_PRIM = 0x23,
  OP_3D_DRAW_VBUF = 0x28,
  OP_3D_DRAW_IMMD = 0x29,
  OP_3D_DRAW_INDX = 0x2a,
  OP_3D_LOAD_VBPNTR = 0x2f,
  OP_3D_CLEAR_ZMASK = 0x32,
  OP_INDX_BUFFER = 0x33,
  OP_3D_DRAW_VBUF_2 = 0x34,
  OP_3D_DRAW_IMMD_2 = 0x35,
  OP_3D_DRAW_INDX_2 = 0x36,
  OP_3D_CLEAR_HIZ = 0x37
};
constexpr u32 TCL_BYPASS = 1u << 8; // SE_CNTL_STATUS
constexpr u32 VF_TCL_ENABLE = 1u << 9;

inline float f32(u32 v) {
  float f;
  memcpy(&f, &v, 4);
  return f;
}
} // namespace

CRadeonR100_3D::CRadeonR100_3D(const CRadeonEngineBus &bus) : c(bus) {
  reset();
}

void CRadeonR100_3D::reset() {
  memset(m_vec, 0, sizeof(m_vec));
  memset(m_scl, 0, sizeof(m_scl));
  m_vec_index = m_vec_comp = m_scl_index = 0;
  memset(m_stipple, 0, sizeof(m_stipple));
  memset(m_aos, 0, sizeof(m_aos));
  m_aos_count = 0;
  m_port.clear();
  m_port_active = false;
}

u32 CRadeonR100_3D::reg(u32 r) const { return c.R(r); }
float CRadeonR100_3D::regf(u32 r) const { return f32(c.R(r)); }

void CRadeonR100_3D::warn_once(int id, const char *what) {
  if (id < 0 || id >= 64 || m_warned[id])
    return;
  m_warned[id] = true;
  printf("%s: 3D: %s\n", c.devid(), what);
}

/// A dword the engine reads by bus mastering: the framebuffer, or the
/// AGP/PCI window (CRadeonEngineBus::bm_translate).
u32 CRadeonR100_3D::mem_read32(u32 mc) { return c.bm_read32(mc); }

u8 CRadeonR100_3D::mem_read8(u32 mc) {
  bool vram;
  u32 a;
  if (c.bm_translate(mc, &vram, &a) && vram)
    return c.vram_byte(a);
  return u8(mem_read32(mc & ~3u) >> (8 * (mc & 3)));
}

/**
 * The ports. SE_TCL_VECTOR_INDX_REG <15:0> is the vector, <27:16> the
 * stride in vectors after every fourth dword written to
 * SE_TCL_VECTOR_DATA_REG; SE_TCL_SCALAR_INDX_REG the same for scalars,
 * a dword at a time (Linux radeon_drv.h VEC_INDX_OCTWORD_STRIDE_SHIFT,
 * SCAL_INDX_DWORD_STRIDE_SHIFT; Mesa radeon_state_init.c OUT_VEC/OUT_SCL).
 * RE_STIPPLE_DATA loads the 32x32 polygon stipple a row at a time from
 * RE_STIPPLE_ADDR on.
 **/
bool CRadeonR100_3D::reg_write(u32 r, u32 data) {
  switch (r) {
  case SE_TCL_VECTOR_INDX_REG:
    c.R(r) = data;
    m_vec_index = data & 0xffff;
    m_vec_comp = 0;
    return true;
  case SE_TCL_VECTOR_DATA_REG: {
    const u32 i = m_vec_index & 127;
    m_vec[i][m_vec_comp] = f32(data);
    if (++m_vec_comp == 4) {
      m_vec_comp = 0;
      u32 stride = (c.R(SE_TCL_VECTOR_INDX_REG) >> 16) & 0xfff;
      m_vec_index += stride ? stride : 1;
    }
    return true;
  }
  case SE_TCL_SCALAR_INDX_REG:
    c.R(r) = data;
    m_scl_index = data & 0xffff;
    return true;
  case SE_TCL_SCALAR_DATA_REG: {
    m_scl[m_scl_index & 63] = f32(data);
    u32 stride = (c.R(SE_TCL_SCALAR_INDX_REG) >> 16) & 0xfff;
    m_scl_index += stride ? stride : 1;
    return true;
  }
  case 0x1a14: // RADEON_FOG_TABLE_INDEX
    c.R(r) = data;
    m_fog_index = data & 0xff;
    return true;
  case 0x1a18: // RADEON_FOG_TABLE_DATA: four entries, low byte first
               // [inference]
    for (int k = 0; k < 4; k++)
      m_fog_table[(m_fog_index + u32(k)) & 0xff] = u8(data >> (8 * k));
    m_fog_index = (m_fog_index + 4) & 0xff;
    c.R(0x1a14) = m_fog_index;
    return true;
  case RE_STIPPLE_DATA: {
    const u32 a = c.R(RE_STIPPLE_ADDR);
    m_stipple[a & 31] = data;
    c.R(RE_STIPPLE_ADDR) = (a + 1) & 31;
    return true;
  }
  case SE_VF_CNTL:
    c.R(r) = data;
    if (((data >> 4) & 3) == WALK_DATA && (data >> 16)) {
      m_port_fmt = c.R(SE_VTX_FMT);
      m_port_cntl = data;
      m_port_need = u32(vertex_dwords(m_port_fmt)) * (data >> 16);
      m_port.clear();
      m_port_active = true;
    } else if (((data >> 4) & 3) == WALK_LIST && (data >> 16)) {
      draw(c.R(SE_VTX_FMT), data, {}, nullptr, 0, 0);
    }
    return true;
  }
  if (r >= SE_PORT_DATA0 && r <= SE_PORT_DATA_LAST) {
    c.R(r) = data;
    if (m_port_active) {
      m_port.push_back(data);
      if (m_port.size() >= m_port_need) {
        m_port_active = false;
        draw(m_port_fmt, m_port_cntl, m_port, nullptr, 0, 0);
      }
    }
    return true;
  }
  return false;
}

bool CRadeonR100_3D::reg_read(u32 r, u32 *v) {
  switch (r) {
  case SE_TCL_VECTOR_DATA_REG: {
    const float f = m_vec[m_vec_index & 127][m_vec_comp];
    memcpy(v, &f, 4);
    if (++m_vec_comp == 4) {
      m_vec_comp = 0;
      u32 stride = (c.R(SE_TCL_VECTOR_INDX_REG) >> 16) & 0xfff;
      m_vec_index += stride ? stride : 1;
    }
    return true;
  }
  case SE_TCL_SCALAR_DATA_REG: {
    memcpy(v, &m_scl[m_scl_index & 63], 4);
    u32 stride = (c.R(SE_TCL_SCALAR_INDX_REG) >> 16) & 0xfff;
    m_scl_index += stride ? stride : 1;
    return true;
  }
  case RE_STIPPLE_DATA:
    *v = m_stipple[c.R(RE_STIPPLE_ADDR) & 31];
    return true;
  }
  return false;
}

/// The dwords one vertex of format `fmt` takes (Linux r100_get_vtx_size).
int CRadeonR100_3D::vertex_dwords(u32 f) const {
  int n = 2;
  if (f & VTX_W0)
    n++;
  if (f & VTX_FPCOLOR)
    n += 3;
  if (f & VTX_FPALPHA)
    n++;
  if (f & VTX_PKCOLOR)
    n++;
  if (f & VTX_FPSPEC)
    n += 3;
  if (f & VTX_FPFOG)
    n++;
  if (f & VTX_PKSPEC)
    n++;
  if (f & VTX_ST0)
    n += 2;
  if (f & VTX_ST1)
    n += 2;
  if (f & VTX_Q1)
    n++;
  if (f & VTX_ST2)
    n += 2;
  if (f & VTX_Q2)
    n++;
  if (f & VTX_ST3)
    n += 2;
  if (f & VTX_Q3)
    n++;
  if (f & VTX_Q0)
    n++;
  n += int((f >> VTX_WEIGHT_SHIFT) & 7);
  if (f & VTX_N0)
    n += 3;
  if (f & VTX_XY1)
    n += 2;
  if (f & VTX_Z1)
    n++;
  if (f & VTX_W1)
    n++;
  if (f & VTX_N1)
    n++;
  if (f & VTX_Z)
    n++;
  return n;
}

/// A packed colour as 0..1 floats, in the byte order VF_CNTL<6> gives:
/// RGBA in memory order (Mesa's choice), else the dword ARGB.
static void unpack_color(u32 v, bool rgba_order, float out[4]) {
  const float k = 1.0f / 255.0f;
  if (rgba_order) {
    out[0] = float(v & 0xff) * k;
    out[1] = float((v >> 8) & 0xff) * k;
    out[2] = float((v >> 16) & 0xff) * k;
  } else {
    out[2] = float(v & 0xff) * k;
    out[1] = float((v >> 8) & 0xff) * k;
    out[0] = float((v >> 16) & 0xff) * k;
  }
  out[3] = float(v >> 24) * k;
}

void CRadeonR100_3D::decode_vertex(u32 f, u32 cntl, const u32 *d,
                                   RadeonVertexIn &v) const {
  memset(&v, 0, sizeof(v));
  v.pos[3] = 1.0f;
  v.col[0] = v.col[1] = v.col[2] = v.col[3] = 1.0f;
  for (int t = 0; t < 4; t++)
    v.tex[t][3] = 1.0f;
  const bool rgba = (cntl & VF_COLOR_ORDER_RGBA) != 0;
  int i = 0;
  v.pos[0] = f32(d[i++]);
  v.pos[1] = f32(d[i++]);
  if (f & VTX_Z) {
    v.pos[2] = f32(d[i++]);
    v.has_z = true;
  }
  if (f & VTX_W0) {
    v.pos[3] = f32(d[i++]);
    v.has_w = true;
  }
  // the blend weights, after the position [inference: no driver emits
  // them on the R100; r100_get_vtx_size counts them]
  v.nweights = int((f >> VTX_WEIGHT_SHIFT) & 7);
  for (int k = 0; k < v.nweights; k++) {
    const float w = f32(d[i++]);
    if (k < 4)
      v.weight[k] = w;
  }
  if (v.nweights > 4)
    v.nweights = 4;
  if (f & VTX_N0) {
    for (int k = 0; k < 3; k++)
      v.norm[k] = f32(d[i++]);
    v.has_norm = true;
  }
  if (f & VTX_FPCOLOR) {
    for (int k = 0; k < 3; k++)
      v.col[k] = f32(d[i++]);
    v.has_col = true;
  }
  if (f & VTX_FPALPHA) {
    v.col[3] = f32(d[i++]);
    v.has_alpha = true;
  }
  if (f & VTX_PKCOLOR) {
    unpack_color(d[i++], rgba, v.col);
    v.has_col = v.has_alpha = true;
  }
  if (f & VTX_FPSPEC) {
    for (int k = 0; k < 3; k++)
      v.spec[k] = f32(d[i++]);
    v.has_spec = true;
  }
  if (f & VTX_FPFOG) {
    v.fog = f32(d[i++]);
    v.spec[3] = v.fog;
    v.has_fog = true;
  }
  if (f & VTX_PKSPEC) {
    unpack_color(d[i++], rgba, v.spec);
    v.fog = v.spec[3];
    v.has_spec = v.has_fog = true;
  }
  static const u32 st_bit[4] = {VTX_ST0, VTX_ST1, VTX_ST2, VTX_ST3};
  static const u32 q_bit[4] = {VTX_Q0, VTX_Q1, VTX_Q2, VTX_Q3};
  for (int t = 0; t < 4; t++) {
    if (f & st_bit[t]) {
      v.tex[t][0] = f32(d[i++]);
      v.tex[t][1] = f32(d[i++]);
      v.has_tex[t] = true;
      if (f & q_bit[t]) {
        v.tex[t][3] = f32(d[i++]);
        v.has_q[t] = true;
      }
    }
  }
}

/// One vertex from the loaded arrays: each array's components, in array
/// order, at `index` times its stride.
void CRadeonR100_3D::fetch_aos(u32 index, std::vector<u32> &out) {
  out.clear();
  for (u32 a = 0; a < m_aos_count; a++) {
    const aos_t &s = m_aos[a];
    const u32 base = s.addr + index * s.stride * 4;
    for (u32 k = 0; k < s.comps; k++)
      out.push_back(mem_read32(base + 4 * k));
  }
}

bool CRadeonR100_3D::tcl_enabled() const {
  return !(c.R(SE_CNTL_STATUS) & TCL_BYPASS);
}

/**
 * The packets. The draws take the vertex format either from the packet
 * (the original forms) or from SE_VTX_FMT (the _2 forms), and VF_CNTL
 * from the packet. Indices come 16 bits at a time, the first in the low
 * half of each dword, unless VF_CNTL<11> asks for 32.
 **/
bool CRadeonR100_3D::packet3(u8 op, const std::vector<u32> &d) {
  auto indices_from = [&](size_t i, u32 cntl, std::vector<u32> &ix) {
    const u32 n = cntl >> 16;
    for (; i < d.size() && ix.size() < n; i++) {
      if (cntl & VF_INDEX_32) {
        ix.push_back(d[i]);
      } else {
        ix.push_back(d[i] & 0xffff);
        if (ix.size() < n)
          ix.push_back(d[i] >> 16);
      }
    }
  };
  // The R200 microcode's packets: the legacy DRM refuses the _2 draws,
  // 3D_CLEAR_HIZ and INDX_BUFFER unless the R200 microcode is loaded
  // ("safe but r200 only", radeon_state.c radeon_check_and_fixup_packet3),
  // so the R100 microcode does not know them: on a part that loads it
  // they do nothing.
  switch (op) {
  case OP_3D_DRAW_IMMD_2:
  case OP_3D_DRAW_VBUF_2:
  case OP_3D_DRAW_INDX_2:
  case OP_3D_CLEAR_HIZ:
  case OP_INDX_BUFFER:
    if (!c.chip().r200_cp_packets && !m_r200_packets &&
        c.cp_microcode_packet(op) != 1) {
      warn_once(3, "an R200 microcode packet (_2 draw, CLEAR_HIZ, "
                   "INDX_BUFFER), which the R100 microcode does not have: "
                   "ignored");
      return true;
    }
    break;
  }
  switch (op) {
  case OP_3D_LOAD_VBPNTR: {
    if (d.empty())
      return true;
    m_aos_count = std::min<u32>(d[0], 16);
    size_t i = 1;
    for (u32 a = 0; a < m_aos_count; a += 2) {
      if (i >= d.size())
        break;
      const u32 attr = d[i++];
      m_aos[a].comps = attr & 0xff;
      m_aos[a].stride = (attr >> 8) & 0xff;
      m_aos[a].addr = i < d.size() ? d[i++] : 0;
      if (a + 1 < m_aos_count) {
        m_aos[a + 1].comps = (attr >> 16) & 0xff;
        m_aos[a + 1].stride = (attr >> 24) & 0xff;
        m_aos[a + 1].addr = i < d.size() ? d[i++] : 0;
      }
    }
    return true;
  }
  case OP_3D_DRAW_IMMD:
    if (d.size() >= 2)
      draw(d[0], d[1], std::vector<u32>(d.begin() + 2, d.end()), nullptr, 0, 0);
    return true;
  case OP_3D_DRAW_IMMD_2:
    if (!d.empty())
      draw(c.R(SE_VTX_FMT), d[0], std::vector<u32>(d.begin() + 1, d.end()),
           nullptr, 0, 0);
    return true;
  case OP_3D_DRAW_VBUF:
    if (d.size() >= 2)
      draw(d[0], d[1], {}, nullptr, 0, 0);
    return true;
  case OP_3D_DRAW_VBUF_2:
    if (!d.empty())
      draw(c.R(SE_VTX_FMT), d[0], {}, nullptr, 0, 0);
    return true;
  case OP_3D_DRAW_INDX:
  case OP_3D_DRAW_INDX_2: {
    const size_t at = op == OP_3D_DRAW_INDX ? 2 : 1;
    if (d.size() < at)
      return true;
    const u32 fmt = op == OP_3D_DRAW_INDX ? d[0] : c.R(SE_VTX_FMT);
    const u32 cntl = d[at - 1];
    // with no indices in the packet, an INDX_BUFFER supplies them (R5xx
    // Acceleration 6.2.3.11), where the microcode has it
    if (d.size() == at && (cntl >> 16) &&
        (c.chip().r200_cp_packets || m_r200_packets ||
         c.cp_microcode_packet(OP_INDX_BUFFER) == 1)) {
      m_indx_pending = true;
      m_indx_fmt = fmt;
      m_indx_cntl = cntl;
      return true;
    }
    std::vector<u32> ix;
    indices_from(at, cntl, ix);
    draw(fmt, cntl, {}, &ix, 0, 0);
    return true;
  }
  case OP_3D_RNDR_GEN_INDX_PRIM:
    // [vertex buffer address] [vertex count or highest index] [VTX_FMT]
    // [VF_CNTL] [indices...] (Mesa radeon_ioctl.c, RADEON_OLD_PACKETS)
    if (d.size() >= 4) {
      std::vector<u32> ix;
      const u32 walk = (d[3] >> 4) & 3;
      if (walk == WALK_INDEX)
        indices_from(4, d[3], ix);
      draw(d[2], d[3], {}, walk == WALK_INDEX ? &ix : nullptr, d[0],
           d[1] ? d[1] : 1);
    }
    return true;
  case OP_3D_CLEAR_ZMASK:
    if (d.size() >= 3)
      clear_zmask(d[0], d[1], d[2]);
    return true;
  case OP_3D_CLEAR_HIZ:
    // the hierarchical Z RAM only lets the chip skip work: clearing it
    // changes no pixel
    return true;
  case OP_INDX_BUFFER: {
    // [ONE_REG_WR <31> | SKIP_COUNT <18:16> | destination <12:0>]
    // [BUFFER_BASE] [BUFFER_SIZE, dwords] (R5xx Acceleration 6.2.3.11):
    // the buffer is read as the pending draw's indices; the destination
    // (the vertex port) is where they would go. ONE_REG_WR marks a buffer
    // that starts in the upper half of a dword: its first 16-bit index is
    // skipped [inference].
    if (d.size() < 3 || !m_indx_pending)
      return true;
    m_indx_pending = false;
    const u32 n = m_indx_cntl >> 16;
    const u32 skip = (d[0] >> 16) & 7;
    std::vector<u32> ix;
    bool skip_half = (d[0] >> 31) & 1;
    for (u32 k = skip; k < (d[2] & 0x7fffff) && ix.size() < n; k++) {
      const u32 v = c.bm_fetch(d[1] + 4 * k);
      if (m_indx_cntl & VF_INDEX_32) {
        ix.push_back(v);
        continue;
      }
      if (!skip_half)
        ix.push_back(v & 0xffff);
      skip_half = false;
      if (ix.size() < n)
        ix.push_back(v >> 16);
    }
    draw(m_indx_fmt, m_indx_cntl, {}, &ix, 0, 0);
    return true;
  }
  }
  return false;
}

/**
 * A draw: decode every vertex the walk names, run it through TCL or the
 * bypass setup, and hand the list to primitive assembly.
 *   `vdata`    the vertex dwords of a data walk;
 *   `indices`  an index walk's indices (else the walk is in order);
 *   `vbuf_mc`  3D_RNDR_GEN_INDX_PRIM's single vertex buffer (0: the
 *              arrays of 3D_LOAD_VBPNTR).
 **/
void CRadeonR100_3D::draw(u32 fmt, u32 cntl, const std::vector<u32> &vdata,
                          const std::vector<u32> *indices, u32 vbuf_mc,
                          u32 vbuf_max) {
  const u32 prim = cntl & 15;
  const u32 walk = (cntl >> 4) & 3;
  u32 n = cntl >> 16;
  const int vd = vertex_dwords(fmt);
  std::vector<RadeonVertex> verts;
  std::vector<u32> one;
  RadeonVertexIn in;
  auto fetch = [&](u32 index) {
    if (vbuf_mc) {
      one.resize(size_t(vd));
      for (int k = 0; k < vd; k++)
        one[size_t(k)] = mem_read32(vbuf_mc + (index * u32(vd) + u32(k)) * 4);
    } else {
      fetch_aos(index, one);
      if (one.size() < size_t(vd))
        one.resize(size_t(vd), 0);
    }
    decode_vertex(fmt, cntl, one.data(), in);
    RadeonVertex out;
    process_vertex(fmt, cntl, in, out);
    verts.push_back(out);
  };

  if (walk == WALK_DATA) {
    if (vdata.size() < size_t(vd) * n)
      n = u32(vdata.size() / size_t(vd));
    for (u32 k = 0; k < n; k++) {
      decode_vertex(fmt, cntl, &vdata[size_t(k) * size_t(vd)], in);
      RadeonVertex out;
      process_vertex(fmt, cntl, in, out);
      verts.push_back(out);
    }
  } else if (indices) {
    for (u32 ix : *indices)
      fetch(ix);
  } else {
    if (vbuf_mc && n > vbuf_max && vbuf_max)
      n = vbuf_max;
    for (u32 k = 0; k < n; k++)
      fetch(k);
  }
  if (verts.empty())
    return;
  raster_setup();
  assemble(prim, verts);
}

/// TCL or the bypass setup.
void CRadeonR100_3D::process_vertex(u32 fmt, u32 cntl, const RadeonVertexIn &in,
                                    RadeonVertex &out) {
  (void)fmt;
  if ((cntl & VF_TCL_ENABLE) && tcl_enabled()) {
    tcl_vertex(in, out);
    return;
  }
  out.x = in.pos[0];
  out.y = in.pos[1];
  out.z = in.pos[2];
  out.w = in.has_w ? in.pos[3] : 1.0f;
  memcpy(out.col, in.col, sizeof(out.col));
  memcpy(out.spec, in.spec, sizeof(out.spec));
  if (!in.has_fog)
    out.spec[3] = 1.0f; // no fog
  memcpy(out.tex, in.tex, sizeof(out.tex));
  out.clip_space = false;
  bypass_to_window(out);
}

/**
 * The bypass setup's coordinates (SE_COORD_FMT, SE_CNTL; the bit names
 * are radeon_reg.h's, their reading Mesa's radeon_swtcl.c): W0 is 1/w
 * unless W0_IS_NOT_1_OVER_W0 <16> says it is w; XY_PRE_MULT_1_OVER_W0
 * <0> and Z_PRE_MULT_1_OVER_W0 <1> divide X, Y and Z by w (the
 * perspective divide); STn_PRE_MULT_1_OVER_W0 <17,19,21,23> divide the
 * texture coordinates as well. Then the viewport (SE_VPORT_*), when
 * SE_CNTL's VPORT_XY_XFORM_ENABLE <24> and VPORT_Z_XFORM_ENABLE <25> ask.
 **/
void CRadeonR100_3D::bypass_to_window(RadeonVertex &v) const {
  const u32 cf = c.R(SE_COORD_FMT), se = c.R(SE_CNTL);
  float rhw = v.w;
  if (cf & (1u << 16))
    rhw = v.w != 0.0f ? 1.0f / v.w : 1.0f;
  if (cf & 1) {
    v.x *= rhw;
    v.y *= rhw;
  }
  if (cf & 2)
    v.z *= rhw;
  for (int t = 0; t < 4; t++)
    if (cf & (1u << (17 + 2 * t))) {
      v.tex[t][0] *= rhw;
      v.tex[t][1] *= rhw;
    }
  if (se & (1u << 24)) {
    v.x = v.x * regf(SE_VPORT_XSCALE) + regf(SE_VPORT_XOFFSET);
    v.y = v.y * regf(SE_VPORT_YSCALE) + regf(SE_VPORT_YOFFSET);
  }
  if (se & (1u << 25))
    v.z = v.z * regf(SE_VPORT_ZSCALE) + regf(SE_VPORT_ZOFFSET);
  v.w = rhw;
}

/// A TCL vertex after clipping: the perspective divide and the viewport.
void CRadeonR100_3D::to_window(RadeonVertex &v) const {
  const float rhw = v.w != 0.0f ? 1.0f / v.w : 1.0f;
  float x = v.x * rhw, y = v.y * rhw, z = v.z * rhw;
  const u32 se = c.R(SE_CNTL);
  if (se & (1u << 24)) {
    x = x * regf(SE_VPORT_XSCALE) + regf(SE_VPORT_XOFFSET);
    y = y * regf(SE_VPORT_YSCALE) + regf(SE_VPORT_YOFFSET);
  }
  if (se & (1u << 25))
    z = z * regf(SE_VPORT_ZSCALE) + regf(SE_VPORT_ZOFFSET);
  v.x = x;
  v.y = y;
  v.z = z;
  v.w = rhw;
  v.clip_space = false;
}

namespace {
/// Clip a polygon in clip space against one plane (dot(p, plane) >= 0
/// inside), interpolating every attribute.
void clip_plane(std::vector<RadeonVertex> &poly, const float pl[4]) {
  std::vector<RadeonVertex> out;
  const size_t n = poly.size();
  auto dist = [&](const RadeonVertex &v) {
    return v.x * pl[0] + v.y * pl[1] + v.z * pl[2] + v.w * pl[3];
  };
  for (size_t i = 0; i < n; i++) {
    const RadeonVertex &a = poly[i], &b = poly[(i + 1) % n];
    const float da = dist(a), db = dist(b);
    if (da >= 0)
      out.push_back(a);
    if ((da >= 0) != (db >= 0)) {
      const float t = da / (da - db);
      RadeonVertex m;
      const float *pa = &a.x, *pb = &b.x;
      float *pm = &m.x;
      const size_t nf = offsetof(RadeonVertex, clip_space) / sizeof(float);
      for (size_t k = 0; k < nf; k++)
        pm[k] = pa[k] + (pb[k] - pa[k]) * t;
      m.clip_space = true;
      out.push_back(m);
    }
  }
  poly.swap(out);
}
} // namespace

/**
 * A triangle: clipped in clip space (TCL vertices: the view volume
 * -w <= x, y <= w, 0 <= z <= w -- Direct3D's depth range, which Mesa's
 * viewport transform (ZSCALE = ZOFFSET = 0.5 for the GL range) does not
 * reach [inference: the guard band and the near plane are not documented;
 * this clips exactly], then the user clip planes SE_TCL_UCP_VERT_BLEND_CTL
 * enables <7:2> (VS_UCP vectors 116..121, in clip space), then rasterised
 * as a fan.
 **/
void CRadeonR100_3D::triangle(const RadeonVertex &a, const RadeonVertex &b,
                              const RadeonVertex &c3,
                              const RadeonVertex &prov) {
  if (!a.clip_space) {
    const RadeonVertex *v[3] = {&a, &b, &c3};
    raster_triangle(v, &prov);
    return;
  }
  std::vector<RadeonVertex> poly = {a, b, c3};
  // flat shading takes the provoking vertex's colours to every new vertex
  RadeonVertex flat = prov;
  static const float planes[6][4] = {{1, 0, 0, 1}, {-1, 0, 0, 1},
                                     {0, 1, 0, 1}, {0, -1, 0, 1},
                                     {0, 0, 1, 0}, {0, 0, -1, 1}};
  for (int p = 0; p < 6 && !poly.empty(); p++)
    clip_plane(poly, planes[p]);
  const u32 ucp = c.R(SE_TCL_UCP_VERT_BLEND_CTL);
  for (int p = 0; p < 6 && !poly.empty(); p++)
    if (ucp & (1u << (2 + p)))
      clip_plane(poly, m_vec[116 + p]);
  if (poly.size() < 3)
    return;
  for (auto &v : poly)
    to_window(v);
  to_window(flat);
  // The TCL unit's own culling and two-sided lighting, by the winding as
  // the screen shows it (SE_TCL_UCP_VERT_BLEND_CTL CULL_FRONT_IS_CCW <28>,
  // CULL_FRONT <29>, CULL_BACK <30>; Mesa radeonCullFace/radeonFrontFace
  // program them with SE_CNTL's)
  const u32 tctl = c.R(SE_TCL_UCP_VERT_BLEND_CTL);
  double area = 0;
  for (size_t i = 1; i + 1 < poly.size() && area == 0; i++)
    area = double(poly[i].x - poly[0].x) * (poly[i + 1].y - poly[0].y) -
           double(poly[i + 1].x - poly[0].x) * (poly[i].y - poly[0].y);
  const bool front = (tctl & (1u << 28)) ? area < 0 : area > 0;
  if ((front && (tctl & (1u << 29))) || (!front && (tctl & (1u << 30))))
    return;
  if (!front && a.has_back) {
    for (auto &v : poly) {
      memcpy(v.col, v.col_back, sizeof(v.col));
      memcpy(v.spec, v.spec_back, sizeof(v.spec));
    }
    memcpy(flat.col, flat.col_back, sizeof(flat.col));
    memcpy(flat.spec, flat.spec_back, sizeof(flat.spec));
  }
  for (size_t i = 1; i + 1 < poly.size(); i++) {
    const RadeonVertex *v[3] = {&poly[0], &poly[i], &poly[i + 1]};
    raster_triangle(v, &flat);
  }
}

/**
 * Primitive assembly (the primitive types of SE_VF_CNTL<3:0>). Strips
 * alternate their winding so every triangle keeps the strip's; a
 * rectangle list draws each three vertices as the rectangle they span
 * (the fourth corner v0 + v2 - v1, as X.org's composite and Mesa's blit
 * use it); quads and polygons draw as fans.
 *
 * The provoking vertex for flat shading is SE_CNTL FLAT_SHADE_VTX <7:6>:
 * the primitive's vertex 0, 1 or 2 in the order the primitive was sent,
 * or 3 (VTX_LAST) its last. That the numbering is the primitive's own and
 * not the winding the strip's odd triangles are drawn in follows from
 * how the drivers use the field: Mesa's r100 driver (radeon_tcl.c
 * radeonTclPrimitive) sends GL strips and fans as the chip's strips and
 * fans with VTX_LAST -- GL's provoking vertex of strip triangle i is
 * v[i+2], odd or even -- and a GL polygon as a fan with VTX_0, GL's first
 * vertex; Direct3D, which the D3D code (VTX_PIX_CENTER_D3D) serves, flat
 * shades every strip triangle i with v[i]. A quad (and a rectangle) is one
 * primitive and one colour: its vertex 0..2, or with VTX_LAST its last
 * (GL's quad provoking vertex) [inference: Mesa never sends the chip
 * quads]. A line takes its first vertex with VTX_0, its second otherwise
 * [inference for VTX_2].
 **/
void CRadeonR100_3D::assemble(u32 prim, std::vector<RadeonVertex> &v) {
  const size_t n = v.size();
  const int flat_sel = int((c.R(SE_CNTL) >> 6) & 3);
  // the provoking vertex of a triangle whose vertices, in the order the
  // primitive sent them, are i0, i1, i2
  auto prov3 = [&](size_t i0, size_t i1, size_t i2) -> const RadeonVertex & {
    return flat_sel == 0 ? v[i0] : flat_sel == 1 ? v[i1] : v[i2];
  };
  // of a quad i0..i3
  auto prov4 = [&](size_t i0, size_t i1, size_t i2,
                   size_t i3) -> const RadeonVertex & {
    return flat_sel == 0   ? v[i0]
           : flat_sel == 1 ? v[i1]
           : flat_sel == 2 ? v[i2]
                           : v[i3];
  };
  auto clipped = [&](RadeonVertex &x) {
    if (x.clip_space) {
      RadeonVertex y = x;
      to_window(y);
      return y;
    }
    return x;
  };
  auto line = [&](RadeonVertex &x, RadeonVertex &y) {
    const RadeonVertex a = clipped(x), b = clipped(y);
    raster_line(a, b, flat_sel == 0 ? &a : &b);
  };
  m_prims++;
  switch (prim) {
  case PRIM_POINT_LIST:
    for (size_t i = 0; i < n; i++)
      if (!v[i].clip_space ||
          (std::fabs(v[i].x) <= v[i].w && std::fabs(v[i].y) <= v[i].w &&
           v[i].z >= 0 && v[i].z <= v[i].w))
        raster_point(clipped(v[i]));
    return;
  case PRIM_LINE_LIST:
    for (size_t i = 0; i + 1 < n; i += 2)
      line(v[i], v[i + 1]);
    return;
  case PRIM_LINE_STRIP:
  case PRIM_LINE_LOOP:
    line_stipple_count = 0;
    for (size_t i = 0; i + 1 < n; i++)
      line(v[i], v[i + 1]);
    if (prim == PRIM_LINE_LOOP && n > 2)
      line(v[n - 1], v[0]);
    return;
  case PRIM_TRI_LIST:
    for (size_t i = 0; i + 2 < n; i += 3)
      triangle(v[i], v[i + 1], v[i + 2], prov3(i, i + 1, i + 2));
    return;
  case PRIM_TRI_FAN:
  case PRIM_POLYGON:
    for (size_t i = 1; i + 1 < n; i++)
      triangle(v[0], v[i], v[i + 1], prov3(0, i, i + 1));
    return;
  case PRIM_TRI_STRIP:
    for (size_t i = 0; i + 2 < n; i++) {
      if (i & 1)
        triangle(v[i + 1], v[i], v[i + 2], prov3(i, i + 1, i + 2));
      else
        triangle(v[i], v[i + 1], v[i + 2], prov3(i, i + 1, i + 2));
    }
    return;
  case PRIM_RECT_LIST:
    for (size_t i = 0; i + 2 < n; i += 3) {
      RadeonVertex d = v[i];
      const float *p0 = &v[i].x, *p1 = &v[i + 1].x, *p2 = &v[i + 2].x;
      float *pd = &d.x;
      const size_t nf = offsetof(RadeonVertex, clip_space) / sizeof(float);
      for (size_t k = 0; k < nf; k++)
        pd[k] = p0[k] + p2[k] - p1[k];
      const RadeonVertex &pv = prov3(i, i + 1, i + 2);
      triangle(v[i], v[i + 1], v[i + 2], pv);
      triangle(v[i], v[i + 2], d, pv);
    }
    return;
  case PRIM_QUAD_LIST:
    for (size_t i = 0; i + 3 < n; i += 4) {
      const RadeonVertex &pv = prov4(i, i + 1, i + 2, i + 3);
      triangle(v[i], v[i + 1], v[i + 2], pv);
      triangle(v[i], v[i + 2], v[i + 3], pv);
    }
    return;
  case PRIM_QUAD_STRIP:
    // quad i is v[2i], v[2i+1], v[2i+3], v[2i+2] around; its vertices
    // count in the order sent, so VTX_LAST is v[2i+3] (GL's)
    for (size_t i = 0; i + 3 < n; i += 2) {
      const RadeonVertex &pv = prov4(i, i + 1, i + 2, i + 3);
      triangle(v[i], v[i + 1], v[i + 3], pv);
      triangle(v[i], v[i + 3], v[i + 2], pv);
    }
    return;
  case PRIM_TRI_FLAG:
    // TRI_TYPE_2 (radeon_reg.h RADEON_CP_VC_CNTL_PRIM_TYPE_TRI_TYPE_2):
    // drawn as a triangle list [inference: no driver uses it; Mesa's TCL
    // table leaves it unused]
    for (size_t i = 0; i + 2 < n; i += 3)
      triangle(v[i], v[i + 1], v[i + 2], prov3(i, i + 1, i + 2));
    return;
  case PRIM_POINT_LIST_3:
    // 3VRT_POINT_LIST and 3VRT_LINE_LIST: Mesa's radeon_sanity.c wants a
    // multiple of three vertices, as for a triangle list; that is all any
    // source says. Drawn as each triple's vertices as points, and its
    // three edges as lines [inference]
    for (size_t i = 0; i + 2 < n; i += 3)
      for (size_t k = 0; k < 3; k++)
        if (!v[i + k].clip_space ||
            (std::fabs(v[i + k].x) <= v[i + k].w &&
             std::fabs(v[i + k].y) <= v[i + k].w && v[i + k].z >= 0 &&
             v[i + k].z <= v[i + k].w))
          raster_point(clipped(v[i + k]));
    return;
  case PRIM_LINE_LIST_3:
    for (size_t i = 0; i + 2 < n; i += 3)
      for (size_t k = 0; k < 3; k++)
        line(v[i + k], v[i + (k + 1) % 3]);
    return;
  default:
    warn_once(8 + int(prim),
              "the sprite primitive (SPIRIT_LIST): not modelled");
    return;
  }
}

/**
 * 3D_CLEAR_ZMASK [START] [COUNT] [MASK], the fast Z clear. No ATI document
 * exists; the legacy DRM's clear (radeon_state.c radeon_cp_dispatch_clear,
 * "based on reverse engineering") is the source: for the R100 and RV200
 * (RADEON_HAS_HIERZ, R100 microcode) START is ((y / 8) * pitch + x) / 64
 * times 8 and COUNT the span from x / 64 to x2 / 64 inclusive in 16-pixel
 * units plus 4, for each band of 8 lines; RB3D_DEPTHCLEARVALUE holds the
 * value, the stencil value in <31:24>. Read that way, START counts 8x8
 * blocks along the band (pitch / 8 a band) and COUNT pairs of blocks
 * [inference]. The visible effect is what is modelled: the blocks' Z
 * (and stencil) read as the clear value afterwards -- written here into
 * the Z buffer, the compressed form being the chip's business. MASK
 * picks blocks not to clear by a pattern the DRM itself was unsure of;
 * the DRM's R100 clears pass 0 (all) or a pattern it sets with
 * hierarchical Z, which changes no pixel [inference]; it is ignored.
 **/
void CRadeonR100_3D::clear_zmask(u32 start, u32 count, u32 mask) {
  (void)mask;
  raster_setup();
  const u32 zf = rs.zfmt;
  const int zb = (zf == 0 || zf == 7) ? 2 : 4;
  const u32 pitch = rs.z_pitch; // pixels
  if (!pitch)
    return;
  const u32 blocks_per_band = pitch / 8;
  const u32 v = c.R(0x3230); // RB3D_DEPTHCLEARVALUE
  const u32 b0 = start;      // 8x8 blocks
  const u32 nblocks = count * 2;
  for (u32 b = b0; b < b0 + nblocks; b++) {
    const u32 band = b / blocks_per_band, col = b % blocks_per_band;
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++) {
        const int px = int(col * 8) + x, py = int(band * 8) + y;
        const u32 a =
            surface_addr(rs.z_base, pitch, u32(zb), rs.z_micro, true, px, py);
        surf_write(a, zb, zb == 2 ? (v & 0xffff) : v, rs.z_swap);
      }
  }
  m_pixels += u64(nblocks) * 64;
}

/**
 * The state file: the TCL vectors and scalars, the stipple and the
 * vertex array pointers, after the card's own block.
 **/
static constexpr u32 k3dMagic = 0x44335241; // 'AR3D'

void CRadeonR100_3D::save(FILE *f) const {
  fwrite(&k3dMagic, 4, 1, f);
  fwrite(m_vec, sizeof(m_vec), 1, f);
  fwrite(m_scl, sizeof(m_scl), 1, f);
  fwrite(m_stipple, sizeof(m_stipple), 1, f);
  fwrite(m_aos, sizeof(m_aos), 1, f);
  fwrite(&m_aos_count, 4, 1, f);
  u32 idx[3] = {m_vec_index, m_vec_comp, m_scl_index};
  fwrite(idx, sizeof(idx), 1, f);
}

/// Returns false (and leaves the file where it was) when the next block
/// is not this one's: state files written before the 3D engine existed.
bool CRadeonR100_3D::restore(FILE *f) {
  const long at = ftell(f);
  u32 m = 0;
  if (fread(&m, 4, 1, f) != 1 || m != k3dMagic) {
    fseek(f, at, SEEK_SET);
    return false;
  }
  u32 idx[3];
  if (fread(m_vec, sizeof(m_vec), 1, f) != 1 ||
      fread(m_scl, sizeof(m_scl), 1, f) != 1 ||
      fread(m_stipple, sizeof(m_stipple), 1, f) != 1 ||
      fread(m_aos, sizeof(m_aos), 1, f) != 1 ||
      fread(&m_aos_count, 4, 1, f) != 1 || fread(idx, sizeof(idx), 1, f) != 1)
    return false;
  m_vec_index = idx[0];
  m_vec_comp = idx[1];
  m_scl_index = idx[2];
  return true;
}

/**
 * The R100 generation (the R100, RV100, RV200, RS100 and RS200 parts, which
 * load the R100 microcode): the rendering engine's registers from 0x1400 to
 * 0x3fff go through the command FIFO -- the Rage 128 Pro guide's "GUI
 * registers (FIFOed)" (RRG 3-149), which the R100 kept --, and RBBM_STATUS
 * shows the 2D blocks busy in E2 <17> and RB2D <18>, the 3D blocks in
 * <24:19> (Linux r100d.h).
 **/
namespace radeon {
extern const Generation gen_r100; // the chip rows name it (RadeonChips.cpp)
const Generation gen_r100 = {
    "r100",
    0x1400,
    0x4000,
    (1u << 17) | (1u << 18),
    (1u << 19) | (1u << 20) | (1u << 21) | (1u << 22) | (1u << 23) | (1u << 24),
    [](const CRadeonEngineBus &bus) -> std::unique_ptr<CRadeonEngine3D> {
      return std::make_unique<r100::CRadeonR100_3D>(bus);
    }};
} // namespace radeon
