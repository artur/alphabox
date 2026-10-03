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

/* The Radeon R100/RV200 3D engine: vertex fetch, TCL, setup, rasteriser
 * and pixel pipeline.
 *
 * No ATI document describes the R100's 3D engine. What is here comes
 * from the open drivers that program it (lab/docs-radeon/INDEX.md):
 * Linux's radeon_reg.h, r100.c (the command-stream checker: which
 * registers and packets exist, the SE_VTX_FMT field order), Mesa's
 * classic r100 driver (radeon_state*.c: the meaning of each field as the
 * driver uses it; radeon_texstate.c: the combiner equations; radeon_tcl
 * and radeon_maos: the vertex layouts and TCL state memory) and X.org's
 * radeon_exa_render.c (Render through the 3D engine). Where a behaviour
 * is inferred rather than read off a driver, the code says so.
 *
 *   Radeon3D.cpp      register ports, packets, vertex fetch, primitive
 *                     assembly, the TCL-bypass setup, clipping
 *   RadeonTcl.cpp     transform, lighting, fog, texture coordinates
 *   RadeonRaster.cpp  triangles, lines, points; textures, combiners, fog,
 *                     alpha/stencil/Z tests, blending, the colour write
 *
 * The engine draws synchronously inside the register write or packet
 * that starts it, like the 2D engine.
 */

#if !defined(INCLUDED_RADEON3D_H)
#define INCLUDED_RADEON3D_H

#include <cstdint>
#include <cstdio>
#include <vector>

#include "datatypes.hpp"

class CRadeon;

