/*=============================================================================
  GLN2D.cpp : native 2D drawing (stage "2d"): Draw2dImage (menus, HUD
  images, video frames) and font text.

  Positions arrive in the engine's 2D pixel space (origin top-left, y down,
  width x height = the render size) and are turned into clip space in the
  vertex shader, so no matrix stack is involved. Blend, depth and cull state
  were already set by the engine (EF_SetState / SetCullMode) and are left
  alone. The texture to sample is the one the engine bound on unit 0.
=============================================================================*/
#include "GLNativeCore.h"
#include "GLN2D.h"

namespace
{
const char* kVS =
  "#version 300 es\n"
  "in vec4 aPos; in vec4 aColor; in vec2 aTex0;\n"
  "uniform vec3 uScale;   // 2/width, 2/height, depth scale\n"
  "out mediump vec4 vColor; out highp vec2 vTex;\n"
  "void main() {\n"
  "  gl_Position = vec4(aPos.x * uScale.x - 1.0, 1.0 - aPos.y * uScale.y, aPos.z * uScale.z, 1.0);\n"
  "  vColor = aColor; vTex = aTex0;\n"
  "}\n";

// Alpha test: uAlphaTest.x = mode (0 off, 1 >ref, 2 <ref, 3 >=ref), .y = ref.
const char* kFSImage =
  "#version 300 es\n"
  "precision mediump float;\n"
  "in mediump vec4 vColor; in highp vec2 vTex;\n"
  "uniform sampler2D uTex; uniform float uTextured; uniform vec2 uAlphaTest;\n"
  "out vec4 oColor;\n"
  "void main() {\n"
  "  vec4 c = vColor;\n"
  "  if (uTextured > 0.5) c *= texture(uTex, vTex);\n"
  "  if (uAlphaTest.x > 0.5) {\n"
  "    bool pass = uAlphaTest.x < 1.5 ? c.a > uAlphaTest.y : (uAlphaTest.x < 2.5 ? c.a < uAlphaTest.y : c.a >= uAlphaTest.y);\n"
  "    if (!pass) discard;\n"
  "  }\n"
  "  oColor = c;\n"
  "}\n";

// Fonts: colour from the vertex, alpha = texture alpha x vertex alpha,
// alpha test > 0 (FontSetRenderingState).
const char* kFSFont =
  "#version 300 es\n"
  "precision mediump float;\n"
  "in mediump vec4 vColor; in highp vec2 vTex;\n"
  "uniform sampler2D uTex;\n"
  "out vec4 oColor;\n"
  "void main() {\n"
  "  float a = texture(uTex, vTex).a * vColor.a;\n"
  "  if (a <= 0.0) discard;\n"
  "  oColor = vec4(vColor.rgb, a);\n"
  "}\n";

GLN::Program s_image, s_font;
unsigned s_quads = 0, s_fontBatches = 0, s_fontVerts = 0;
bool s_built = false, s_ok = false;

bool Ready()
{
  if (!s_built)
  {
    s_built = true;
    s_ok = s_image.Build("2d-image", kVS, kFSImage) && s_font.Build("2d-font", kVS, kFSFont);
    if (s_ok)
    {
      s_image.Use(); GLN::gl.glUniform1i(s_image.Uniform("uTex"), 0);
      s_font.Use(); GLN::gl.glUniform1i(s_font.Uniform("uTex"), 0);
    }
    GLN::Log("GLNative: 2D programs %s", s_ok ? "ready" : "FAILED (2D stays on the translation layer)");
  }
  return s_ok;
}

struct V { float x, y, z; uint8_t c[4]; float s, t; };

void Draw(GLN::Program& p, const V* v, int nVerts, const uint16_t* idx, int nIdx, float w, float h, float zScale)
{
  GLN::StreamAlloc va = GLN::AllocVertices((size_t)nVerts * sizeof(V));
  memcpy(va.cpu, v, (size_t)nVerts * sizeof(V));
  GLN::StreamAlloc ia = { nullptr, 0, 0 };
  if (idx) { ia = GLN::AllocIndices((size_t)nIdx * 2); memcpy(ia.cpu, idx, (size_t)nIdx * 2); }
  GLN::CommitStreams();
  GLN::Layout l; l.stride = sizeof(V);
  l.Add(GLN_POS, 3, GL_FLOAT, false, 0);
  l.Add(GLN_COLOR, 4, GL_UNSIGNED_BYTE, true, 12);
  l.Add(GLN_TEX0, 2, GL_FLOAT, false, 16);
  p.Use();
  const float scale[3] = { 2.0f / w, 2.0f / h, zScale };
  GLN::gl.glUniform3fv(p.Uniform("uScale"), 1, scale);
  GLN::BindVertexArray(l, va.buffer, va.offset, idx ? ia.buffer : 0);
  GLN::gl.glActiveTexture(GL_TEXTURE0);
  if (idx) GLN::gl.glDrawElements(GL_TRIANGLES, nIdx, GL_UNSIGNED_SHORT, (const void*)ia.offset);
  else GLN::gl.glDrawArrays(GL_TRIANGLES, 0, nVerts);
}
} // namespace

bool GLN2D_DrawQuad(const float pos[4][3], const float st[4][2], const float rgba[4], bool textured,
                    int alphaTestMode, float alphaRef, float width, float height, float zScale)
{
  if (!GLN::Enabled(GLN_STAGE_2D) || !Ready()) return false;
  GLNSection section;
  V v[4];
  uint8_t c[4];
  for (int i = 0; i < 4; ++i) { float x = rgba[i] < 0 ? 0 : (rgba[i] > 1 ? 1 : rgba[i]); c[i] = (uint8_t)(x * 255.0f + 0.5f); }
  for (int i = 0; i < 4; ++i)
  {
    v[i].x = pos[i][0]; v[i].y = pos[i][1]; v[i].z = pos[i][2];
    memcpy(v[i].c, c, 4); v[i].s = st[i][0]; v[i].t = st[i][1];
  }
  static const uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
  s_image.Use();
  GLN::gl.glUniform1f(s_image.Uniform("uTextured"), textured ? 1.0f : 0.0f);
  const float at[2] = { (float)alphaTestMode, alphaRef };
  GLN::gl.glUniform2fv(s_image.Uniform("uAlphaTest"), 1, at);
  Draw(s_image, v, 4, idx, 6, width, height, zScale);
  ++s_quads;
  return true;
}

bool GLN2D_DrawFontTriangles(const void* verts, int nVerts, float width, float height)
{
  if (!GLN::Enabled(GLN_STAGE_2D) || !Ready() || nVerts <= 0) return false;
  // The engine's font vertices (struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F) have
  // the same 24-byte layout as V: xyz, rgba bytes, st.
  static_assert(sizeof(V) == 24, "font vertex layout");
  GLNSection section;
  Draw(s_font, (const V*)verts, nVerts, nullptr, 0, width, height, -1.0f);
  ++s_fontBatches; s_fontVerts += (unsigned)nVerts;
  return true;
}

void GLN2D_FrameStats()
{
  static unsigned frames = 0;
  if (++frames % 60 == 0 && (s_quads || s_fontBatches))
    GLN::Log("GLNative 2D: last 60 frames %u image quads, %u font batches (%u glyph vertices)", s_quads, s_fontBatches, s_fontVerts);
  if (frames % 60 == 0) s_quads = s_fontBatches = s_fontVerts = 0;
}
