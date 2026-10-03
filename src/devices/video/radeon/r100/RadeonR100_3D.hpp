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
 *   RadeonR100_3D.cpp     register ports, packets, vertex fetch,
 *                         primitive assembly, the TCL-bypass setup,
 *                         clipping
 *   RadeonR100Tcl.cpp     transform, lighting, fog, texture coordinates
 *   RadeonR100Raster.cpp  triangles, lines, points; textures, combiners,
 *                         fog, alpha/stencil/Z tests, blending, the
 *                         colour write
 *
 *   RadeonR100Regs.hpp    the registers and the packets' fields
 *
 * The engine is the R100 generation's CRadeonEngine3D (RadeonEngine3D.hpp,
 * which has its threading rule): it runs on the card's engine thread
 * (RadeonQueue.cpp), and a draw happens when the engine takes the
 * register write or packet that starts it. It reaches the card through
 * CRadeonEngineBus alone: the register file, VRAM, the memory
 * controller's translation for bus-master fetches and the part's row.
 */

#if !defined(INCLUDED_RADEON_R100_3D_H)
#define INCLUDED_RADEON_R100_3D_H

#include <cstdint>
#include <cstdio>
#include <vector>

#include "RadeonEngine3D.hpp"
#include "RadeonR100Regs.hpp"
#include "datatypes.hpp"

namespace radeon {
namespace r100 {

/// A vertex as the setup engine takes it. Before setup x/y/z/w are clip
/// coordinates (TCL) or what the vertex carried (bypass); after it, the
/// window position, the depth in [0, 1] and 1/w.
struct RadeonVertex {
  float x, y, z, w;
  float col[4];    ///< diffuse RGBA, 0..1
  float spec[4];   ///< specular RGB, fog factor in [3]
  float tex[4][4]; ///< texture coordinate sets: s, t, r, q
  /// two-sided lighting: the colours a back-facing triangle takes
  float col_back[4], spec_back[4];
  bool clip_space; ///< x/y/z/w still clip coordinates
  bool has_back;   ///< col_back/spec_back are set
};

/// What a TCL input vertex carries, decoded from its dwords.
struct RadeonVertexIn {
  float pos[4];
  float norm[3];
  float col[4];
  float spec[4];
  float fog;
  float tex[4][4];
  float weight[4]; ///< vertex blend weights
  int nweights;
  bool has_w, has_z, has_norm, has_col, has_alpha, has_spec, has_fog;
  bool has_tex[4], has_q[4];
};

class CRadeonR100_3D : public CRadeonEngine3D {
public:
  /// A fragment as the rasteriser hands it to the pixel pipeline.
  struct frag_t {
    int x, y;
    float z;
    float col[4], spec[4];
    /// s, t (divided by q, 0..1 across the image), lod, r (cube maps)
    float tex[3][4];
    /// the share of the pixel the primitive covers (anti-aliasing)
    float cov;
    /// the eye-space w (W depth formats)
    float w;
  };

  explicit CRadeonR100_3D(const CRadeonEngineBus &bus);

  /// A register write the 3D engine acts on (the vertex and TCL state
  /// ports, SE_VF_CNTL). Returns true when the write is consumed; plain
  /// state registers are left to the register file.
  bool reg_write(u32 reg, u32 data) override;
  bool reg_read(u32 reg, u32 *v) override;
  /// A type-3 packet the 3D engine owns. Returns false for any other.
  bool packet3(u8 op, const std::vector<u32> &d) override;
  void reset() override;
  void save(FILE *f) const override;
  bool restore(FILE *f) override;
  u64 pixels() const override { return m_pixels; }

  /// Counters the self-test checks.
  u64 m_prims = 0, m_pixels = 0;
  /// The self-test's switch: take the R200 microcode's packets on this
  /// part (to check them).
  bool m_r200_packets = false;

  // TCL state memory (public for the self-test's reference).
  float m_vec[128][4]; ///< VS_* vectors: matrices, lights, fog, ...
  float m_scl[64];     ///< SS_* scalars

private:
  CRadeonEngineBus c;
  u32 reg(u32 r) const;
  float regf(u32 r) const;
  u32 mem_read32(u32 mc);
  u8 mem_read8(u32 mc);