namespace radeon3d {

// --- registers (radeon_reg.h names) -----------------------------------------
constexpr u32 PP_MISC = 0x1c14;
constexpr u32 PP_FOG_COLOR = 0x1c18;
constexpr u32 RE_SOLID_COLOR = 0x1c1c;
constexpr u32 RB3D_BLENDCNTL = 0x1c20;
constexpr u32 RB3D_DEPTHOFFSET = 0x1c24;
constexpr u32 RB3D_DEPTHPITCH = 0x1c28;
constexpr u32 RB3D_ZSTENCILCNTL = 0x1c2c;
constexpr u32 PP_CNTL = 0x1c38;
constexpr u32 RB3D_CNTL = 0x1c3c;
constexpr u32 RB3D_COLOROFFSET = 0x1c40;
constexpr u32 RE_WIDTH_HEIGHT = 0x1c44;
constexpr u32 RB3D_COLORPITCH = 0x1c48;
constexpr u32 SE_CNTL = 0x1c4c;
constexpr u32 SE_COORD_FMT = 0x1c50;
constexpr u32 PP_TXFILTER_0 = 0x1c54; ///< unit n at + 0x18 * n
constexpr u32 PP_TXFORMAT_0 = 0x1c58;
constexpr u32 PP_TXOFFSET_0 = 0x1c5c;
constexpr u32 PP_TXCBLEND_0 = 0x1c60;
constexpr u32 PP_TXABLEND_0 = 0x1c64;
constexpr u32 PP_TFACTOR_0 = 0x1c68;
constexpr u32 PP_UNIT_STRIDE = 0x18;
constexpr u32 RE_STIPPLE_ADDR = 0x1cc8;
constexpr u32 RE_STIPPLE_DATA = 0x1ccc;
constexpr u32 RE_LINE_PATTERN = 0x1cd0;
constexpr u32 RE_LINE_STATE = 0x1cd4;
constexpr u32 PP_TEX_SIZE_0 = 0x1d04; ///< unit n at + 8 * n
constexpr u32 PP_TEX_PITCH_0 = 0x1d08;
constexpr u32 PP_BORDER_COLOR_0 = 0x1d40; ///< unit n at + 4 * n
constexpr u32 RB3D_STENCILREFMASK = 0x1d7c;
constexpr u32 RB3D_ROPCNTL = 0x1d80;
constexpr u32 RB3D_PLANEMASK = 0x1d84;
constexpr u32 SE_VPORT_XSCALE = 0x1d98;
constexpr u32 SE_VPORT_XOFFSET = 0x1d9c;
constexpr u32 SE_VPORT_YSCALE = 0x1da0;
constexpr u32 SE_VPORT_YOFFSET = 0x1da4;
constexpr u32 SE_VPORT_ZSCALE = 0x1da8;
constexpr u32 SE_VPORT_ZOFFSET = 0x1dac;
constexpr u32 SE_ZBIAS_FACTOR = 0x1db0;
constexpr u32 SE_ZBIAS_CONSTANT = 0x1db4;
constexpr u32 SE_LINE_WIDTH = 0x1db8;
constexpr u32 SE_PORT_DATA0 = 0x2000; ///< ... 0x203c
constexpr u32 SE_PORT_DATA_LAST = 0x203c;
constexpr u32 SE_VTX_FMT = 0x2080;
constexpr u32 SE_VF_CNTL = 0x2084;
constexpr u32 SE_CNTL_STATUS = 0x2140;
constexpr u32 SE_TCL_VECTOR_INDX_REG = 0x2200;
constexpr u32 SE_TCL_VECTOR_DATA_REG = 0x2204;
constexpr u32 SE_TCL_SCALAR_INDX_REG = 0x2208;
constexpr u32 SE_TCL_SCALAR_DATA_REG = 0x220c;
constexpr u32 SE_TCL_MATERIAL_EMISSIVE = 0x2210; ///< RGBA floats
constexpr u32 SE_TCL_MATERIAL_AMBIENT = 0x2220;
constexpr u32 SE_TCL_MATERIAL_DIFFUSE = 0x2230;
constexpr u32 SE_TCL_MATERIAL_SPECULAR = 0x2240;
constexpr u32 SE_TCL_SHININESS = 0x2250;
constexpr u32 SE_TCL_OUTPUT_VTX_FMT = 0x2254;
constexpr u32 SE_TCL_OUTPUT_VTX_SEL = 0x2258;
constexpr u32 SE_TCL_MATRIX_SELECT_0 = 0x225c;
constexpr u32 SE_TCL_MATRIX_SELECT_1 = 0x2260;
constexpr u32 SE_TCL_UCP_VERT_BLEND_CTL = 0x2264;
constexpr u32 SE_TCL_TEXTURE_PROC_CTL = 0x2268;
constexpr u32 SE_TCL_LIGHT_MODEL_CTL = 0x226c;
constexpr u32 SE_TCL_PER_LIGHT_CTL_0 = 0x2270; ///< two lights a register
constexpr u32 SE_TCL_STATE_FLUSH = 0x2284;
constexpr u32 RE_TOP_LEFT = 0x26c0;
constexpr u32 RE_MISC = 0x26c4;

// --- vertex format (SE_VTX_FMT, the packets' VTX_FMT) ------------------------
constexpr u32 VTX_W0 = 1u << 0;
constexpr u32 VTX_FPCOLOR = 1u << 1;
constexpr u32 VTX_FPALPHA = 1u << 2;
constexpr u32 VTX_PKCOLOR = 1u << 3;
constexpr u32 VTX_FPSPEC = 1u << 4;
constexpr u32 VTX_FPFOG = 1u << 5;
constexpr u32 VTX_PKSPEC = 1u << 6;
constexpr u32 VTX_ST0 = 1u << 7;
constexpr u32 VTX_ST1 = 1u << 8;
constexpr u32 VTX_Q1 = 1u << 9;
constexpr u32 VTX_ST2 = 1u << 10;
constexpr u32 VTX_Q2 = 1u << 11;
constexpr u32 VTX_ST3 = 1u << 12;
constexpr u32 VTX_Q3 = 1u << 13;
constexpr u32 VTX_Q0 = 1u << 14;
constexpr u32 VTX_WEIGHT_SHIFT = 15; ///< <17:15> blend weights
constexpr u32 VTX_N0 = 1u << 18;
constexpr u32 VTX_XY1 = 1u << 27;
constexpr u32 VTX_Z1 = 1u << 28;
constexpr u32 VTX_W1 = 1u << 29;
constexpr u32 VTX_N1 = 1u << 30;
constexpr u32 VTX_Z = 1u << 31;

// --- vertex control (SE_VF_CNTL, the packets' VF_CNTL)
// ------------------------
enum : u32 {
  PRIM_NONE = 0,
  PRIM_POINT_LIST = 1,
  PRIM_LINE_LIST = 2,
  PRIM_LINE_STRIP = 3,
  PRIM_TRI_LIST = 4,
  PRIM_TRI_FAN = 5,
  PRIM_TRI_STRIP = 6,
  PRIM_TRI_FLAG = 7,
  PRIM_RECT_LIST = 8,
  PRIM_POINT_LIST_3 = 9,
  PRIM_LINE_LIST_3 = 10,
  PRIM_SPIRIT_LIST = 11,
  PRIM_LINE_LOOP = 12,
  PRIM_QUAD_LIST = 13,
  PRIM_QUAD_STRIP = 14,
  PRIM_POLYGON = 15
};
enum : u32 { WALK_STATE = 0, WALK_INDEX = 1, WALK_LIST = 2, WALK_DATA = 3 };
constexpr u32 VF_COLOR_ORDER_RGBA = 1u << 6;
constexpr u32 VF_INDEX_32 = 1u << 11;

} // namespace radeon3d

/// A vertex as the setup engine takes it. Before setup x/y/z/w are clip
/// coordinates (TCL) or what the vertex carried (bypass); after it, the
/// window position, the depth in [0, 1] and 1/w.
struct RadeonVertex {
  float x, y, z, w;
  float col[4];    ///< diffuse RGBA, 0..1
  float spec[4];   ///< specular RGB, fog factor in [3]
  float tex[4][4]; ///< texture coordinate sets: s, t, r, q
  bool clip_space; ///< x/y/z/w still clip coordinates
};

