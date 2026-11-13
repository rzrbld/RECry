/*=============================================================================
  GLRCore.cpp : native backend core: current state, frame recording,
  playback through one GL state cache, frame ring, static buffers,
  matrix stacks, immediate-mode builder.

  Frame ring (per-frame vertex/index data), FARCRY_GLR_RING:
  - upload (default, renfs5): data is written to CPU arrays while recording;
    each playback segment uploads exactly what it uses with one glBufferData
    into the next of three buffers, then draws.
  - map (d3es): each segment maps the next of three buffers (fence-checked,
    unsynchronised) and writes into it directly; unmapped before playback.
  Either way the GPU never reads a buffer that is being written, and data is
  uploaded once per segment (normally one segment per frame).
=============================================================================*/
#include "GLRCore.h"
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unordered_set>
#include "GLTexUpload.h"
#include "../../port/platform/PortTiming.h"

namespace GLR
{
Funcs gl;
static const char* s_debugLabel = "";   // engine shader of the draws being recorded (diagnostics)

void Log(const char* fmt, ...)
{
  char b[1024];
  va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
  fprintf(stderr, "%s\n", b);
}

bool Active()
{
  static int s = -1;
  if (s < 0) { const char* r = getenv("FARCRY_RENDERER"); s = (r && !strcmp(r, "native")) ? 1 : 0; }
  return s == 1;
}

namespace
{
bool s_ready = false;

// ---------------------------------------------------------------- current state (CPU)
struct Current
{
  Raster r;
  bool alphaTestOn = false; uint16_t alphaFunc = GL_ALWAYS; float alphaRef = 0;
  bool lighting = false, colorMaterial = false, normalize = false;
  uint8_t lightOn = 0, clipOn = 0, texGenOn[4] = {};   // bit per light / clip plane / texgen coordinate (s t r q)
  uint16_t genMode[4][4];                               // per stage and coordinate (GL enum)
  struct Light { float amb[4], diff[4], pos[4], att[3]; } light[8];
  float matAmb[4] = { 0.2f, 0.2f, 0.2f, 1 }, matDiff[4] = { 0.8f, 0.8f, 0.8f, 1 }, matEmis[4] = { 0, 0, 0, 1 };
  float sceneAmb[4] = { 0.2f, 0.2f, 0.2f, 1 };
  float curNormal[3] = { 0, 0, 1 };
  bool fogOn = false; uint16_t fogMode = 0x0800 /*GL_EXP*/; float fogStart = 0, fogEnd = 1, fogDensity = 1, fogColor[3] = { 0, 0, 0 };
  float clearColor[4] = { 0, 0, 0, 0 }, clearDepth = 1; int clearStencil = 0;
  TexBind tex[4] = {};          // what each stage samples (enabled target + its name)
  unsigned bound2D[4] = {}, boundCube[4] = {};
  bool on2D[4] = {}, onCube[4] = {};
  int selected = 0;
  uint8_t co[4] = {}, ao[4] = {}, ca[4] = {}, aa[4] = {};
  bool vpOn = false, fpOn = false;    // GL_VERTEX/FRAGMENT_PROGRAM_ARB
  DrawUniforms u;
} s_cur;
CgParams s_cgEnv;                     // program env registers (current)
bool s_cgDirty = true;                // changed since the last snapshot
std::vector<CgParams> s_cgSnaps;      // this segment's snapshots

void DefaultRaster(Raster& r)
{
  memset(&r, 0, sizeof(r));
  r.blendSrc = GL_ONE; r.blendDst = GL_ZERO;
  r.depthWrite = 1; r.depthFunc = GL_LESS;
  r.cullFace = GL_BACK; r.frontFace = GL_CCW;
  r.colorMask = 0xF;
  r.depthRange[1] = 1.0f;
  r.viewport[2] = r.viewport[3] = -1;   // set by the engine before drawing
}

// ---------------------------------------------------------------- layouts
std::vector<Layout> s_layouts;

// ---------------------------------------------------------------- recording
std::vector<Cmd> s_cmds;
std::vector<DrawUniforms> s_uniforms;
std::unordered_set<unsigned> s_pendingTex, s_pendingBuf;   // used by recorded draws

// ---------------------------------------------------------------- frame ring
const int kSlots = 3;
struct Ring
{
  GLenum target = 0;
  GLuint buf[kSlots] = {};
  GLsync fence[kSlots] = {};
  int slot = 0;
  size_t cap = 0;            // map mode: capacity of each buffer
  std::vector<uint8_t> cpu;  // upload mode: this segment's data
  uint8_t* mapped = nullptr; // map mode
  size_t used = 0;
};
Ring s_vring, s_iring;
bool s_mapMode = false;

void RingInit(Ring& r, GLenum target, size_t cap)
{
  r.target = target; r.cap = cap;
  gl.glGenBuffers(kSlots, r.buf);
  for (int i = 0; i < kSlots; ++i)
  {
    gl.glBindBuffer(target, r.buf[i]);
    gl.glBufferData(target, (GLsizeiptr)(s_mapMode ? cap : 4096), nullptr, GL_STREAM_DRAW);
  }
  r.cpu.reserve(cap);
}
void RingBeginSegment(Ring& r)
{
  r.used = 0;
  if (!s_mapMode) { r.cpu.clear(); return; }
  r.slot = (r.slot + 1) % kSlots;
  if (r.fence[r.slot])
  {
    // Normally signalled long ago (three segments back); wait if not.
    gl.glClientWaitSync(r.fence[r.slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    gl.glDeleteSync(r.fence[r.slot]); r.fence[r.slot] = 0;
  }
  gl.glBindBuffer(r.target, r.buf[r.slot]);
  r.mapped = (uint8_t*)gl.glMapBufferRange(r.target, 0, (GLsizeiptr)r.cap, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
}
// Make this segment's data visible to the GPU; returns the buffer to draw from.
GLuint RingEndSegment(Ring& r)
{
  if (s_mapMode)
  {
    gl.glBindBuffer(r.target, r.buf[r.slot]);
    if (r.mapped) gl.glUnmapBuffer(r.target);
    r.mapped = nullptr;
    return r.buf[r.slot];
  }
  r.slot = (r.slot + 1) % kSlots;
  gl.glBindBuffer(r.target, r.buf[r.slot]);
  if (r.used) gl.glBufferData(r.target, (GLsizeiptr)r.used, r.cpu.data(), GL_STREAM_DRAW);
  return r.buf[r.slot];
}
void RingFence(Ring& r)
{
  if (!s_mapMode) return;
  if (r.fence[r.slot]) gl.glDeleteSync(r.fence[r.slot]);
  r.fence[r.slot] = gl.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}
bool RingAllocRaw(Ring& r, size_t bytes, RingAlloc& out)
{
  const size_t off = (r.used + 15) & ~(size_t)15;
  if (s_mapMode)
  {
    if (!r.mapped || off + bytes > r.cap) return false;
    out = RingAlloc{ r.mapped + off, r.buf[r.slot], off };
  }
  else
  {
    if (r.cpu.size() < off + bytes) r.cpu.resize(off + bytes);
    out = RingAlloc{ r.cpu.data() + off, 0, off };
  }
  r.used = off + bytes;
  return true;
}

// ---------------------------------------------------------------- GL state cache
struct GLState
{
  bool rasterValid = false;
  Raster r;
  GLuint prog = 0;
  int unit = -1;
  GLuint tex2D[4] = {}, texCube[4] = {};
  GLuint arrayBuf = 0, elemBuf = 0;
  struct A { bool on; GLuint buf; uint8_t size, type, norm; uint16_t stride; size_t ptr; } attr[ATTR_COUNT];
} s_gl;

// Forget everything cached; vertex attributes are put into a known state
// (disabled, pointer unknown) since enable state cannot be "unknown".
void Invalidate()
{
  s_gl = GLState();
  for (int i = 0; i < ATTR_COUNT; ++i)
  {
    GLState::A& a = s_gl.attr[i];
    memset(&a, 0xFF, sizeof(a)); a.on = false;
    gl.glDisableVertexAttribArray(i);
  }
}

inline void Cap(GLenum cap, bool on) { if (on) gl.glEnable(cap); else gl.glDisable(cap); }

// d3es GL_State: apply only what differs from the cache.
void ApplyRaster(const Raster& n)
{
  Raster& c = s_gl.r;
  const bool all = !s_gl.rasterValid;
  if (all || n.blend != c.blend) Cap(GL_BLEND, n.blend);
  if (n.blend && (all || n.blendSrc != c.blendSrc || n.blendDst != c.blendDst)) gl.glBlendFunc(n.blendSrc, n.blendDst);
  if (all || n.depthTest != c.depthTest) Cap(GL_DEPTH_TEST, n.depthTest);
  if (all || n.depthWrite != c.depthWrite) gl.glDepthMask(n.depthWrite ? GL_TRUE : GL_FALSE);
  if (all || n.depthFunc != c.depthFunc) gl.glDepthFunc(n.depthFunc);
  if (all || n.colorMask != c.colorMask) gl.glColorMask(n.colorMask & 1, (n.colorMask >> 1) & 1, (n.colorMask >> 2) & 1, (n.colorMask >> 3) & 1);
  if (all || n.stencilTest != c.stencilTest) Cap(GL_STENCIL_TEST, n.stencilTest);
  if (all || n.cullOn != c.cullOn) Cap(GL_CULL_FACE, n.cullOn);
  if (all || n.cullFace != c.cullFace) gl.glCullFace(n.cullFace);
  if (all || n.frontFace != c.frontFace) gl.glFrontFace(n.frontFace);
  if (all || n.scissorOn != c.scissorOn) Cap(GL_SCISSOR_TEST, n.scissorOn);
  if (n.scissorOn && (all || memcmp(n.scissor, c.scissor, sizeof(n.scissor)))) gl.glScissor(n.scissor[0], n.scissor[1], n.scissor[2], n.scissor[3]);
  if (n.viewport[2] >= 0 && (all || memcmp(n.viewport, c.viewport, sizeof(n.viewport)))) gl.glViewport(n.viewport[0], n.viewport[1], n.viewport[2], n.viewport[3]);
  if (all || n.depthRange[0] != c.depthRange[0] || n.depthRange[1] != c.depthRange[1]) gl.glDepthRangef(n.depthRange[0], n.depthRange[1]);
  if (all || n.polyOn != c.polyOn) Cap(GL_POLYGON_OFFSET_FILL, n.polyOn);
  if (n.polyOn && (all || n.poly[0] != c.poly[0] || n.poly[1] != c.poly[1])) gl.glPolygonOffset(n.poly[0], n.poly[1]);
  c = n; s_gl.rasterValid = true;
}
} // namespace

// Shared with the texture and buffer code: keep the cache in step with binds
// done outside playback.
void StateBindTexture(int u, GLenum target, GLuint name)
{
  if (s_gl.unit != u) { gl.glActiveTexture(GL_TEXTURE0 + u); s_gl.unit = u; }
  GLuint& cur = target == GL_TEXTURE_CUBE_MAP ? s_gl.texCube[u] : s_gl.tex2D[u];
  if (cur != name) { gl.glBindTexture(target, name); cur = name; }
}
void StateBindBuffer(GLenum target, GLuint b)
{
  GLuint& cur = target == GL_ELEMENT_ARRAY_BUFFER ? s_gl.elemBuf : s_gl.arrayBuf;
  if (cur != b) { gl.glBindBuffer(target, b); cur = b; }
}
static void StateForgetBuffer(GLuint b)
{
  if (s_gl.arrayBuf == b) s_gl.arrayBuf = 0xFFFFFFFFu;
  if (s_gl.elemBuf == b) s_gl.elemBuf = 0xFFFFFFFFu;
  for (auto& a : s_gl.attr) if (a.buf == b) a.buf = 0xFFFFFFFFu;
}
static void StateForgetTexture(GLuint t)
{
  for (int u = 0; u < 4; ++u) { if (s_gl.tex2D[u] == t) s_gl.tex2D[u] = 0xFFFFFFFFu; if (s_gl.texCube[u] == t) s_gl.texCube[u] = 0xFFFFFFFFu; }
}

// ---------------------------------------------------------------- matrices
namespace
{
struct Mat { float m[16]; };
const Mat kIdentity = { { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 } };
std::vector<Mat> s_stack[2 + 4] = { { kIdentity }, { kIdentity }, { kIdentity }, { kIdentity }, { kIdentity }, { kIdentity } };
uint8_t s_mode = MAT_MODELVIEW;
std::vector<Mat>& Stack() { return s_stack[s_mode < MAT_TEXTURE ? s_mode : 2 + s_cur.selected]; }
void MulInto(float* out, const float* a, const float* b)   // out = a * b (column-major)
{
  float r[16];
  for (int c = 0; c < 4; ++c)
    for (int rr = 0; rr < 4; ++rr)
      r[c * 4 + rr] = a[rr] * b[c * 4] + a[4 + rr] * b[c * 4 + 1] + a[8 + rr] * b[c * 4 + 2] + a[12 + rr] * b[c * 4 + 3];
  memcpy(out, r, 64);
}
} // namespace
void MatrixModeSet(MatrixMode m) { s_mode = m; }
void LoadIdentity() { Stack().back() = kIdentity; }
void LoadMatrix(const float m[16]) { memcpy(Stack().back().m, m, 64); }
void MultMatrix(const float m[16]) { float* t = Stack().back().m; MulInto(t, t, m); }
void PushMatrix() { std::vector<Mat>& s = Stack(); if (s.size() < 64) s.push_back(s.back()); }
void PopMatrix() { std::vector<Mat>& s = Stack(); if (s.size() > 1) s.pop_back(); }
void Ortho(double l, double r, double b, double t, double n, double f)
{
  const float m[16] = { (float)(2 / (r - l)), 0, 0, 0,  0, (float)(2 / (t - b)), 0, 0,  0, 0, (float)(-2 / (f - n)), 0,
                        (float)(-(r + l) / (r - l)), (float)(-(t + b) / (t - b)), (float)(-(f + n) / (f - n)), 1 };
  MultMatrix(m);
}
void Translate(float x, float y, float z) { const float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1 }; MultMatrix(m); }
void Scale(float x, float y, float z) { const float m[16] = { x,0,0,0, 0,y,0,0, 0,0,z,0, 0,0,0,1 }; MultMatrix(m); }
void Rotate(float deg, float x, float y, float z)
{
  const float len = sqrtf(x * x + y * y + z * z); if (len <= 0) return;
  x /= len; y /= len; z /= len;
  const float a = deg * 3.14159265358979f / 180.0f, c = cosf(a), s = sinf(a), t = 1 - c;
  const float m[16] = { t*x*x + c, t*x*y + s*z, t*x*z - s*y, 0,  t*x*y - s*z, t*y*y + c, t*y*z + s*x, 0,
                        t*x*z + s*y, t*y*z - s*x, t*z*z + c, 0,  0, 0, 0, 1 };
  MultMatrix(m);
}
const float* ModelView() { return s_stack[0].back().m; }
const float* Projection() { return s_stack[1].back().m; }
const float* TextureMatrix(int stage) { return s_stack[2 + (stage & 3)].back().m; }

// ---------------------------------------------------------------- raster state setters
bool Enable(unsigned cap, bool on)
{
  Raster& r = s_cur.r;
  switch (cap)
  {
    case GL_BLEND: r.blend = on; return true;
    case GL_DEPTH_TEST: r.depthTest = on; return true;
    case GL_STENCIL_TEST: r.stencilTest = on; return true;
    case GL_CULL_FACE: r.cullOn = on; return true;
    case GL_SCISSOR_TEST: r.scissorOn = on; return true;
    case GL_POLYGON_OFFSET_FILL: r.polyOn = on; return true;
    case 0x0BC0 /*GL_ALPHA_TEST*/: s_cur.alphaTestOn = on; return true;
    case 0x0B60 /*GL_FOG*/: s_cur.fogOn = on; return true;
    // Fixed-function lighting and texgen: state for the mesh passes.
    case 0x0B50 /*GL_LIGHTING*/: s_cur.lighting = on; return true;
    case 0x0B57 /*GL_COLOR_MATERIAL*/: s_cur.colorMaterial = on; return true;
    case 0x0BA1 /*GL_NORMALIZE*/: s_cur.normalize = on; return true;
    case 0x0C60: case 0x0C61: case 0x0C62: case 0x0C63:   // GL_TEXTURE_GEN_S..Q (selected stage)
    {
      const uint8_t bit = (uint8_t)(1 << (cap - 0x0C60)); uint8_t& g = s_cur.texGenOn[s_cur.selected];
      g = on ? (uint8_t)(g | bit) : (uint8_t)(g & ~bit); return true;
    }
    // Desktop-only switches with no effect on the native path: dithering,
    // smoothing, line/point offsets, the (inactive on this tier) vertex /
    // NV register-combiner switches.
    case 0x0BD0 /*GL_DITHER*/: case 0x0B10 /*GL_POINT_SMOOTH*/: case 0x0B20 /*GL_LINE_SMOOTH*/: case 0x0B41 /*GL_POLYGON_SMOOTH*/:
    case 0x2A02 /*GL_POLYGON_OFFSET_LINE*/: case 0x2A01 /*GL_POLYGON_OFFSET_POINT*/:
    case 0x8620 /*GL_VERTEX_PROGRAM_ARB*/: s_cur.vpOn = on; return true;
    case 0x8804 /*GL_FRAGMENT_PROGRAM_ARB*/: s_cur.fpOn = on; return true;
    case 0x8522 /*GL_REGISTER_COMBINERS_NV*/:
    case 0x86DE /*GL_TEXTURE_SHADER_NV*/: case 0x8642 /*GL_VERTEX_PROGRAM_POINT_SIZE*/:
      return true;
  }
  if (cap >= 0x4000 && cap < 0x4008) { const uint8_t bit = (uint8_t)(1 << (cap - 0x4000)); s_cur.lightOn = on ? (uint8_t)(s_cur.lightOn | bit) : (uint8_t)(s_cur.lightOn & ~bit); return true; }   // GL_LIGHTi
  if (cap >= 0x3000 && cap < 0x3006) { const uint8_t bit = (uint8_t)(1 << (cap - 0x3000)); s_cur.clipOn = on ? (uint8_t)(s_cur.clipOn | bit) : (uint8_t)(s_cur.clipOn & ~bit); return true; }    // GL_CLIP_PLANEi
  return false;
}
void BlendFunc(unsigned src, unsigned dst) { s_cur.r.blendSrc = (uint16_t)src; s_cur.r.blendDst = (uint16_t)dst; }
void DepthMask(bool on) { s_cur.r.depthWrite = on; }
void DepthFunc(unsigned f) { s_cur.r.depthFunc = (uint16_t)f; }
void ColorMask(bool r, bool g, bool b, bool a) { s_cur.r.colorMask = (uint8_t)((r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0)); }
void AlphaFunc(unsigned f, float ref) { s_cur.alphaFunc = (uint16_t)f; s_cur.alphaRef = ref; }
void CullFace(unsigned m) { s_cur.r.cullFace = (uint16_t)m; }
void FrontFace(unsigned m) { s_cur.r.frontFace = (uint16_t)m; }
void PolygonOffset(float factor, float units) { s_cur.r.poly[0] = factor; s_cur.r.poly[1] = units; }
void Viewport(int x, int y, int w, int h) { int* v = s_cur.r.viewport; v[0] = x; v[1] = y; v[2] = w; v[3] = h; }
void Scissor(int x, int y, int w, int h) { int* v = s_cur.r.scissor; v[0] = x; v[1] = y; v[2] = w; v[3] = h; }
void DepthRange(float n, float f) { s_cur.r.depthRange[0] = n; s_cur.r.depthRange[1] = f; }
void ClearColor(float r, float g, float b, float a) { s_cur.clearColor[0] = r; s_cur.clearColor[1] = g; s_cur.clearColor[2] = b; s_cur.clearColor[3] = a; }
void ClearDepth(float d) { s_cur.clearDepth = d; }
void ClearStencil(int s) { s_cur.clearStencil = s; }
void Clear(unsigned mask)
{
  if (!s_ready) return;
  Cmd c; memset(&c, 0, sizeof(c));
  c.type = CMD_CLEAR; c.r = s_cur.r; c.clearMask = mask;
  memcpy(c.clearColor, s_cur.clearColor, 16); c.clearDepth = s_cur.clearDepth; c.clearStencil = s_cur.clearStencil;
  s_cmds.push_back(c);
}
// Engine GS_* bits -> raster state (as CRenderer::EF_SetState applies them).
void SetState(uint32_t st)
{
  static const uint16_t src[] = { GL_ONE, GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE };
  static const uint16_t dst[] = { GL_ZERO, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA };
  Raster& r = s_cur.r;
  const unsigned s = st & 0xf, d = (st & 0xf0) >> 4;
  r.blend = s && d && s <= 9 && d <= 8;
  if (r.blend) { r.blendSrc = src[s]; r.blendDst = dst[d]; }
  r.depthWrite = (st & 0x100) != 0;
  r.depthTest = !(st & 0x20000);
  r.depthFunc = (st & 0x100000) ? GL_EQUAL : ((st & 0x200000) ? GL_GREATER : GL_LEQUAL);
  r.colorMask = (st & 0x400) ? 0 : ((st & 0x10000) ? 8 : ((st & 0x40000) ? 7 : 0xF));
  r.stencilTest = (st & 0x400000) != 0;
  switch (st & 0xf0000000u)
  {
    case 0x10000000u: s_cur.alphaTestOn = true; s_cur.alphaFunc = GL_GREATER; s_cur.alphaRef = 0.0f; break;
    case 0x20000000u: s_cur.alphaTestOn = true; s_cur.alphaFunc = GL_LESS; s_cur.alphaRef = 0.5f; break;
    case 0x40000000u: s_cur.alphaTestOn = true; s_cur.alphaFunc = GL_GEQUAL; s_cur.alphaRef = 0.5f; break;
    case 0x80000000u: s_cur.alphaTestOn = true; s_cur.alphaFunc = GL_GEQUAL; s_cur.alphaRef = 0.25f; break;
    default: s_cur.alphaTestOn = false; break;
  }
}

// ---------------------------------------------------------------- textures
// Stage u's sampled texture: the cube map when enabled, else the 2D texture.
static void UpdateStage(int u)
{
  TexBind& b = s_cur.tex[u];
  if (s_cur.onCube[u]) { b.target = TEX_CUBE; b.name = s_cur.boundCube[u]; }
  else if (s_cur.on2D[u]) { b.target = TEX_2D; b.name = s_cur.bound2D[u]; }
  else { b.target = TEX_NONE; b.name = 0; }
}
void SelectTexture(int u) { if (u >= 0 && u < 4) s_cur.selected = u; }
int SelectedTexture() { return s_cur.selected; }
void BindTexture(TexTarget t, unsigned name)
{
  const int u = s_cur.selected;
  if (t == TEX_CUBE) s_cur.boundCube[u] = name; else s_cur.bound2D[u] = name;
  UpdateStage(u);
}
unsigned BoundTexture(TexTarget t) { const int u = s_cur.selected; return t == TEX_CUBE ? s_cur.boundCube[u] : s_cur.bound2D[u]; }
void EnableTexture(TexTarget t, bool on)
{
  const int u = s_cur.selected;
  if (t == TEX_CUBE) s_cur.onCube[u] = on; else s_cur.on2D[u] = on;
  UpdateStage(u);
}
void SetTexture(int u, TexTarget t, unsigned name)
{
  if (u < 0 || u >= 4) return;
  const int sel = s_cur.selected; s_cur.selected = u;
  if (t != TEX_NONE) BindTexture(t, name);
  s_cur.on2D[u] = t == TEX_2D; s_cur.onCube[u] = t == TEX_CUBE; UpdateStage(u);
  s_cur.selected = sel;
}
void TextureWillChange(unsigned name) { if (s_pendingTex.count(name)) Flush(); }
void TextureDeleted(unsigned name)
{
  if (s_pendingTex.count(name)) Flush();
  StateForgetTexture(name);
  for (int u = 0; u < 4; ++u)
  {
    if (s_cur.bound2D[u] == name) s_cur.bound2D[u] = 0;
    if (s_cur.boundCube[u] == name) s_cur.boundCube[u] = 0;
    UpdateStage(u);
  }
}
void SetStageOps(int u, uint8_t co, uint8_t ao, uint8_t ca, uint8_t aa)
{ if (u >= 0 && u < 4) { s_cur.co[u] = co; s_cur.ao[u] = ao; s_cur.ca[u] = ca; s_cur.aa[u] = aa; } }
void SetStageScale(int u, float rgb, float a) { if (u >= 0 && u < 4) { s_cur.u.stageScale[u][0] = rgb; s_cur.u.stageScale[u][1] = a; } }
void SetStageColor(int u, const float c[4]) { if (u >= 0 && u < 4) memcpy(s_cur.u.stageColor[u], c, 16); }

// ---------------------------------------------------------------- fog, current attributes
void Fogi(unsigned pname, int v) { if (pname == 0x0B65 /*GL_FOG_MODE*/) s_cur.fogMode = (uint16_t)v; else Fogf(pname, (float)v); }
void Fogf(unsigned pname, float v)
{
  switch (pname)
  {
    case 0x0B65: s_cur.fogMode = (uint16_t)v; break;        // GL_FOG_MODE
    case 0x0B63: s_cur.fogStart = v; break;                 // GL_FOG_START
    case 0x0B64: s_cur.fogEnd = v; break;                   // GL_FOG_END
    case 0x0B62: s_cur.fogDensity = v; break;               // GL_FOG_DENSITY
  }
}
void Fogfv(unsigned pname, const float* v)
{
  if (pname == 0x0B66) memcpy(s_cur.fogColor, v, 12);       // GL_FOG_COLOR
  else Fogf(pname, v[0]);
}
void ProgramEnvParameter(unsigned target, unsigned index, const float v[4])
{
  float* d;
  if (target == 0x8620 /*GL_VERTEX_PROGRAM_ARB*/) { if (index >= (unsigned)kCgVRegs) return; d = s_cgEnv.v[index]; }
  else { if (index >= (unsigned)kCgFRegs) return; d = s_cgEnv.f[index]; }
  if (memcmp(d, v, 16)) { memcpy(d, v, 16); s_cgDirty = true; }
}
void SetColor(const float c[4]) { memcpy(s_cur.u.curColor, c, 16); }

// ---------------------------------------------------------------- lighting, texgen
namespace
{
// out = in transformed as a row vector by m (column-major): out_j = sum_i in_i * m[j*4+i]
void PlaneTimesMatrix(const float* in, const float* m, float* out)
{ for (int j = 0; j < 4; ++j) out[j] = in[0] * m[j * 4] + in[1] * m[j * 4 + 1] + in[2] * m[j * 4 + 2] + in[3] * m[j * 4 + 3]; }
void MatTimesVec(const float* m, const float* v, float* out)
{ for (int i = 0; i < 4; ++i) out[i] = m[i] * v[0] + m[4 + i] * v[1] + m[8 + i] * v[2] + m[12 + i] * v[3]; }
bool Invert4(const float* m, float* inv)
{
  float t[16];
  t[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
  t[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
  t[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
  t[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
  t[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
  t[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
  t[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
  t[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
  t[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
  t[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
  t[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
  t[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
  t[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
  t[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
  t[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
  t[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
  const float det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
  if (det == 0) return false;
  for (int i = 0; i < 16; ++i) inv[i] = t[i] / det;
  return true;
}
} // namespace
void TexGeni(unsigned coord, unsigned pname, int v)
{
  const unsigned c = coord - 0x2000; if (c > 3) return;            // GL_S..GL_Q
  if (pname == 0x2500 /*GL_TEXTURE_GEN_MODE*/) s_cur.genMode[s_cur.selected][c] = (uint16_t)v;
}
void TexGenfv(unsigned coord, unsigned pname, const float* v)
{
  const unsigned c = coord - 0x2000; if (c > 3) return;
  const int u = s_cur.selected;
  if (pname == 0x2500) { s_cur.genMode[u][c] = (uint16_t)v[0]; return; }
  if (pname == 0x2501 /*GL_OBJECT_PLANE*/) memcpy(&s_cur.u.genObj[u][c * 4], v, 16);
  else if (pname == 0x2502 /*GL_EYE_PLANE*/)
  {
    float inv[16];
    if (Invert4(ModelView(), inv)) PlaneTimesMatrix(v, inv, &s_cur.u.genEye[u][c * 4]);   // GL: p * inverse(MV) at specification
    else memcpy(&s_cur.u.genEye[u][c * 4], v, 16);
  }
}
void Lightfv(unsigned light, unsigned pname, const float* v)
{
  const unsigned i = light - 0x4000; if (i > 7) return;            // GL_LIGHT0..7
  Current::Light& L = s_cur.light[i];
  switch (pname)
  {
    case 0x1200: memcpy(L.amb, v, 16); break;                        // GL_AMBIENT
    case 0x1201: memcpy(L.diff, v, 16); break;                       // GL_DIFFUSE
    case 0x1203: MatTimesVec(ModelView(), v, L.pos); break;          // GL_POSITION: eye space at specification
    case 0x1207: L.att[0] = v[0]; break; case 0x1208: L.att[1] = v[0]; break; case 0x1209: L.att[2] = v[0]; break;
  }
}
void Lightf(unsigned light, unsigned pname, float v) { const float a[4] = { v, 0, 0, 0 }; Lightfv(light, pname, a); }
void Materialfv(unsigned face, unsigned pname, const float* v)
{
  (void)face;   // front and back are the same here (no two-sided lighting)
  switch (pname)
  {
    case 0x1200: memcpy(s_cur.matAmb, v, 16); break;                                  // GL_AMBIENT
    case 0x1201: memcpy(s_cur.matDiff, v, 16); break;                                 // GL_DIFFUSE
    case 0x1602: memcpy(s_cur.matAmb, v, 16); memcpy(s_cur.matDiff, v, 16); break;    // GL_AMBIENT_AND_DIFFUSE
    case 0x1600: memcpy(s_cur.matEmis, v, 16); break;                                 // GL_EMISSION
  }
}
void LightModelfv(unsigned pname, const float* v) { if (pname == 0x0B53 /*GL_LIGHT_MODEL_AMBIENT*/) memcpy(s_cur.sceneAmb, v, 16); }
void ColorMaterial(unsigned face, unsigned mode) { (void)face; (void)mode; }   // the engine only uses ambient-and-diffuse
void SetTexCoord(int u, float s, float t, float r, float q)
{ if (u >= 0 && u < 4) { float* d = s_cur.u.curTex[u]; d[0] = s; d[1] = t; d[2] = r; d[3] = q; } }
void SetNormal(float x, float y, float z) { s_cur.curNormal[0] = x; s_cur.curNormal[1] = y; s_cur.curNormal[2] = z; }

// ---------------------------------------------------------------- lifetime
static void SegvTrace(int sig)
{
  void* bt[48]; const int n = backtrace(bt, 48);
  const char m[] = "PORT crash backtrace:\n"; if (write(2, m, sizeof(m) - 1)) {}
  backtrace_symbols_fd(bt, n, 2);
  signal(sig, SIG_DFL); raise(sig);
}
bool Init()
{
  if (getenv("FARCRY_SEGV_TRACE")) { signal(SIGSEGV, SegvTrace); signal(SIGBUS, SegvTrace); signal(SIGABRT, SegvTrace); }
  if (s_ready) return true;
  bool ok = true;
#define GLR_LOAD(n) gl.n = (decltype(gl.n))SDL_GL_GetProcAddress(#n); if (!gl.n) { Log("GLR: missing driver entry point %s", #n); ok = false; }
  GLR_FUNCS(GLR_LOAD)
#undef GLR_LOAD
  if (!ok) return false;
  const char* ring = getenv("FARCRY_GLR_RING");
  s_mapMode = ring && !strcmp(ring, "map");
  RingInit(s_vring, GL_ARRAY_BUFFER, (size_t)8 << 20);
  RingInit(s_iring, GL_ELEMENT_ARRAY_BUFFER, (size_t)2 << 20);
  RingBeginSegment(s_vring); RingBeginSegment(s_iring);
  Invalidate();
  // GL's initial state: identity matrices, stage scale 1, white colour,
  // texcoords (0,0,0,1), raster defaults.
  DefaultRaster(s_cur.r);
  for (int u = 0; u < 4; ++u) { s_cur.u.stageScale[u][0] = s_cur.u.stageScale[u][1] = 1.0f; s_cur.u.curTex[u][3] = 1.0f; }
  for (int i = 0; i < 4; ++i) s_cur.u.curColor[i] = 1.0f;
  for (int i = 0; i < 8; ++i)
  {
    Current::Light& L = s_cur.light[i];
    const float d = i == 0 ? 1.0f : 0.0f;   // GL: light 0 is white, others black
    const float amb[4] = { 0, 0, 0, 1 }, diff[4] = { d, d, d, 1 }, pos[4] = { 0, 0, 1, 0 }, att[3] = { 1, 0, 0 };
    memcpy(L.amb, amb, 16); memcpy(L.diff, diff, 16); memcpy(L.pos, pos, 16); memcpy(L.att, att, 12);
  }
  for (int u = 0; u < 4; ++u)
    for (int c = 0; c < 4; ++c)
    {
      s_cur.genMode[u][c] = 0x2400;   // GL_EYE_LINEAR
      // GL default planes: s = (1,0,0,0), t = (0,1,0,0), r = q = 0
      float pl[4] = { c == 0 ? 1.0f : 0.0f, c == 1 ? 1.0f : 0.0f, 0, 0 };
      memcpy(&s_cur.u.genObj[u][c * 4], pl, 16); memcpy(&s_cur.u.genEye[u][c * 4], pl, 16);
    }
  // Texture uploads (shared module): flush draws that still use the old
  // contents, then bind through the state cache.
  {
    GLTex::Backend be;
    be.bind = [](unsigned target, unsigned tex) {
      TextureWillChange(tex);
      GLenum t = target;
      if (t >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && t <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) t = GL_TEXTURE_CUBE_MAP;
      if (t != GL_TEXTURE_CUBE_MAP) t = GL_TEXTURE_2D;   // rectangle -> 2D
      StateBindTexture(0, t, tex);
    };
    be.log = [](const char* m) { Log("%s", m); };
    GLTex::SetBackend(be);
  }
  s_ready = true;
  Log("GLR: native backend ready (renderer %s, ring %s)", (const char*)gl.glGetString(GL_RENDERER), s_mapMode ? "map" : "upload");
  BeginFrame();
  return true;
}

void Shutdown()
{
  if (!s_ready) return;
  ProgramsShutdown();
  for (Ring* r : { &s_vring, &s_iring })
  {
    if (r->mapped) { gl.glBindBuffer(r->target, r->buf[r->slot]); gl.glUnmapBuffer(r->target); r->mapped = nullptr; }
    for (int i = 0; i < kSlots; ++i) if (r->fence[i]) gl.glDeleteSync(r->fence[i]);
    gl.glDeleteBuffers(kSlots, r->buf);
  }
  s_ready = false;
}

// ---------------------------------------------------------------- layouts
int RegisterLayout(const Layout& l)
{
  for (size_t i = 0; i < s_layouts.size(); ++i)
    if (!memcmp(&s_layouts[i], &l, sizeof(Layout))) return (int)i;
  s_layouts.push_back(l);
  return (int)s_layouts.size() - 1;
}
const Layout& GetLayout(int id) { return s_layouts[id]; }

// ---------------------------------------------------------------- recording
namespace
{
std::vector<uint8_t> s_arena;     // recorded buffer uploads of this segment
unsigned s_statUploadsRecorded = 0;

uint32_t RecordUniforms()
{
  memcpy(s_cur.u.mv, ModelView(), 64);
  memcpy(s_cur.u.proj, Projection(), 64);
  if (!s_uniforms.empty() && !memcmp(&s_uniforms.back(), &s_cur.u, sizeof(DrawUniforms))) return (uint32_t)s_uniforms.size() - 1;
  s_uniforms.push_back(s_cur.u);
  return (uint32_t)s_uniforms.size() - 1;
}

uint8_t AlphaTestMode(unsigned f)
{
  switch (f)
  {
    case GL_GREATER: return 1; case GL_LESS: return 2; case GL_GEQUAL: return 3; case GL_LEQUAL: return 4;
    case GL_EQUAL: return 5; case GL_NOTEQUAL: return 6; case GL_NEVER: return 7;
  }
  return 0;   // GL_ALWAYS
}

// Program, textures, raster state and uniforms of a draw (vertex bindings
// and indices are filled by the caller).
int s_traceLeft = -1, s_traceRequest = 0; double s_traceAt = 0; bool s_traceGameplay = false;
unsigned s_statRejected = 0, s_statCgDraws = 0, s_statNoArrays = 0;
bool PrepareDraw(Cmd& c, uint16_t attrMask, int colorSemantic)
{
  ++s_statRejected;   // undone at the end when the draw is recorded
  ProgramKey k; memset(&k, 0, sizeof(k));
  int n = 0;
  for (; n < 4; ++n)   // fixed function stops at the first disabled stage
  {
    if (!s_cur.tex[n].target) break;
    k.co[n] = s_cur.co[n]; k.ao[n] = s_cur.ao[n]; k.ca[n] = s_cur.ca[n]; k.aa[n] = s_cur.aa[n];
    if (s_cur.tex[n].target == TEX_CUBE) k.cube |= (uint8_t)(1 << n);
  }
  k.stages = (uint8_t)n;
  k.colorArray = (attrMask >> ATTR_COLOR) & 1;
  k.texStream = (uint8_t)((attrMask >> ATTR_TEX0) & 0xF);
  k.colorSemantic = k.colorArray ? (uint8_t)colorSemantic : 0;
  k.alphaTest = s_cur.alphaTestOn ? AlphaTestMode(s_cur.alphaFunc) : 0;
  s_cur.u.alphaRef = s_cur.alphaRef;
  // Texgen and texture matrices of the enabled stages.
  for (int u = 0; u < n; ++u)
  {
    for (int cc = 0; cc < 4; ++cc)
    {
      if (!((s_cur.texGenOn[u] >> cc) & 1)) continue;
      switch (s_cur.genMode[u][cc])
      {
        case 0x2401: k.gen[u][cc] = 1; break;   // GL_OBJECT_LINEAR
        case 0x2400: k.gen[u][cc] = 2; break;   // GL_EYE_LINEAR
        case 0x2402: k.gen[u][cc] = 3; break;   // GL_SPHERE_MAP
        case 0x8512: k.gen[u][cc] = 4; break;   // GL_REFLECTION_MAP
        case 0x8511: k.gen[u][cc] = 5; break;   // GL_NORMAL_MAP
      }
    }
    const float* tm = TextureMatrix(u);
    static const float id[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    if (memcmp(tm, id, 64)) { k.texMat |= (uint8_t)(1 << u); memcpy(s_cur.u.texMat[u], tm, 64); }
  }
  // Fixed-function lighting: enabled lights packed in GL order.
  bool needNormal = false;
  if (s_cur.lighting)
  {
    int nl = 0;
    for (int i = 0; i < 8 && nl < 4; ++i)
    {
      if (!((s_cur.lightOn >> i) & 1)) continue;
      const Current::Light& L = s_cur.light[i];
      memcpy(s_cur.u.lightPos[nl], L.pos, 16); memcpy(s_cur.u.lightAmb[nl], L.amb, 16);
      memcpy(s_cur.u.lightDiff[nl], L.diff, 16); memcpy(s_cur.u.lightAtt[nl], L.att, 12);
      ++nl;
    }
    k.lights = (uint8_t)(1 + nl);
    k.colorMaterial = s_cur.colorMaterial ? 1 : 0;
    memcpy(s_cur.u.matAmb, s_cur.matAmb, 16); memcpy(s_cur.u.matDiff, s_cur.matDiff, 16);
    memcpy(s_cur.u.matEmis, s_cur.matEmis, 16); memcpy(s_cur.u.sceneAmb, s_cur.sceneAmb, 16);
    needNormal = true;
  }
  for (int u = 0; u < n; ++u) for (int cc = 0; cc < 4; ++cc) if (k.gen[u][cc] >= 3) needNormal = true;
  if (needNormal)
  {
    // Normal matrix: inverse-transpose of the model-view's upper 3x3.
    const float* m = ModelView(); float inv[16];
    if (Invert4(m, inv)) { for (int r = 0; r < 3; ++r) for (int cc = 0; cc < 3; ++cc) s_cur.u.normalMat[cc * 3 + r] = inv[r * 4 + cc]; }
    else { for (int r = 0; r < 3; ++r) for (int cc = 0; cc < 3; ++cc) s_cur.u.normalMat[cc * 3 + r] = m[cc * 4 + r]; }
    memcpy(s_cur.u.curNormal, s_cur.curNormal, 12);
  }
  k.normalArray = (attrMask >> ATTR_NORMAL) & 1;
  if (s_cur.fogOn)
  {
    k.fog = s_cur.fogMode == 0x2601 /*GL_LINEAR*/ ? 1 : (s_cur.fogMode == 0x0801 /*GL_EXP2*/ ? 3 : 2);
    const float range = s_cur.fogEnd - s_cur.fogStart;
    s_cur.u.fog[0] = s_cur.fogStart; s_cur.u.fog[1] = s_cur.fogEnd; s_cur.u.fog[2] = range != 0 ? 1.0f / range : 0.0f; s_cur.u.fog[3] = s_cur.fogDensity;
    memcpy(s_cur.u.fogColor, s_cur.fogColor, 12);
  }
  // Engine Cg programs (vertex and/or fragment side).
  CgProgram* vp = nullptr; CgProgram* fp = nullptr;
  if ((s_cur.vpOn || s_cur.fpOn) && ProgramsSupported())
  {
    // FARCRY_GLR_NOCG=v|f|vf: diagnostic, run that side as fixed function.
    static const char* s_noCg = getenv("FARCRY_GLR_NOCG") ? getenv("FARCRY_GLR_NOCG") : "";
    if (s_cur.vpOn && !strchr(s_noCg, 'v')) vp = CgFind(CgBound(true), true);
    if (s_cur.fpOn && !strchr(s_noCg, 'f')) fp = CgFind(CgBound(false), false);
  }
  Program* prog;
  uint8_t fpUnits = 0, fpCube = 0;
  if (vp || fp)
  {
    prog = CgPair(vp, fp, k, fpUnits, fpCube);
    if (!prog) return false;
  }
  else prog = GetProgram(k);
  if (!prog || !prog->ok) return false;
  c.type = CMD_DRAW; c.prog = prog; c.r = s_cur.r; c.attrMask = attrMask; c.colorSemantic = (uint8_t)colorSemantic;
  for (int u = 0; u < 4; ++u) c.tex[u] = TexBind();
  if (fp)
  {
    // A fragment program samples its units whatever is enabled there.
    for (int u = 0; u < 4; ++u)
      if ((fpUnits >> u) & 1)
      {
        const bool cube = (fpCube >> u) & 1;
        c.tex[u].target = cube ? TEX_CUBE : TEX_2D; c.tex[u].name = cube ? s_cur.boundCube[u] : s_cur.bound2D[u];
        s_pendingTex.insert(c.tex[u].name);
      }
  }
  else for (int u = 0; u < n; ++u) { c.tex[u] = s_cur.tex[u]; s_pendingTex.insert(s_cur.tex[u].name); }
  c.cgParams = ~0u;
  if (vp || fp)
  {
    if (s_cgDirty || s_cgSnaps.empty()) { s_cgSnaps.push_back(s_cgEnv); s_cgDirty = false; }
    c.cgParams = (uint32_t)s_cgSnaps.size() - 1;
  }
  c.uniforms = RecordUniforms();
  // Diagnostic: FARCRY_GLR_TRACE=<draws>[@<seconds>] logs that many draws after the given time.
  if (s_traceRequest > 0) { s_traceLeft = s_traceRequest; s_traceAt = 0; s_traceGameplay = false; s_traceRequest = 0; }
  if (s_traceLeft < 0)
  {
    s_traceLeft = 0;
    if (const char* t = getenv("FARCRY_GLR_TRACE"))
    {
      s_traceLeft = atoi(t); const char* at = strchr(t, '@');
      s_traceGameplay = at && at[1] == 'g';                      // @g<sec>: seconds after gameplay starts
      s_traceAt = at ? atof(at + (s_traceGameplay ? 2 : 1)) : 0;
    }
  }
  const double traceNow = s_traceGameplay ? (g_PortGameplayStartMs > 0 ? (PortNowMs() - g_PortGameplayStartMs) / 1000.0 : -1.0) : SDL_GetTicks() / 1000.0;
  if (s_traceLeft > 0 && traceNow >= s_traceAt)
  {
    --s_traceLeft;
    const DrawUniforms& u = s_uniforms[c.uniforms];
    char att[400] = ""; int o = 0;
    for (int a = 0; a < ATTR_COUNT; ++a)
      if ((attrMask >> a) & 1) o += snprintf(att + o, sizeof(att) - o, " a%d(buf %d off %zu n%d t0x%x s%d)", a, c.vb[a].buf == RING_VTX ? -1 : (int)c.vb[a].buf, c.vb[a].off, c.vb[a].size, c.vb[a].type, c.vb[a].stride);
    char st[900] = ""; int so = 0;
    for (int u = 0; u < n; ++u)
      so += snprintf(st + so, sizeof(st) - so, " [t%d %u:%s/%d op %d/%d arg %d/%d gen %d%d%d%d sc %.1f col %.2f,%.2f,%.2f,%.2f]", u, c.tex[u].name, GLTex::TexName(c.tex[u].name), c.tex[u].target,
                     k.co[u], k.ao[u], k.ca[u], k.aa[u], k.gen[u][0], k.gen[u][1], k.gen[u][2], k.gen[u][3], s_cur.u.stageScale[u][0],
                     s_cur.u.stageColor[u][0], s_cur.u.stageColor[u][1], s_cur.u.stageColor[u][2], s_cur.u.stageColor[u][3]);
    Log("GLR trace: shader %s%s", s_debugLabel, (vp || fp) ? "" : " (fixed function)");
    if (vp || fp) Log("GLR trace:   programs vp %u fp %u (pair program %u)", vp ? CgBound(true) : 0, fp ? CgBound(false) : 0, prog->prog);
    Log("GLR trace: prim %d count %d ibo %d itype 0x%x lights %d cm %d colArr %d sem %d alpha %d fog %d blend %d %x/%x depth %d/%d cur %.2f,%.2f,%.2f,%.2f%s%s",
        c.prim, c.count, c.ibo == RING_IDX ? -1 : (int)c.ibo, c.indexType, k.lights, k.colorMaterial, k.colorArray, k.colorSemantic, k.alphaTest, k.fog,
        c.r.blend, c.r.blendSrc, c.r.blendDst, c.r.depthTest, c.r.depthWrite,
        s_cur.u.curColor[0], s_cur.u.curColor[1], s_cur.u.curColor[2], s_cur.u.curColor[3], st, att);
    if (k.lights > 1)
      Log("GLR trace:   light0 pos %.2f,%.2f,%.2f,%.2f amb %.2f,%.2f,%.2f diff %.2f,%.2f,%.2f  mat amb %.2f,%.2f,%.2f diff %.2f,%.2f,%.2f emis %.2f scene %.2f,%.2f,%.2f",
          s_cur.u.lightPos[0][0], s_cur.u.lightPos[0][1], s_cur.u.lightPos[0][2], s_cur.u.lightPos[0][3], s_cur.u.lightAmb[0][0], s_cur.u.lightAmb[0][1], s_cur.u.lightAmb[0][2],
          s_cur.u.lightDiff[0][0], s_cur.u.lightDiff[0][1], s_cur.u.lightDiff[0][2], s_cur.u.matAmb[0], s_cur.u.matAmb[1], s_cur.u.matAmb[2],
          s_cur.u.matDiff[0], s_cur.u.matDiff[1], s_cur.u.matDiff[2], s_cur.u.matEmis[0], s_cur.u.sceneAmb[0], s_cur.u.sceneAmb[1], s_cur.u.sceneAmb[2]);
    Log("GLR trace:   mv %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f   p %.2f %.2f %.2f %.2f / %.2f %.2f",
        u.mv[0], u.mv[4], u.mv[8], u.mv[12], u.mv[1], u.mv[5], u.mv[9], u.mv[13], u.mv[2], u.mv[6], u.mv[10], u.mv[14],
        u.proj[0], u.proj[5], u.proj[10], u.proj[14], u.proj[11], u.proj[15]);
  }
  for (int a = 0; a < ATTR_COUNT; ++a)
    if ((attrMask >> a) & 1 && c.vb[a].buf != RING_VTX) s_pendingBuf.insert(c.vb[a].buf);
  if (c.ibo && c.ibo != RING_IDX) s_pendingBuf.insert(c.ibo);
  --s_statRejected;
  if (c.cgParams != ~0u) ++s_statCgDraws;
  return true;
}

bool RingAllocBoth(size_t vbytes, RingAlloc& va, size_t ibytes, RingAlloc& ia)
{
  if ((!vbytes || RingAllocRaw(s_vring, vbytes, va)) && (!ibytes || RingAllocRaw(s_iring, ibytes, ia))) return true;
  Flush();   // map mode: segment full; play back and start a new one
  return (!vbytes || RingAllocRaw(s_vring, vbytes, va)) && (!ibytes || RingAllocRaw(s_iring, ibytes, ia));
}

const GLenum kPrimGL[] = { GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_LINES, GL_LINE_STRIP, GL_POINTS, GL_LINE_LOOP };
const uint16_t kCompGL[] = { GL_FLOAT, GL_UNSIGNED_BYTE, GL_SHORT };
} // namespace

// Client-memory geometry with a registered layout (immediate mode, tests).
void DrawClient(Prim p, int layout, const void* verts, int numVerts, const uint16_t* idx, int numIdx, int colorSemantic)
{
  if (!s_ready || numVerts <= 0) return;
  const Layout& l = GetLayout(layout);
  RingAlloc va = {}, ia = {};
  if (!RingAllocBoth((size_t)l.stride * numVerts, va, idx ? (size_t)numIdx * 2 : 0, ia)) { Log("GLR: frame ring too small for %d vertices", numVerts); return; }
  memcpy(va.cpu, verts, (size_t)l.stride * numVerts);
  if (idx) memcpy(ia.cpu, idx, (size_t)numIdx * 2);
  Cmd c; memset(&c, 0, sizeof(c));
  uint16_t mask = 0;
  for (int i = 0; i < l.count; ++i)
  {
    const AttribDesc& d = l.attrib[i];
    mask |= (uint8_t)(1 << d.attr);
    c.vb[d.attr] = VtxBinding{ RING_VTX, va.offset + d.offset, kCompGL[d.type], d.size, d.normalized, l.stride };
  }
  c.prim = p;
  if (idx) { c.ibo = RING_IDX; c.iboOffset = ia.offset; c.indexType = GL_UNSIGNED_SHORT; c.count = numIdx; }
  else { c.count = numVerts; c.first = 0; }
  if (PrepareDraw(c, mask, colorSemantic)) s_cmds.push_back(c);
}

// ---------------------------------------------------------------- immediate mode
namespace
{
struct IV { float pos[3]; uint8_t color[4]; float t0[4], t1[4]; };
std::vector<IV> s_imm;
std::vector<uint16_t> s_immIdx;
unsigned s_immPrim = 0;
bool s_inBegin = false;
int ImmLayout()
{
  static int id = -1;
  if (id < 0)
  {
    Layout l; memset(&l, 0, sizeof(l)); l.stride = sizeof(IV); l.count = 4;
    l.attrib[0] = AttribDesc{ ATTR_POS, 3, CT_FLOAT, 0, 0 };
    l.attrib[1] = AttribDesc{ ATTR_COLOR, 4, CT_UBYTE, 1, 12 };
    l.attrib[2] = AttribDesc{ ATTR_TEX0, 4, CT_FLOAT, 0, 16 };
    l.attrib[3] = AttribDesc{ ATTR_TEX1, 4, CT_FLOAT, 0, 32 };
    id = RegisterLayout(l);
  }
  return id;
}
inline uint8_t ToByte(float v) { return (uint8_t)(v <= 0 ? 0 : (v >= 1 ? 255 : v * 255.0f + 0.5f)); }
// GL primitive -> native primitive; quads / quad strips need indices (filled).
bool MapPrim(unsigned glPrim, int n, Prim& p, std::vector<uint32_t>& quadIdx)
{
  quadIdx.clear();
  switch (glPrim)
  {
    case 0x0000: p = PRIM_POINTS; return true;
    case 0x0001: p = PRIM_LINES; return true;
    case 0x0002: p = PRIM_LINE_LOOP; return true;
    case 0x0003: p = PRIM_LINE_STRIP; return true;
    case 0x0004: p = PRIM_TRIANGLES; return true;
    case 0x0005: p = PRIM_TRIANGLE_STRIP; return true;
    case 0x0006: case 0x0009: p = PRIM_TRIANGLE_FAN; return true;    // fan, convex polygon
    case 0x0007:                                                    // quads
      p = PRIM_TRIANGLES;
      for (int q = 0; q + 3 < n; q += 4) { const uint32_t t[6] = { (uint32_t)q, (uint32_t)q + 1, (uint32_t)q + 2, (uint32_t)q, (uint32_t)q + 2, (uint32_t)q + 3 }; quadIdx.insert(quadIdx.end(), t, t + 6); }
      return true;
    case 0x0008:                                                    // quad strip
      p = PRIM_TRIANGLES;
      for (int q = 0; q + 3 < n; q += 2) { const uint32_t t[6] = { (uint32_t)q, (uint32_t)q + 1, (uint32_t)q + 3, (uint32_t)q, (uint32_t)q + 3, (uint32_t)q + 2 }; quadIdx.insert(quadIdx.end(), t, t + 6); }
      return true;
  }
  return false;
}
std::vector<uint32_t> s_quadIdx;
} // namespace
void Begin(unsigned glPrim) { s_imm.clear(); s_immPrim = glPrim; s_inBegin = true; }
void Vertex(float x, float y, float z)
{
  if (!s_inBegin) return;
  IV v;
  v.pos[0] = x; v.pos[1] = y; v.pos[2] = z;
  for (int i = 0; i < 4; ++i) v.color[i] = ToByte(s_cur.u.curColor[i]);
  memcpy(v.t0, s_cur.u.curTex[0], 16); memcpy(v.t1, s_cur.u.curTex[1], 16);
  s_imm.push_back(v);
}
void End()
{
  if (!s_inBegin) return;
  s_inBegin = false;
  const int n = (int)s_imm.size();
  if (!n || n > 65535) return;
  Prim p;
  if (!MapPrim(s_immPrim, n, p, s_quadIdx)) return;
  if (s_quadIdx.empty() && (s_immPrim == 7 || s_immPrim == 8)) return;
  s_immIdx.assign(s_quadIdx.begin(), s_quadIdx.end());
  DrawClient(p, ImmLayout(), s_imm.data(), n, s_immIdx.empty() ? nullptr : s_immIdx.data(), (int)s_immIdx.size());
}

// ---------------------------------------------------------------- buffers
namespace
{
struct Buf { size_t size = 0; GLenum usage = GL_STATIC_DRAW; std::vector<uint8_t> shadow; bool mapped = false; };
std::unordered_map<GLuint, Buf> s_bufs;
GLuint s_boundArray = 0, s_boundElement = 0;   // the engine's bindings (CPU)
GLuint& Bound(unsigned target) { return target == GL_ELEMENT_ARRAY_BUFFER ? s_boundElement : s_boundArray; }

// Upload now, or record it when recorded draws still use the buffer.
void Upload(GLenum target, GLuint b, bool whole, size_t offset, size_t size, const void* data, GLenum usage)
{
  if (!s_pendingBuf.count(b))
  {
    StateBindBuffer(target, b);
    if (whole) gl.glBufferData(target, (GLsizeiptr)size, data, usage);
    else gl.glBufferSubData(target, (GLintptr)offset, (GLsizeiptr)size, data);
    return;
  }
  Cmd c; memset(&c, 0, sizeof(c));
  c.type = CMD_BUFFER; c.buf = b; c.bufTarget = (uint16_t)target; c.bufUsage = (uint16_t)usage; c.whole = whole;
  c.bufOffset = offset; c.bufSize = size; c.arenaOffset = (size_t)-1;
  if (data && size)
  {
    c.arenaOffset = s_arena.size();
    s_arena.insert(s_arena.end(), (const uint8_t*)data, (const uint8_t*)data + size);
  }
  s_cmds.push_back(c);
  ++s_statUploadsRecorded;
  // Draws recorded after this see the new contents: still pending, nothing to change.
}
} // namespace

void GenBuffers(int n, unsigned* out)
{
  gl.glGenBuffers(n, out);
  for (int i = 0; i < n; ++i) s_bufs[out[i]] = Buf();
}
void DeleteBuffers(int n, const unsigned* b)
{
  for (int i = 0; i < n; ++i)
  {
    if (!b[i]) continue;
    if (s_pendingBuf.count(b[i])) Flush();   // rare: a buffer pending draws use goes away
    StateForgetBuffer(b[i]);
    if (s_boundArray == b[i]) s_boundArray = 0;
    if (s_boundElement == b[i]) s_boundElement = 0;
    s_bufs.erase(b[i]);
    gl.glDeleteBuffers(1, &b[i]);
  }
}
void BindBuffer(unsigned target, unsigned b) { Bound(target) = b; }
void BufferData(unsigned target, size_t size, const void* data, unsigned usage)
{
  const GLuint b = Bound(target); if (!b) return;
  Buf& B = s_bufs[b];
  B.size = size; B.usage = usage;
  if (B.usage == GL_DYNAMIC_DRAW || B.usage == GL_STREAM_DRAW) { B.shadow.resize(size); if (data) memcpy(B.shadow.data(), data, size); }
  else B.shadow.clear();
  Upload(target, b, true, 0, size, data, usage);
}
void BufferSubData(unsigned target, size_t offset, size_t size, const void* data)
{
  const GLuint b = Bound(target); if (!b || !data) return;
  Buf& B = s_bufs[b];
  if (B.shadow.size() >= offset + size)
  {
    // Dynamic: keep the CPU copy and re-specify the whole buffer (orphaning,
    // never a GPU wait on a buffer in use).
    memcpy(B.shadow.data() + offset, data, size);
    Upload(target, b, true, 0, B.shadow.size(), B.shadow.data(), B.usage);
  }
  else Upload(target, b, false, offset, size, data, B.usage);
}
void* MapBuffer(unsigned target)
{
  const GLuint b = Bound(target); if (!b) return nullptr;
  Buf& B = s_bufs[b];
  if (B.shadow.size() != B.size) B.shadow.resize(B.size);   // static: temporary until Unmap
  B.mapped = true;
  return B.shadow.data();
}
bool UnmapBuffer(unsigned target)
{
  const GLuint b = Bound(target); if (!b) return false;
  Buf& B = s_bufs[b];
  if (!B.mapped) return false;
  B.mapped = false;
  Upload(target, b, true, 0, B.shadow.size(), B.shadow.data(), B.usage);
  if (B.usage != GL_DYNAMIC_DRAW && B.usage != GL_STREAM_DRAW) { std::vector<uint8_t>().swap(B.shadow); }   // static: free the copy
  return true;
}

// ---------------------------------------------------------------- vertex arrays
namespace
{
struct Array { bool on = false; uint8_t size = 4; uint16_t type = GL_FLOAT; bool norm = false; int stride = 0; GLuint buf = 0; const void* ptr = nullptr; };
Array s_arrays[ATTR_COUNT];
int s_clientUnit = 0;
int s_colorSemanticNext = 0, s_colorSemantic = 0;
int TypeSize(unsigned t) { switch (t) { case GL_BYTE: case GL_UNSIGNED_BYTE: return 1; case GL_SHORT: case GL_UNSIGNED_SHORT: case 0x140B /*HALF*/: return 2; } return 4; }

// Vertex bindings for the current arrays; client arrays copy vertices
// [minV, maxV] into the ring (the GL client-array contract).
bool BindArrays(Cmd& c, uint16_t& mask, unsigned minV, unsigned maxV)
{
  mask = 0;
  for (int a = 0; a < ATTR_COUNT; ++a)
  {
    const Array& A = s_arrays[a];
    if (!A.on) continue;
    const int size = A.size == 0x80E1 /*GL_BGRA*/ ? 4 : A.size;
    const int stride = A.stride ? A.stride : size * TypeSize(A.type);
    VtxBinding& v = c.vb[a];
    v.type = A.type; v.size = (uint8_t)size; v.norm = A.norm; v.stride = (uint16_t)stride;
    if (A.buf) { v.buf = A.buf; v.off = (size_t)A.ptr; }
    else
    {
      if (!A.ptr) continue;
      const size_t count = (size_t)maxV - minV + 1, elem = (size_t)size * TypeSize(A.type);
      RingAlloc ra;
      if (!RingAllocBoth(count * stride, ra, 0, ra)) return false;
      const uint8_t* src = (const uint8_t*)A.ptr + (size_t)minV * stride;
      if (stride == (int)elem) memcpy(ra.cpu, src, count * stride);
      else for (size_t i = 0; i < count; ++i) memcpy(ra.cpu + i * stride, src + i * stride, elem);
      // the draw's indices address vertex minV.. : the ring copy starts at minV
      v.buf = RING_VTX; v.off = ra.offset - (size_t)minV * stride;
    }
    mask |= (uint16_t)(1 << a);
  }
  return (mask & 1) != 0;   // a position array is required
}

template <typename T> void IndexRange(const T* idx, int count, unsigned& lo, unsigned& hi)
{
  lo = 0xFFFFFFFFu; hi = 0;
  for (int i = 0; i < count; ++i) { const unsigned v = idx[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
}
bool AnyClientArray() { for (const Array& A : s_arrays) if (A.on && !A.buf) return true; return false; }
} // namespace

void ArrayPointer(Attr a, int size, unsigned glType, bool normalized, int stride, const void* ptr)
{
  Array& A = s_arrays[a];
  A.size = (uint8_t)(size == 0x80E1 ? 4 : size); A.type = (uint16_t)glType; A.norm = normalized; A.stride = stride;
  A.buf = s_boundArray; A.ptr = ptr;
  if (a == ATTR_COLOR) { s_colorSemantic = s_colorSemanticNext; s_colorSemanticNext = 0; }
}
void EnableArray(Attr a, bool on)
{
  // The position array stays on: a draw always needs positions, and the
  // engine's stream bookkeeping (EF_CommitStreams) switches it off between
  // vertex-program passes that bind positions themselves.
  if (a == ATTR_POS && !on) return;
  s_arrays[a].on = on;
}
void ClientActiveTexture(int u) { if (u >= 0 && u < 8) s_clientUnit = u; }
int ClientActiveTextureUnit() { return s_clientUnit; }
void SetColorSemantic(int semantic) { s_colorSemanticNext = semantic; }

void DrawElements(unsigned mode, int count, unsigned type, const void* indices) { DrawRangeElements(mode, 0xFFFFFFFFu, 0, count, type, indices); }

void DrawRangeElements(unsigned mode, unsigned start, unsigned end, int count, unsigned type, const void* indices)
{
  if (!s_ready || count <= 0) return;
  Cmd c; memset(&c, 0, sizeof(c));
  Prim p;
  if (!MapPrim(mode, count, p, s_quadIdx)) return;
  const int isz = type == GL_UNSIGNED_INT ? 4 : 2;
  // Client-memory indices (or quads to expand): read them from memory.
  const void* cpuIdx = nullptr;
  if (!s_boundElement) cpuIdx = indices;
  else if (!s_quadIdx.empty() || (AnyClientArray() && start > end))
  {
    Buf& B = s_bufs[s_boundElement];
    if (B.shadow.size() >= (size_t)indices + (size_t)count * isz) cpuIdx = B.shadow.data() + (size_t)indices;
    else { Log("GLR: indexed draw needs CPU indices of a static buffer (mode 0x%x)", mode); return; }
  }
  unsigned lo = start, hi = end;
  if (AnyClientArray() && (lo > hi))
  {
    if (!cpuIdx) return;
    if (isz == 4) IndexRange((const uint32_t*)cpuIdx, count, lo, hi); else IndexRange((const uint16_t*)cpuIdx, count, lo, hi);
  }
  uint16_t mask;
  if (!BindArrays(c, mask, lo, hi))
  {
    if (++s_statNoArrays < 4 && getenv("FARCRY_GLR_DEBUG"))
      Log("GLR: draw without position array: on %d buf %u ptr %p size %d type 0x%x lo %u hi %u", s_arrays[0].on, s_arrays[0].buf, s_arrays[0].ptr, s_arrays[0].size, s_arrays[0].type, lo, hi);
    return;
  }
  c.prim = p; c.indexType = (uint16_t)type;
  if (!s_quadIdx.empty() || !s_boundElement)
  {
    // Indices into the ring: quads expanded to triangles.
    const int n = s_quadIdx.empty() ? count : (int)s_quadIdx.size() / 6 * 6 * 1;
    RingAlloc ia, dummy;
    const size_t bytes = (size_t)(s_quadIdx.empty() ? count : (int)s_quadIdx.size()) * isz;
    if (!RingAllocBoth(0, dummy, bytes, ia)) return;
    if (s_quadIdx.empty()) memcpy(ia.cpu, cpuIdx, bytes);
    else
    {
      // quad corner k of the source indices -> s_quadIdx gives positions in the index list
      for (size_t i = 0; i < s_quadIdx.size(); ++i)
      {
        const uint32_t v = isz == 4 ? ((const uint32_t*)cpuIdx)[s_quadIdx[i]] : ((const uint16_t*)cpuIdx)[s_quadIdx[i]];
        if (isz == 4) ((uint32_t*)ia.cpu)[i] = v; else ((uint16_t*)ia.cpu)[i] = (uint16_t)v;
      }
    }
    (void)n;
    c.ibo = RING_IDX; c.iboOffset = ia.offset; c.count = (int)(bytes / isz);
  }
  else { c.ibo = s_boundElement; c.iboOffset = (size_t)indices; c.count = count; }
  if (PrepareDraw(c, mask, s_colorSemantic)) s_cmds.push_back(c);
}

void DrawArrays(unsigned mode, int first, int count)
{
  if (!s_ready || count <= 0) return;
  Cmd c; memset(&c, 0, sizeof(c));
  Prim p;
  if (!MapPrim(mode, count, p, s_quadIdx)) return;
  uint16_t mask;
  if (!BindArrays(c, mask, (unsigned)first, (unsigned)(first + count - 1))) return;
  c.prim = p;
  if (!s_quadIdx.empty())
  {
    RingAlloc ia, dummy;
    if (!RingAllocBoth(0, dummy, s_quadIdx.size() * 4, ia)) return;
    for (size_t i = 0; i < s_quadIdx.size(); ++i) ((uint32_t*)ia.cpu)[i] = (uint32_t)first + s_quadIdx[i];
    c.ibo = RING_IDX; c.iboOffset = ia.offset; c.indexType = GL_UNSIGNED_INT; c.count = (int)s_quadIdx.size();
  }
  else { c.first = first; c.count = count; }
  if (PrepareDraw(c, mask, s_colorSemantic)) s_cmds.push_back(c);
}

// ---------------------------------------------------------------- playback
static unsigned s_profUniformCalls = 0, s_profAttribCalls = 0;   // playback profile counters
static void BindVertex(const Cmd& c, GLuint ringV)
{
  for (int i = 0; i < ATTR_COUNT; ++i)
  {
    GLState::A& a = s_gl.attr[i];
    if (!((c.attrMask >> i) & 1)) { if (a.on) { gl.glDisableVertexAttribArray(i); a.on = false; } continue; }
    const VtxBinding& d = c.vb[i];
    const GLuint buf = d.buf == RING_VTX ? ringV : d.buf;
    if (!a.on) { gl.glEnableVertexAttribArray(i); a.on = true; }
    if (a.buf != buf || a.size != d.size || a.type != d.type || a.norm != d.norm || a.stride != d.stride || a.ptr != d.off)
    {
      StateBindBuffer(GL_ARRAY_BUFFER, buf);
      gl.glVertexAttribPointer(i, d.size, d.type, d.norm ? GL_TRUE : GL_FALSE, d.stride, (const void*)d.off);
      ++s_profAttribCalls;
      a.buf = buf; a.size = d.size; a.type = d.type; a.norm = d.norm; a.stride = d.stride; a.ptr = d.off;
    }
  }
}

template <size_t N> static inline bool Changed(float (&dst)[N], const float* src, bool force)
{ if (!force && !memcmp(dst, src, sizeof(dst))) return false; memcpy(dst, src, sizeof(dst)); return true; }

static void UploadUniforms(Program& p, const DrawUniforms& u)
{
  const bool all = p.fresh; p.fresh = false;
  // (counted by the profile: each ++s_profUniformCalls, gl.glUniform* below)
  if (Changed(p.last.mv, u.mv, all) && p.uMV >= 0) ++s_profUniformCalls, gl.glUniformMatrix4fv(p.uMV, 1, GL_FALSE, u.mv);
  if (Changed(p.last.proj, u.proj, all) && p.uP >= 0) ++s_profUniformCalls, gl.glUniformMatrix4fv(p.uP, 1, GL_FALSE, u.proj);
  if (p.uCurColor >= 0 && Changed(p.last.curColor, u.curColor, all)) ++s_profUniformCalls, gl.glUniform4fv(p.uCurColor, 1, u.curColor);
  if (p.uCurTex >= 0 && (all || memcmp(p.last.curTex, u.curTex, sizeof(u.curTex))))
  { memcpy(p.last.curTex, u.curTex, sizeof(u.curTex)); ++s_profUniformCalls, gl.glUniform4fv(p.uCurTex, 4, &u.curTex[0][0]); }
  for (int s = 0; s < 4; ++s)
  {
    if (p.uStageColor[s] >= 0 && Changed(p.last.stageColor[s], u.stageColor[s], all)) ++s_profUniformCalls, gl.glUniform4fv(p.uStageColor[s], 1, u.stageColor[s]);
    if (p.uStageScale[s] >= 0 && Changed(p.last.stageScale[s], u.stageScale[s], all)) ++s_profUniformCalls, gl.glUniform2fv(p.uStageScale[s], 1, u.stageScale[s]);
  }
  if (p.uAlphaRef >= 0 && (all || p.last.alphaRef != u.alphaRef)) { p.last.alphaRef = u.alphaRef; ++s_profUniformCalls, gl.glUniform1f(p.uAlphaRef, u.alphaRef); }
  if (p.uLightPos >= 0)
  {
    // light arrays: compared as one block
    if (all || memcmp(p.last.lightPos, u.lightPos, sizeof(u.lightPos) + sizeof(u.lightAmb) + sizeof(u.lightDiff) + sizeof(u.lightAtt)))
    {
      memcpy(p.last.lightPos, u.lightPos, sizeof(u.lightPos)); memcpy(p.last.lightAmb, u.lightAmb, sizeof(u.lightAmb));
      memcpy(p.last.lightDiff, u.lightDiff, sizeof(u.lightDiff)); memcpy(p.last.lightAtt, u.lightAtt, sizeof(u.lightAtt));
      ++s_profUniformCalls, gl.glUniform4fv(p.uLightPos, 4, &u.lightPos[0][0]); ++s_profUniformCalls, gl.glUniform4fv(p.uLightAmb, 4, &u.lightAmb[0][0]);
      ++s_profUniformCalls, gl.glUniform4fv(p.uLightDiff, 4, &u.lightDiff[0][0]); ++s_profUniformCalls, gl.glUniform3fv(p.uLightAtt, 4, &u.lightAtt[0][0]);
    }
  }
  if (p.uMatAmb >= 0 && Changed(p.last.matAmb, u.matAmb, all)) ++s_profUniformCalls, gl.glUniform4fv(p.uMatAmb, 1, u.matAmb);
  if (p.uMatDiff >= 0 && Changed(p.last.matDiff, u.matDiff, all)) ++s_profUniformCalls, gl.glUniform4fv(p.uMatDiff, 1, u.matDiff);
  if (p.uMatEmis >= 0 && Changed(p.last.matEmis, u.matEmis, all)) ++s_profUniformCalls, gl.glUniform4fv(p.uMatEmis, 1, u.matEmis);
  if (p.uSceneAmb >= 0 && Changed(p.last.sceneAmb, u.sceneAmb, all)) ++s_profUniformCalls, gl.glUniform4fv(p.uSceneAmb, 1, u.sceneAmb);
  if (p.uNormalMat >= 0 && Changed(p.last.normalMat, u.normalMat, all)) ++s_profUniformCalls, gl.glUniformMatrix3fv(p.uNormalMat, 1, GL_FALSE, u.normalMat);
  if (p.uCurNormal >= 0 && Changed(p.last.curNormal, u.curNormal, all)) ++s_profUniformCalls, gl.glUniform3fv(p.uCurNormal, 1, u.curNormal);
  for (int s = 0; s < 4; ++s)
  {
    if (p.uGenObj[s] >= 0 && Changed(p.last.genObj[s], u.genObj[s], all)) ++s_profUniformCalls, gl.glUniformMatrix4fv(p.uGenObj[s], 1, GL_FALSE, u.genObj[s]);
    if (p.uGenEye[s] >= 0 && Changed(p.last.genEye[s], u.genEye[s], all)) ++s_profUniformCalls, gl.glUniformMatrix4fv(p.uGenEye[s], 1, GL_FALSE, u.genEye[s]);
    if (p.uTexMat[s] >= 0 && Changed(p.last.texMat[s], u.texMat[s], all)) ++s_profUniformCalls, gl.glUniformMatrix4fv(p.uTexMat[s], 1, GL_FALSE, u.texMat[s]);
  }
  if (p.uFog >= 0 && Changed(p.last.fog, u.fog, all)) ++s_profUniformCalls, gl.glUniform4fv(p.uFog, 1, u.fog);
  if (p.uFogColor >= 0 && Changed(p.last.fogColor, u.fogColor, all)) ++s_profUniformCalls, gl.glUniform3fv(p.uFogColor, 1, u.fogColor);
}

static unsigned s_statDraws = 0, s_statClears = 0, s_statSegments = 0, s_statProgSwitch = 0, s_statUploads = 0;
static size_t s_statRingBytes = 0;

// FARCRY_GLR_PROFILE=1: where playback time goes (state, uniforms, textures, vertex, draw calls).
static const bool s_prof = getenv("FARCRY_GLR_PROFILE") != NULL;
static uint64_t s_profT[6] = {};
static void Playback()
{
  const uint64_t tPlay0 = s_prof ? SDL_GetPerformanceCounter() : 0;
  // The ring data of this segment becomes visible first.
  const size_t vUsed = s_vring.used, iUsed = s_iring.used;
  const GLuint vbuf = RingEndSegment(s_vring);
  const GLuint ibuf = RingEndSegment(s_iring);
  s_gl.arrayBuf = vbuf; s_gl.elemBuf = ibuf;
  s_gl.prog = 0xFFFFFFFFu;   // programs built while recording were bound outside playback
  s_statRingBytes += vUsed + iUsed;
  for (const Cmd& c : s_cmds)
  {
    if (c.type == CMD_BUFFER)
    {
      StateBindBuffer(c.bufTarget, c.buf);
      const void* data = c.arenaOffset == (size_t)-1 ? nullptr : s_arena.data() + c.arenaOffset;
      if (c.whole) gl.glBufferData(c.bufTarget, (GLsizeiptr)c.bufSize, data, c.bufUsage);
      else gl.glBufferSubData(c.bufTarget, (GLintptr)c.bufOffset, (GLsizeiptr)c.bufSize, data);
      ++s_statUploads;
      continue;
    }
    if (c.type == CMD_CLEAR)
    {
      // A clear is limited by the scissor box and the write masks (GL rules).
      { static const bool d2 = getenv("FARCRY_GLR_DEBUG") && atoi(getenv("FARCRY_GLR_DEBUG")) >= 2;
        if (d2) { static int n = 0; const GLenum e = gl.glGetError(); if (e && n < 10) { ++n; Log("GLR error 0x%x BEFORE clear (earlier call)", e); } } }
      ApplyRaster(c.r);
      GLbitfield m = 0;
      if (c.clearMask & GL_COLOR_BUFFER_BIT) { gl.glClearColor(c.clearColor[0], c.clearColor[1], c.clearColor[2], c.clearColor[3]); m |= GL_COLOR_BUFFER_BIT; }
      if (c.clearMask & GL_DEPTH_BUFFER_BIT) { gl.glClearDepthf(c.clearDepth); m |= GL_DEPTH_BUFFER_BIT; }
      if (c.clearMask & GL_STENCIL_BUFFER_BIT) { gl.glClearStencil(c.clearStencil); m |= GL_STENCIL_BUFFER_BIT; }
      if (m) gl.glClear(m);
      ++s_statClears;
      { static const bool d2 = getenv("FARCRY_GLR_DEBUG") && atoi(getenv("FARCRY_GLR_DEBUG")) >= 2; static int n = 0;
        if (d2 && n < 5) { const GLenum e = gl.glGetError(); if (e) { ++n; Log("GLR error 0x%x at clear mask 0x%x", e, m); } } }
      continue;
    }
    Program& p = *c.prog;
    // FARCRY_GLR_ONLYPROG=<GL program>: diagnostic, draw only that program's draws.
    { static const int only = getenv("FARCRY_GLR_ONLYPROG") ? atoi(getenv("FARCRY_GLR_ONLYPROG")) : 0;
      if (only && (int)p.prog != only && c.count > 6) continue; }
    { static const bool d2 = getenv("FARCRY_GLR_DEBUG") && atoi(getenv("FARCRY_GLR_DEBUG")) >= 2;   // attribute errors exactly
      if (d2) { static int n = 0; const GLenum e = gl.glGetError(); if (e && n < 10) { ++n; Log("GLR error 0x%x BEFORE draw (earlier call)", e); } } }
    const uint64_t t0 = s_prof ? SDL_GetPerformanceCounter() : 0;
    ApplyRaster(c.r);
    if (s_gl.prog != p.prog) { gl.glUseProgram(p.prog); s_gl.prog = p.prog; ++s_statProgSwitch; }
    const uint64_t t1 = s_prof ? SDL_GetPerformanceCounter() : 0;
    UploadUniforms(p, s_uniforms[c.uniforms]);
    if (c.cgParams != ~0u)
    {
      const uint64_t tag = ((uint64_t)s_statSegments << 32) | c.cgParams;
      if (p.lastCg != tag)
      {
        p.lastCg = tag;
        const CgParams& cp = s_cgSnaps[c.cgParams];
        if (p.uCv >= 0 && p.nCv) ++s_profUniformCalls, gl.glUniform4fv(p.uCv, p.nCv, &cp.v[0][0]);
        if (p.uCf >= 0 && p.nCf) ++s_profUniformCalls, gl.glUniform4fv(p.uCf, p.nCf, &cp.f[0][0]);
      }
    }
    const uint64_t t2 = s_prof ? SDL_GetPerformanceCounter() : 0;
    for (int u = 0; u < 4; ++u)
      if (c.tex[u].target) StateBindTexture(u, c.tex[u].target == TEX_CUBE ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, c.tex[u].name);
    const uint64_t t3 = s_prof ? SDL_GetPerformanceCounter() : 0;
    BindVertex(c, vbuf);
    // A vertex program reads the current colour / texcoords where no array is
    // enabled (ARB: vertex.color etc. = current values).
    if (p.uCv >= 0)
    {
      const DrawUniforms& du = s_uniforms[c.uniforms];
      if (!((c.attrMask >> ATTR_COLOR) & 1)) gl.glVertexAttrib4fv(ATTR_COLOR, du.curColor);
      for (int u = 0; u < 4; ++u) if (!((c.attrMask >> (ATTR_TEX0 + u)) & 1)) gl.glVertexAttrib4fv(ATTR_TEX0 + u, du.curTex[u]);
    }
    if (c.ibo) StateBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c.ibo == RING_IDX ? ibuf : c.ibo);
    const uint64_t t4 = s_prof ? SDL_GetPerformanceCounter() : 0;
    if (c.ibo) gl.glDrawElements(kPrimGL[c.prim], c.count, c.indexType, (const void*)c.iboOffset);
    else gl.glDrawArrays(kPrimGL[c.prim], c.first, c.count);
    if (s_prof)
    {
      const uint64_t t5 = SDL_GetPerformanceCounter();
      s_profT[0] += t1 - t0; s_profT[1] += t2 - t1; s_profT[2] += t3 - t2; s_profT[3] += t4 - t3; s_profT[4] += t5 - t4;
    }
    ++s_statDraws;
    // FARCRY_GLR_DEBUG=2: GL error after every draw, first offenders logged.
    static const bool s_debug2 = getenv("FARCRY_GLR_DEBUG") && atoi(getenv("FARCRY_GLR_DEBUG")) >= 2;
    static int s_errLogged = 0;
    if (s_debug2 && s_errLogged < 20)
    {
      const GLenum e = gl.glGetError();
      if (e)
      {
        ++s_errLogged;
        Log("GLR error 0x%x at draw: prim %d count %d first %d ibo %d+%zu itype 0x%x mask 0x%x blend %d %x/%x depthfunc %x cull %d/%x front %x colormask %x tex0 %u/%d stages-key %d",
            e, c.prim, c.count, c.first, (int)c.ibo, c.iboOffset, c.indexType, c.attrMask, c.r.blend, c.r.blendSrc, c.r.blendDst, c.r.depthFunc,
            c.r.cullOn, c.r.cullFace, c.r.frontFace, c.r.colorMask, c.tex[0].name, c.tex[0].target, p.prog);
        for (int a = 0; a < ATTR_COUNT; ++a)
          if ((c.attrMask >> a) & 1) Log("GLR error   attr %d: buf %d off %zu size %d type 0x%x norm %d stride %d", a, (int)c.vb[a].buf, c.vb[a].off, c.vb[a].size, c.vb[a].type, c.vb[a].norm, c.vb[a].stride);
      }
    }
  }
  RingFence(s_vring); RingFence(s_iring);
  if (s_prof) s_profT[5] += SDL_GetPerformanceCounter() - tPlay0;
  static const bool s_debug = getenv("FARCRY_GLR_DEBUG") != NULL;   // diagnostic
  if (s_debug && !s_cmds.empty())
  {
    GLint fb = 0, vp[4] = {};
    gl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb); gl.glGetIntegerv(GL_VIEWPORT, vp);
    Log("GLR debug: %zu commands, GL error 0x%x, draw framebuffer %d, viewport %d,%d %dx%d", s_cmds.size(), gl.glGetError(), fb, vp[0], vp[1], vp[2], vp[3]);
  }
  s_cmds.clear(); s_uniforms.clear(); s_arena.clear(); s_cgSnaps.clear(); s_cgDirty = true;
  s_pendingTex.clear(); s_pendingBuf.clear();
  ++s_statSegments;
  RingBeginSegment(s_vring); RingBeginSegment(s_iring);
}

void BeginFrame()
{
  if (!s_ready) return;
  // Other code (scene FBO blit, swap) changes GL state between frames.
  Invalidate();
}
void Flush() { if (s_ready) Playback(); }
void TraceNextDraws(int n) { s_traceRequest = n; }
void SetDebugLabel(const char* n) { s_debugLabel = n ? n : ""; }
void EndFrame() { if (s_ready) { Playback(); FrameStats(); } }

// Test pattern: two overlapping quads in clip space (colour stream, alpha
// blend), drawn last. Proves programs, ring, playback and state.
void SelfTest()
{
  struct V { float x, y, z; uint8_t c[4]; };
  static int layout = -1;
  if (layout < 0)
  {
    Layout l; memset(&l, 0, sizeof(l)); l.stride = sizeof(V); l.count = 2;
    l.attrib[0] = AttribDesc{ ATTR_POS, 3, CT_FLOAT, 0, 0 };
    l.attrib[1] = AttribDesc{ ATTR_COLOR, 4, CT_UBYTE, 1, 12 };
    layout = RegisterLayout(l);
  }
  const Current saved = s_cur;
  MatrixModeSet(MAT_PROJECTION); PushMatrix(); LoadIdentity();
  MatrixModeSet(MAT_MODELVIEW); PushMatrix(); LoadIdentity();
  for (int u = 0; u < 4; ++u) { s_cur.on2D[u] = s_cur.onCube[u] = false; UpdateStage(u); }
  s_cur.r.cullOn = 0; s_cur.r.depthTest = 0; s_cur.r.colorMask = 0xF; s_cur.r.scissorOn = 0;
  s_cur.alphaTestOn = false; s_cur.fogOn = false;
  static const uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
  const V a[4] = { { -0.95f, 0.95f, 0, { 255, 0, 0, 255 } }, { -0.55f, 0.95f, 0, { 0, 255, 0, 255 } },
                   { -0.55f, 0.55f, 0, { 0, 0, 255, 255 } }, { -0.95f, 0.55f, 0, { 255, 255, 255, 255 } } };
  s_cur.r.blend = 0;
  DrawClient(PRIM_TRIANGLES, layout, a, 4, idx, 6);
  V b[4]; memcpy(b, a, sizeof(b));
  for (V& v : b) { v.x += 0.2f; v.y -= 0.2f; v.c[0] = 255; v.c[1] = 255; v.c[2] = 0; v.c[3] = 128; }
  s_cur.r.blend = 1; s_cur.r.blendSrc = GL_SRC_ALPHA; s_cur.r.blendDst = GL_ONE_MINUS_SRC_ALPHA;
  DrawClient(PRIM_TRIANGLES, layout, b, 4, idx, 6);
  MatrixModeSet(MAT_PROJECTION); PopMatrix(); MatrixModeSet(MAT_MODELVIEW); PopMatrix();
  s_cur = saved;
}

void FrameStats()
{
  static unsigned frames = 0;
  if (++frames % 120) return;
  Log("GLR: last 120 frames: %u draws, %u clears, %u segments, %u program switches, %u buffer uploads in order (%u recorded), %.1f KB ring data per frame",
      s_statDraws, s_statClears, s_statSegments, s_statProgSwitch, s_statUploads, s_statUploadsRecorded, s_statRingBytes / 1024.0 / 120.0);
  if (s_statRejected || s_statCgDraws || s_statNoArrays) Log("GLR: last 120 frames: %u draws with engine programs, %u draws rejected, %u without position array", s_statCgDraws, s_statRejected, s_statNoArrays);
  s_statRejected = s_statCgDraws = s_statNoArrays = 0;
  s_statDraws = s_statClears = s_statSegments = s_statProgSwitch = s_statUploads = s_statUploadsRecorded = 0; s_statRingBytes = 0;
  if (s_prof)
  {
    const double f = 1000.0 / (double)SDL_GetPerformanceFrequency() / 120.0;
    Log("GLR profile (ms per frame): playback %.2f = raster+program %.2f, uniforms %.2f, textures %.2f, vertex %.2f, draw calls %.2f; per frame %u uniform calls, %u attribute pointers",
        s_profT[5] * f, s_profT[0] * f, s_profT[1] * f, s_profT[2] * f, s_profT[3] * f, s_profT[4] * f, s_profUniformCalls / 120, s_profAttribCalls / 120);
    memset(s_profT, 0, sizeof(s_profT)); s_profUniformCalls = s_profAttribCalls = 0;
  }
}
} // namespace GLR
