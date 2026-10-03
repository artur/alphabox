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
 * The Radeon's TCL unit: transform, clip coordinates, lighting, fog and
 * texture coordinates for one vertex. The fixed-function model is
 * OpenGL's, programmed as Mesa's classic r100 driver programs it
 * (radeon_state.c, radeon_state_init.c, radeon_tcl.c):
 *
 *   - state memory: 4-float vectors and scalars loaded through
 *     SE_TCL_VECTOR/SCALAR_INDX_REG and _DATA_REG (Radeon3D.cpp). A
 *     matrix is four vectors from 4 * its number, one row each (Mesa's
 *     upload_matrix writes column-major GL matrices transposed);
 *     SE_TCL_MATRIX_SELECT_0 picks the modelview <3:0> and the inverse
 *     transpose modelview <19:16>, _SELECT_1 the modelview-projection
 *     <3:0> and the texture matrices <19:16>, <23:20>, <27:24>;
 *   - per light i (0..7): ambient, diffuse, specular colours in vectors
 *     64+i, 72+i, 80+i; position or direction 88+i; for an infinite light
 *     the half vector, for a local one the negated spot direction, 96+i;
 *     attenuation (quadratic, linear, constant) 104+i; scalars: spot
 *     exponent 8+i, spot cutoff (as a cosine) 16+i, range cutoff 32+i,
 *     1/constant attenuation 40+i. SE_TCL_PER_LIGHT_CTL_n holds two
 *     lights' flags, 16 bits apart: enable, ambient, specular, local,
 *     spot, range attenuation, constant-only attenuation;
 *   - the eye vector 124 (for a non-local viewer, w the normal rescale
 *     factor), the global ambient 122, the fog parameters 123 (y the
 *     constant, z the factor), the user clip planes 116..121;
 *   - the material in the SE_TCL_MATERIAL_* registers and
 *     SE_TCL_SHININESS; SE_TCL_LIGHT_MODEL_CTL's source fields choose for
 *     emissive <17:16>, ambient <19:18>, diffuse <21:20>, specular
 *     <23:22> between the material register (0 premultiplied, 1 state)
 *     and the vertex's diffuse (2) or specular (3) colour. With emissive
 *     and ambient both premultiplied the global ambient vector already
 *     holds emissive + model ambient x material ambient (Mesa's
 *     update_global_ambient).
 * Outputs (SE_TCL_OUTPUT_VTX_SEL): the lit diffuse colour when lighting
 * <0> and COMPUTE_DIFFUSE <1> are on, else the vertex's; the lit
 * specular when COMPUTE_SPECULAR <2> and not DIFFUSE_SPECULAR_COMBINE
 * (which adds it to the diffuse instead); the fog factor in the
 * specular's alpha per SE_TCL_UCP_VERT_BLEND_CTL<9:8> (exp, exp2,
 * linear), with D negative as Mesa sets it: f = exp(D * c),
 * exp(D * c * c), or C + D * c, with c the eye distance
 * (|z| in eye space, or the range when RNG_BASED_FOG <10>); texture set n
 * from input set <19:16>+4n (0-3), or the texture matrix applied to the
 * texture-generation input TEXTURE_PROC_CTL selects (texture coordinate,
 * object or eye position, eye normal, reflection vector).
 * Vertex blending and two-sided lighting: see tcl_vertex (the first an
 * inference from the register names alone).
 * Not modelled: DUAL_CONE spots, the specular threshold (no source says
 * what either computes).
 **/

#include "Radeon.hpp"
#include "Radeon3D.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace radeon3d;