/// What a TCL input vertex carries, decoded from its dwords.
struct RadeonVertexIn {
  float pos[4];
  float norm[3];
  float col[4];
  float spec[4];
  float fog;
  float tex[4][4];
  bool has_w, has_z, has_norm, has_col, has_alpha, has_spec, has_fog;
  bool has_tex[4], has_q[4];
};

class CRadeon3D {
public:
  /// A fragment as the rasteriser hands it to the pixel pipeline.
  struct frag_t {
    int x, y;
    float z;
    float col[4], spec[4];
    float tex[3][4]; ///< s, t (divided by q, 0..1 across the image), lod
  };

  explicit CRadeon3D(CRadeon &card);

  /// A register write the 3D engine acts on (the vertex and TCL state
  /// ports, SE_VF_CNTL). Returns true when the write is consumed; plain
  /// state registers are left to the register file.
  bool reg_write(u32 reg, u32 data);
  bool reg_read(u32 reg, u32 *v);
  /// A type-3 packet the 3D engine owns. Returns false for any other.
  bool packet3(u8 op, const std::vector<u32> &d);
  void reset();
  void save(FILE *f) const;
  bool restore(FILE *f);

  /// Counters the self-test checks.
  u64 m_prims = 0, m_pixels = 0;

  // TCL state memory (public for the self-test's reference).
  float m_vec[128][4]; ///< VS_* vectors: matrices, lights, fog, ...
  float m_scl[64];     ///< SS_* scalars

private:
  CRadeon &c;
  u32 reg(u32 r) const;
  float regf(u32 r) const;
  u32 mem_read32(u32 mc);
  u8 mem_read8(u32 mc);

  // --- ports and packets (Radeon3D.cpp) -------------------------------------
  u32 m_vec_index = 0, m_vec_comp = 0;
  u32 m_scl_index = 0;
  u32 m_stipple[32] = {};
  struct aos_t {
    u32 comps, stride, addr;
  };
  aos_t m_aos[16] = {};
  u32 m_aos_count = 0;
  std::vector<u32> m_port; ///< SE_PORT_DATA dwords of a register-port draw
  u32 m_port_fmt = 0, m_port_cntl = 0, m_port_need = 0;
  bool m_port_active = false;
  bool m_warned[64] = {};
  void warn_once(int id, const char *what);

  int vertex_dwords(u32 fmt) const;
  void decode_vertex(u32 fmt, u32 cntl, const u32 *d, RadeonVertexIn &v) const;
  void fetch_aos(u32 index, std::vector<u32> &out);
  void draw(u32 fmt, u32 cntl, const std::vector<u32> &vdata,
            const std::vector<u32> *indices, u32 vbuf_mc, u32 vbuf_max);
  void process_vertex(u32 fmt, u32 cntl, const RadeonVertexIn &in,
                      RadeonVertex &out);
  bool tcl_enabled() const;
  void assemble(u32 prim, std::vector<RadeonVertex> &v);
  void triangle(const RadeonVertex &a, const RadeonVertex &b,
                const RadeonVertex &c, int flat_index);
  void to_window(RadeonVertex &v) const;
  void bypass_to_window(RadeonVertex &v) const;

  // --- TCL (RadeonTcl.cpp) ---------------------------------------------------
  void tcl_vertex(const RadeonVertexIn &in, RadeonVertex &out);
  const float *matrix(u32 sel) const { return &m_vec[(sel & 15) * 4][0]; }

  // --- raster and pixel pipeline (RadeonRaster.cpp) --------------------------
  struct texunit_t {
    bool enabled;
    u32 filter, format, offset, cblend, ablend, tfactor;
    int w, h, levels, pitch_bytes, bpp, fmt;
    bool npot, persp;
    int route;
    u32 level_off[12];
    int level_w[12], level_h[12], level_pitch[12];
  };
  struct raster_t {
    u32 pp_cntl, rb3d_cntl, se_cntl, zcntl, blend, misc;
    u32 color_base, color_pitch, color_fmt, color_bpp;
    u32 z_base, z_pitch, zfmt;
    int clip_l, clip_t, clip_r, clip_b; ///< inclusive
    texunit_t tex[3];
    u32 stencil_ref, stencil_mask, stencil_wmask;
    u32 planemask, rop;
    float fog_col[3];
  };
  raster_t rs;
  void raster_setup();
  void raster_triangle(const RadeonVertex *v[3], const RadeonVertex *prov);
  void raster_line(const RadeonVertex &a, const RadeonVertex &b);
  void raster_point(const RadeonVertex &a);
  void fragment(frag_t &f);
  void sample(int unit, float s, float t, float lod, float out[4]);
  void texel(const texunit_t &tu, int level, int x, int y, float out[4]);
  void fetch_texel(const texunit_t &tu, int level, int x, int y, float out[4]);
  u32 line_stipple_count = 0;
};

#endif // !defined(INCLUDED_RADEON3D_H)