  // --- ports and packets (RadeonR100_3D.cpp) ---------------------------------
  u32 m_vec_index = 0, m_vec_comp = 0;
  u32 m_scl_index = 0;
  u32 m_stipple[32] = {};
  struct aos_t {
    u32 comps, stride, addr;
  };
  aos_t m_aos[16] = {};
  u32 m_aos_count = 0;
  /// a 3D_DRAW_INDX(_2) without indices, waiting for INDX_BUFFER
  bool m_indx_pending = false;
  u32 m_indx_fmt = 0, m_indx_cntl = 0;
  /// the fog table (RADEON_FOG_TABLE_INDEX/DATA)
  u8 m_fog_table[256] = {};
  u32 m_fog_index = 0;

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
  /// 3D_CLEAR_ZMASK: the fast Z clear (HyperZ)
  void clear_zmask(u32 start, u32 count, u32 mask);
  void to_window(RadeonVertex &v) const;
  void bypass_to_window(RadeonVertex &v) const;

  // --- TCL (RadeonR100Tcl.cpp) -----------------------------------------------
  void tcl_vertex(const RadeonVertexIn &in, RadeonVertex &out);
  /// The lit colours for normal `n` at eye position `P` (two-sided
  /// lighting calls it again with the normal negated).
  void tcl_light(const RadeonVertexIn &in, const float *P, const float n[3],
                 float col[4], float spec[4]);
  const float *matrix(u32 sel) const { return &m_vec[(sel & 15) * 4][0]; }

  // --- raster and pixel pipeline (RadeonR100Raster.cpp) ----------------------
  struct texunit_t {
    bool enabled;
    u32 filter, format, offset, cblend, ablend, tfactor;
    int w, h, levels, pitch_bytes, bpp, fmt;
    bool npot, persp;
    int route;
    u32 level_off[12];
    int level_w[12], level_h[12], level_pitch[12];
    u32 swap;   ///< the endian swap of its memory (0 none, 1-3)
    bool micro; ///< micro-tiled (TXO_MICRO_TILE_X2)
    bool cube;  ///< a cube map: six faces
    u32 face_off[6];
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
    u32 color_swap, z_swap;    ///< COLOR_ENDIAN, DEPTH_ENDIAN
    bool color_micro, z_micro; ///< micro-tiled colour and depth buffers
    float zbias_factor, zbias_const;
    /// the dither's horizontal error, per channel, and the line it is on
    float dither_err[3];
    int dither_y;
  };
  raster_t rs;
  void raster_setup();
  void raster_triangle(const RadeonVertex *v[3], const RadeonVertex *prov);
  void raster_line(const RadeonVertex &a, const RadeonVertex &b);
  void raster_point(const RadeonVertex &a);
  void fragment(frag_t &f);
  /// The byte address of pixel (x, y) of a surface: linear, or micro-tiled
  /// (32-byte tiles, Mesa radeon_tile.c).
  u32 surface_addr(u32 base, u32 pitch_px, u32 bpp, bool micro, bool depth,
                   int x, int y) const;
  u32 surf_read(u32 addr, int bytes, u32 swap) const;
  void surf_write(u32 addr, int bytes, u32 data, u32 swap);
  /// A colour quantised to `bits` with the dither or rounding RB3D_CNTL
  /// asks for.
  u32 quantise(float v, int bits, int ch, int x, int y);
  u32 pack_color(const float c[4], int x, int y);
  /// The cube map face and face coordinates of (s, t, r).
  static int cube_face(float s, float t, float r, float *fs, float *ft);
  void sample(int unit, float s, float t, float lod, float out[4]);
  void texel(const texunit_t &tu, int level, int x, int y, float out[4]);
  void fetch_texel(const texunit_t &tu, int level, int x, int y, float out[4]);
  u32 line_stipple_count = 0;
  int m_cube_face = 0; ///< the face a cube map unit is being sampled on
};

} // namespace r100
} // namespace radeon

#endif // !defined(INCLUDED_RADEON_R100_3D_H)