namespace {
inline float f32(u32 v) {
  float f;
  memcpy(&f, &v, 4);
  return f;
}
inline void mat_vec(const float *m, const float v[4], float out[4]) {
  for (int r = 0; r < 4; r++)
    out[r] = m[4 * r] * v[0] + m[4 * r + 1] * v[1] + m[4 * r + 2] * v[2] +
             m[4 * r + 3] * v[3];
}
inline float dot3(const float *a, const float *b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline void normalize3(float *v) {
  const float l = std::sqrt(dot3(v, v));
  if (l > 0) {
    v[0] /= l;
    v[1] /= l;
    v[2] /= l;
  }
}
inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// SE_TCL_LIGHT_MODEL_CTL
constexpr u32 LIGHTING_ENABLE = 1u << 0;
constexpr u32 LIGHT_IN_MODELSPACE = 1u << 1;
constexpr u32 LOCAL_VIEWER = 1u << 2;
constexpr u32 NORMALIZE_NORMALS = 1u << 3;
constexpr u32 RESCALE_NORMALS = 1u << 4;
constexpr u32 SPECULAR_LIGHTS = 1u << 5;
constexpr u32 DIFFUSE_SPECULAR_COMBINE = 1u << 6;
// SE_TCL_OUTPUT_VTX_SEL
constexpr u32 COMPUTE_DIFFUSE = 1u << 1;
constexpr u32 COMPUTE_SPECULAR = 1u << 2;
// per light (shifted by 16 for the odd light)
constexpr u32 LIGHT_ENABLE = 1u << 0;
constexpr u32 LIGHT_ENABLE_AMBIENT = 1u << 1;
constexpr u32 LIGHT_ENABLE_SPECULAR = 1u << 2;
constexpr u32 LIGHT_IS_LOCAL = 1u << 3;
constexpr u32 LIGHT_IS_SPOT = 1u << 4;
constexpr u32 LIGHT_RANGE_ATTEN = 1u << 6;
constexpr u32 LIGHT_CONST_ATTEN = 1u << 7;
} // namespace

void CRadeon3D::tcl_vertex(const RadeonVertexIn &in, RadeonVertex &out) {
  const u32 sel0 = c.R(SE_TCL_MATRIX_SELECT_0);
  const u32 sel1 = c.R(SE_TCL_MATRIX_SELECT_1);
  const u32 lm = c.R(SE_TCL_LIGHT_MODEL_CTL);
  const u32 vsel = c.R(SE_TCL_OUTPUT_VTX_SEL);
  const u32 ucp = c.R(SE_TCL_UCP_VERT_BLEND_CTL);
  const u32 tpc = c.R(SE_TCL_TEXTURE_PROC_CTL);

  float pos[4] = {in.pos[0], in.pos[1], in.has_z ? in.pos[2] : 0.0f,
                  in.has_w ? in.pos[3] : 1.0f};
  if (ucp & (1u << 31)) // FORCE_W_TO_ONE
    pos[3] = 1.0f;

  // Vertex blending (SE_TCL_UCP_VERT_BLEND_CTL: BLEND_OP_COUNT <14:12>,
  // POSITION_BLEND_OP_ENABLE <16>, NORMAL_BLEND_OP_ENABLE <17>,
  // VERTEX_BLEND_WGT_MINUS_ONE <22>; radeon_reg.h): the position is the
  // weighted sum of the MODELPROJECT_i and MODELVIEW_i transforms
  // (SE_TCL_MATRIX_SELECT_1 <4i+3:4i>, _SELECT_0 <4i+3:4i>), the normal
  // of the IT_MODELVIEW_i ones, for i up to the count, with the vertex's
  // blend weights (SE_VTX_FMT <17:15>) and, with WGT_MINUS_ONE, the last
  // weight one less the others [inference: no driver programs vertex
  // blending on the R100; the field names are all there is].
  const u32 nmat = (ucp & (3u << 16)) ? ((ucp >> 12) & 7) + 1 : 1;
  float wgt[4] = {1, 0, 0, 0};
  if (nmat > 1) {
    float sum = 0;
    for (u32 i = 0; i < nmat && i < 4; i++) {
      wgt[i] = i < u32(in.nweights) ? in.weight[i] : 0.0f;
      if (i + 1 < nmat)
        sum += wgt[i];
    }
    if (ucp & (1u << 22))
      wgt[std::min(nmat, 4u) - 1] = 1.0f - sum;
  }
  auto blended = [&](u32 sel, bool on, const float v[4], float o[4]) {
    if (!on || nmat == 1) {
      mat_vec(matrix(sel & 15), v, o);
      return;
    }
    o[0] = o[1] = o[2] = o[3] = 0;
    for (u32 i = 0; i < nmat && i < 4; i++) {
      float t[4];
      mat_vec(matrix((sel >> (4 * i)) & 15), v, t);
      for (int k = 0; k < 4; k++)
        o[k] += wgt[i] * t[k];
    }
  };
  const bool pos_blend = (ucp & (1u << 16)) != 0;
  const bool nrm_blend = (ucp & (1u << 17)) != 0;

  float clip[4];
  blended(sel1, pos_blend, pos, clip);
  out.x = clip[0];
  out.y = clip[1];
  out.z = clip[2];
  out.w = clip[3];
  out.clip_space = true;
  out.has_back = false;

  float eye[4];
  blended(sel0, pos_blend, pos, eye);
  if (eye[3] != 0.0f && eye[3] != 1.0f)
    for (int k = 0; k < 3; k++)
      eye[k] /= eye[3];

  // The normal, in eye space (or model space).
  float n[3] = {in.norm[0], in.norm[1], in.norm[2]};
  const bool modelspace = (lm & LIGHT_IN_MODELSPACE) != 0;
  if (!modelspace) {
    const float n4[4] = {n[0], n[1], n[2], 0};
    float t[4];
    if (nrm_blend && nmat > 1) {
      blended(sel0 >> 16, true, n4, t);
    } else {
      const float *it = matrix((sel0 >> 16) & 15);
      for (int r = 0; r < 3; r++)
        t[r] = dot3(&it[4 * r], n4);
    }
    n[0] = t[0];
    n[1] = t[1];
    n[2] = t[2];
  }
  if (lm & RESCALE_NORMALS)
    for (float &k : n)
      k *= m_vec[124][3];
  if (lm & NORMALIZE_NORMALS)
    normalize3(n);
  const float *P = modelspace ? pos : eye;

  // Colours.
  memcpy(out.col, in.col, sizeof(out.col));
  memcpy(out.spec, in.spec, sizeof(out.spec));
  out.spec[3] = in.has_fog ? in.fog : 1.0f;
  if ((lm & LIGHTING_ENABLE) && (vsel & (COMPUTE_DIFFUSE | COMPUTE_SPECULAR))) {
    tcl_light(in, P, n, out.col, out.spec);
    // Two-sided lighting (SE_TCL_UCP_VERT_BLEND_CTL LIGHT_TWOSIDE <11>):
    // the colours a back-facing triangle takes, lit with the normal
    // reversed and the same material -- Mesa's r100 driver leaves two-sided
    // lighting to the hardware only when the front and back materials are
    // equal (check_twoside_fallback); the triangle's facing picks the set
    // (Radeon3D.cpp) [inference: how the chip carries the second set is
    // not documented].
    if (ucp & (1u << 11)) {
      memcpy(out.col_back, in.col, sizeof(out.col_back));
      memcpy(out.spec_back, out.spec, sizeof(out.spec_back));
      const float nb[3] = {-n[0], -n[1], -n[2]};
      tcl_light(in, P, nb, out.col_back, out.spec_back);
      out.has_back = true;
    }
  }

  // Fog: the factor into the specular alpha.
  const u32 fogmode = (ucp >> 8) & 3;
  if (fogmode) {
    float cdist = std::fabs(eye[2]);
    if (ucp & (1u << 10))
      cdist = std::sqrt(dot3(eye, eye));
    const float C = m_vec[123][1], D = m_vec[123][2];
    float f = 1.0f;
    switch (fogmode) {
    case 1:
      f = std::exp(D * cdist);
      break;
    case 2:
      f = std::exp(D * cdist * cdist);
      break;
    case 3:
      f = C + D * cdist;
      break;
    }
    out.spec[3] = clamp01(f);
  }
  out.spec_back[3] = out.spec[3];

  // Texture coordinates.
  for (int t = 0; t < 4; t++) {
    const u32 src = (vsel >> (16 + 4 * t)) & 15;
    float tc[4] = {0, 0, 0, 1};
    if (src < 4) {
      memcpy(tc, in.tex[src], sizeof(tc));
      if (!in.has_q[src])
        tc[3] = 1.0f;
    } else if (src >= 8 && src < 12) {
      const int u = int(src - 8);
      // the texture-generation input
      float g[4] = {0, 0, 0, 1};
      const u32 gin = (tpc >> (16 + 4 * u)) & 15;
      switch (gin) {
      case 0:
      case 1:
      case 2:
      case 3:
        memcpy(g, in.tex[gin], sizeof(g));
        if (!in.has_q[gin])
          g[3] = 1.0f;
        g[2] = in.has_q[gin] ? g[2] : 0.0f;
        break;
      case 4: // object position
        memcpy(g, pos, sizeof(g));
        break;
      case 5: // eye position
        memcpy(g, eye, 3 * sizeof(float));
        break;
      case 6: // eye normal
        memcpy(g, n, 3 * sizeof(float));
        break;
      case 7:   // reflection vector
      case 8: { // normalized eye position
        float u3[3] = {eye[0], eye[1], eye[2]};
        normalize3(u3);
        if (gin == 8) {
          memcpy(g, u3, sizeof(u3));
        } else {
          const float dn = 2 * dot3(n, u3);
          for (int k = 0; k < 3; k++)
            g[k] = u3[k] - n[k] * dn;
        }
        break;
      }
      default:
        break;
      }
      if (tpc & (1u << u) || tpc & (1u << (4 + u)))
        mat_vec(matrix((sel1 >> (16 + 4 * u)) & 15), g, tc);
      else
        memcpy(tc, g, sizeof(tc));
    }
    memcpy(out.tex[t], tc, sizeof(tc));
  }
}

/**
 * Lighting: the lit diffuse colour into `col` and the specular into
 * `spec` (rgb) as SE_TCL_OUTPUT_VTX_SEL asks, for the normal `n` at the
 * position `P` (eye or model space).
 **/
void CRadeon3D::tcl_light(const RadeonVertexIn &in, const float *P,
                          const float n[3], float col[4], float spec[4]) {
  const u32 lm = c.R(SE_TCL_LIGHT_MODEL_CTL);
  const u32 vsel = c.R(SE_TCL_OUTPUT_VTX_SEL);
  auto source = [&](int shift, u32 reg, float o[4]) {
    switch ((lm >> shift) & 3) {
    case 2:
      memcpy(o, in.col, 4 * sizeof(float));
      return;
    case 3:
      memcpy(o, in.spec, 4 * sizeof(float));
      o[3] = 1.0f;
      return;
    default:
      for (int k = 0; k < 4; k++)
        o[k] = f32(c.R(reg + 4 * u32(k)));
    }
  };
  float me[4], ma[4], md[4], ms[4];
  source(16, SE_TCL_MATERIAL_EMISSIVE, me);
  source(18, SE_TCL_MATERIAL_AMBIENT, ma);
  source(20, SE_TCL_MATERIAL_DIFFUSE, md);
  source(22, SE_TCL_MATERIAL_SPECULAR, ms);
  const float shin = f32(c.R(SE_TCL_SHININESS));
  float dif[3], spc[3] = {0, 0, 0};
  const float *glt = m_vec[122];
  if (((lm >> 16) & 3) == 0 && ((lm >> 18) & 3) == 0) {
    for (int k = 0; k < 3; k++)
      dif[k] = glt[k];
  } else {
    for (int k = 0; k < 3; k++)
      dif[k] = me[k] + glt[k] * ma[k];
  }
  // the view direction
  float V[3] = {0, 0, 1};
  if (lm & LOCAL_VIEWER) {
    V[0] = -P[0];
    V[1] = -P[1];
    V[2] = -P[2];
    normalize3(V);
  } else {
    V[0] = m_vec[124][0];
    V[1] = m_vec[124][1];
    V[2] = m_vec[124][2];
  }
  for (int l = 0; l < 8; l++) {
    const u32 flags =
        c.R(SE_TCL_PER_LIGHT_CTL_0 + 4 * u32(l / 2)) >> (16 * (l & 1));
    if (!(flags & LIGHT_ENABLE))
      continue;
    const float *amb = m_vec[64 + l], *diff = m_vec[72 + l],
                *spe = m_vec[80 + l], *dirpos = m_vec[88 + l],
                *hv = m_vec[96 + l], *att = m_vec[104 + l];
    float L[3], H[3];
    float atten = 1.0f;
    if (flags & LIGHT_IS_LOCAL) {
      for (int k = 0; k < 3; k++)
        L[k] = dirpos[k] - P[k];
      const float d = std::sqrt(dot3(L, L));
      if (d > m_scl[32 + l] && m_scl[32 + l] > 0)
        continue; // beyond the range cutoff
      normalize3(L);
      if (flags & LIGHT_RANGE_ATTEN) {
        if (flags & LIGHT_CONST_ATTEN)
          atten = m_scl[40 + l];
        else {
          const float den = att[2] + att[1] * d + att[0] * d * d;
          atten = den > 0 ? 1.0f / den : 1.0f;
        }
      }
      if (flags & LIGHT_IS_SPOT) {
        const float cs = dot3(L, hv);
        if (cs < m_scl[16 + l])
          atten = 0;
        else
          atten *= std::pow(std::max(cs, 0.0f), m_scl[8 + l]);
      }
      for (int k = 0; k < 3; k++)
        H[k] = L[k] + V[k];
      normalize3(H);
    } else {
      for (int k = 0; k < 3; k++) {
        L[k] = dirpos[k];
        H[k] = hv[k];
      }
      if (lm & LOCAL_VIEWER) {
        for (int k = 0; k < 3; k++)
          H[k] = L[k] + V[k];
        normalize3(H);
      }
    }
    if (atten <= 0)
      continue;
    const float ndl = dot3(n, L);
    for (int k = 0; k < 3; k++) {
      float t = 0;
      if (flags & LIGHT_ENABLE_AMBIENT)
        t += amb[k] * ma[k];
      if (ndl > 0)
        t += ndl * diff[k] * md[k];
      dif[k] += atten * t;
    }
    if ((flags & LIGHT_ENABLE_SPECULAR) && (lm & SPECULAR_LIGHTS) && ndl > 0) {
      const float ndh = dot3(n, H);
      if (ndh > 0) {
        const float f = shin > 0 ? std::pow(ndh, shin) : 1.0f;
        for (int k = 0; k < 3; k++)
          spc[k] += atten * f * spe[k] * ms[k];
      }
    }
  }
  if (lm & DIFFUSE_SPECULAR_COMBINE)
    for (int k = 0; k < 3; k++) {
      dif[k] += spc[k];
      spc[k] = 0;
    }
  if (vsel & COMPUTE_DIFFUSE) {
    for (int k = 0; k < 3; k++)
      col[k] = clamp01(dif[k]);
    col[3] = clamp01(md[3]);
  }
  if (vsel & COMPUTE_SPECULAR)
    for (int k = 0; k < 3; k++)
      spec[k] = clamp01(spc[k]);
}
