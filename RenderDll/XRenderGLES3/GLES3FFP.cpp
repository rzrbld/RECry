#include <algorithm>
#include <map>
/*=============================================================================
  GLES3FFP.cpp : fixed-function state core for the GLES3 renderer.
  See GLES3FFP.h for the overall contract.
=============================================================================*/

#include <GLES3/gl3.h>
#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <execinfo.h>
#include <sys/stat.h>
#include "../../port/platform/PortTiming.h"
#include <unordered_map>
#include <unordered_set>
#include "GLES3TexConvert.h"
#include "GLTexUpload.h"
#include "GLR.h"

#ifndef APIENTRY
#define APIENTRY
#endif

// Desktop GL scalar types absent from the ES headers.
typedef double GLdouble;
typedef float GLclampf;

///////////////////////////////////////////////////////////////////////////////
// Desktop GL enums the engine passes (not defined by the ES headers)
///////////////////////////////////////////////////////////////////////////////
enum
{
  D_QUADS = 0x0007, D_QUAD_STRIP = 0x0008, D_POLYGON = 0x0009,
  D_MODELVIEW = 0x1700, D_PROJECTION = 0x1701, D_TEXTURE = 0x1702,
  D_MATRIX_MODE = 0x0BA0, D_MODELVIEW_MATRIX = 0x0BA6, D_PROJECTION_MATRIX = 0x0BA7, D_TEXTURE_MATRIX = 0x0BA8,
  D_CURRENT_COLOR = 0x0B00,
  D_LIGHTING = 0x0B50, D_COLOR_MATERIAL = 0x0B57, D_FOG = 0x0B60, D_NORMALIZE = 0x0BA1, D_ALPHA_TEST = 0x0BC0,
  D_LIGHT0 = 0x4000, D_CLIP_PLANE0 = 0x3000,
  D_TEXTURE_1D = 0x0DE0, D_TEXTURE_CUBE_MAP = 0x8513, D_TEXTURE_3D = 0x806F, D_TEXTURE_RECTANGLE = 0x84F5,
  D_TEXTURE_GEN_S = 0x0C60, D_TEXTURE_GEN_T = 0x0C61, D_TEXTURE_GEN_R = 0x0C62, D_TEXTURE_GEN_Q = 0x0C63,
  D_VERTEX_ARRAY = 0x8074, D_NORMAL_ARRAY = 0x8075, D_COLOR_ARRAY = 0x8076, D_TEXTURE_COORD_ARRAY = 0x8078,
  D_SECONDARY_COLOR_ARRAY = 0x845E, D_FOG_COORD_ARRAY = 0x8457,
  D_TEXTURE_ENV = 0x2300, D_TEXTURE_ENV_MODE = 0x2200, D_TEXTURE_ENV_COLOR = 0x2201,
  D_MODULATE = 0x2100, D_DECAL = 0x2101, D_ADD = 0x0104, D_BLEND = 0x0BE2, D_REPLACE = 0x1E01, D_COMBINE = 0x8570,
  D_COMBINE_RGB = 0x8571, D_COMBINE_ALPHA = 0x8572, D_RGB_SCALE = 0x8573, D_ALPHA_SCALE = 0x0D1C,
  D_SOURCE0_RGB = 0x8580, D_SOURCE0_ALPHA = 0x8588, D_OPERAND0_RGB = 0x8590, D_OPERAND0_ALPHA = 0x8598,
  D_ADD_SIGNED = 0x8574, D_INTERPOLATE = 0x8575, D_SUBTRACT = 0x84E7, D_DOT3_RGB = 0x86AE, D_DOT3_RGBA = 0x86AF,
  D_DOT3_RGB_EXT = 0x8740, D_DOT3_RGBA_EXT = 0x8741,
  D_CONSTANT = 0x8576, D_PRIMARY_COLOR = 0x8577, D_PREVIOUS = 0x8578, D_TEXTURE0 = 0x84C0,
  D_SRC_COLOR = 0x0300, D_ONE_MINUS_SRC_COLOR = 0x0301, D_SRC_ALPHA = 0x0302, D_ONE_MINUS_SRC_ALPHA = 0x0303,
  D_FOG_MODE = 0x0B65, D_FOG_DENSITY = 0x0B62, D_FOG_START = 0x0B63, D_FOG_END = 0x0B64, D_FOG_COLOR = 0x0B66,
  D_EXP = 0x0800, D_EXP2 = 0x0801, D_LINEAR_FOG = 0x2601,
  D_S = 0x2000, D_T = 0x2001, D_R = 0x2002, D_Q = 0x2003,
  D_TEXTURE_GEN_MODE = 0x2500, D_OBJECT_PLANE = 0x2501, D_EYE_PLANE = 0x2502,
  D_EYE_LINEAR = 0x2400, D_OBJECT_LINEAR = 0x2401, D_SPHERE_MAP = 0x2402, D_NORMAL_MAP = 0x8511, D_REFLECTION_MAP = 0x8512,
  D_AMBIENT = 0x1200, D_DIFFUSE = 0x1201, D_SPECULAR = 0x1202, D_POSITION = 0x1203,
  D_CONSTANT_ATTENUATION = 0x1207, D_LINEAR_ATTENUATION = 0x1208, D_QUADRATIC_ATTENUATION = 0x1209,
  D_EMISSION = 0x1600, D_SHININESS = 0x1601, D_AMBIENT_AND_DIFFUSE = 0x1602, D_LIGHT_MODEL_AMBIENT = 0x0B53,
  D_CLAMP = 0x2900, D_CLAMP_TO_BORDER = 0x812D, D_GENERATE_MIPMAP = 0x8191, D_TEXTURE_MAX_ANISOTROPY = 0x84FE,
  D_BGR = 0x80E0, D_BGRA = 0x80E1, D_LUMINANCE = 0x1909, D_LUMINANCE_ALPHA = 0x190A, D_ALPHA = 0x1906,
  D_INTENSITY = 0x8049, D_INTENSITY8 = 0x804B, D_ALPHA8 = 0x803C, D_LUMINANCE8 = 0x8040, D_LUMINANCE8_ALPHA8 = 0x8045,
  D_RGB4 = 0x804F, D_RGB5 = 0x8050, D_RGB8 = 0x8051, D_RGBA2 = 0x8055, D_RGBA4 = 0x8056, D_RGB5_A1 = 0x8057, D_RGBA8 = 0x8058,
  D_UNSIGNED_INT_8_8_8_8_REV = 0x8367, D_UNSIGNED_INT_8_8_8_8 = 0x8035,
  D_READ_ONLY = 0x88B8, D_WRITE_ONLY = 0x88B9, D_READ_WRITE = 0x88BA,
  D_MAX_TEXTURE_UNITS = 0x84E2, D_MAX_TEXTURE_COORDS = 0x8871, D_MAX_TEXTURE_IMAGE_UNITS = 0x8872,
  D_DOUBLE = 0x140A,
};

///////////////////////////////////////////////////////////////////////////////
// Driver entry points used by the core
///////////////////////////////////////////////////////////////////////////////
#define FFP_REAL_LIST(X) \
  X(glEnable) X(glDisable) X(glIsEnabled) X(glDrawArrays) X(glDrawElements) X(glDrawRangeElements) \
  X(glTexImage2D) X(glTexSubImage2D) X(glTexParameteri) X(glTexParameterf) X(glGetFloatv) X(glGetIntegerv) \
  X(glActiveTexture) X(glBindBuffer) X(glBindTexture) X(glVertexAttribPointer) X(glEnableVertexAttribArray) \
  X(glDisableVertexAttribArray) X(glVertexAttrib4f) X(glUseProgram) X(glCreateShader) X(glShaderSource) \
  X(glCompileShader) X(glGetShaderiv) X(glGetShaderInfoLog) X(glCreateProgram) X(glAttachShader) \
  X(glBindAttribLocation) X(glLinkProgram) X(glGetProgramiv) X(glGetProgramInfoLog) X(glDeleteShader) \
  X(glGetUniformLocation) X(glUniform1i) X(glUniform1f) X(glUniform2fv) X(glUniform3fv) X(glUniform4fv) X(glUniformMatrix3fv) \
  X(glUniformMatrix4fv) X(glGenBuffers) X(glBufferData) X(glMapBufferRange) X(glGetBufferParameteriv) \
  X(glUnmapBuffer) X(glDepthRangef) X(glClearDepthf) X(glGenerateMipmap) X(glDeleteBuffers) X(glDeleteProgram) \
  X(glBindVertexArray) X(glCompressedTexImage2D) X(glCompressedTexSubImage2D) X(glGetString) X(glPixelStorei) X(glDeleteTextures) X(glIsTexture) X(glReadPixels) X(glGetTexParameteriv) X(glProgramBinary) X(glGetProgramBinary) X(glProgramParameteri) X(glBufferSubData) X(glCopyTexSubImage2D) X(glDepthFunc) X(glFinish) X(glHint)

#define FFP_DECLARE_REAL(n) static decltype(&::n) r_##n = 0;
FFP_REAL_LIST(FFP_DECLARE_REAL)
#undef FFP_DECLARE_REAL

static void (*s_log)(const char*) = 0;
static void FFPLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void FFPLog(const char* fmt, ...)
{
  if (!s_log) return;
  char buf[1024];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  s_log(buf);
}

///////////////////////////////////////////////////////////////////////////////
// State
///////////////////////////////////////////////////////////////////////////////
enum { MAX_UNITS = 4, MAX_LIGHTS = 4, ATTR_POS = 0, ATTR_COLOR = 1, ATTR_NORMAL = 2, ATTR_TEX0 = 3, ATTR_COLOR2 = 7, NUM_ATTRS = 8 };

struct SMat { float m[16]; };

static void MatIdentity(float* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }
static void MatMul(float* out, const float* a, const float* b) // out = a * b (column-major)
{
  float r[16];
  for (int c = 0; c < 4; ++c)
    for (int rr = 0; rr < 4; ++rr)
      r[c*4+rr] = a[0*4+rr]*b[c*4+0] + a[1*4+rr]*b[c*4+1] + a[2*4+rr]*b[c*4+2] + a[3*4+rr]*b[c*4+3];
  memcpy(out, r, 64);
}
static bool MatIsIdentity(const float* m)
{
  static float id[16]; static bool init = false;
  if (!init) { MatIdentity(id); init = true; }
  return memcmp(m, id, 64) == 0;
}
static bool MatInverse(float* inv, const float* m)
{
  float t[16];
  t[0] = m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
  t[4] = -m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
  t[8] = m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
  t[12] = -m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
  t[1] = -m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
  t[5] = m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
  t[9] = -m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
  t[13] = m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
  t[2] = m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
  t[6] = -m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
  t[10] = m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
  t[14] = -m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
  t[3] = -m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
  t[7] = m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
  t[11] = -m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
  t[15] = m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
  float det = m[0]*t[0] + m[1]*t[4] + m[2]*t[8] + m[3]*t[12];
  if (fabsf(det) < 1e-20f) { MatIdentity(inv); return false; }
  det = 1.0f / det;
  for (int i = 0; i < 16; ++i) inv[i] = t[i] * det;
  return true;
}

// Matrix stacks: 0 modelview, 1 projection, 2.. texture per unit.
static std::vector<SMat> s_stack[2 + MAX_UNITS];
static int s_matrixMode = 0;              // 0 modelview, 1 projection, 2 texture
static int s_activeUnit = 0, s_clientUnit = 0;
static bool s_matDirty = true;

static float* CurMat()
{
  int idx = s_matrixMode < 2 ? s_matrixMode : 2 + s_activeUnit;
  return s_stack[idx].back().m;
}
static float* Modelview() { return s_stack[0].back().m; }
static float* Projection() { return s_stack[1].back().m; }
static float* TexMatrix(int u) { return s_stack[2 + u].back().m; }

// semantic (colour arrays): 0 = colour, 1 = terrain layer masks with brightness
// in alpha (the engine's terrain vertex program used only the brightness).
struct SClientArray { bool on; GLint size; GLenum type; GLsizei stride; const void* ptr; GLuint buffer; int semantic; };
static int s_pendingColorSemantic = 0;
static float s_forceEnvAlpha0 = -1.0f; // see GLES3FFPForceEnvAlpha0
static const char* s_dumpLabel = ""; // diagnostic: shader name for frame dumps
void GLES3FFPSetDumpLabel(const char* n) { s_dumpLabel = n ? n : ""; GLR::SetDebugLabel(s_dumpLabel); }
static SClientArray s_vertexArr, s_colorArr, s_normalArr, s_color2Arr, s_texArr[MAX_UNITS];
static float s_curColor[4] = {1,1,1,1}, s_curNormal[3] = {0,0,1}, s_curTex[MAX_UNITS][4];
static GLuint s_arrayBuffer = 0, s_elementBuffer = 0;
// Big shared dynamic pools (particles, decals) are rewritten every frame. Orphaning
// them with glBufferData(NULL) on the first map of a frame made Mali block inside
// the next draw for 2-7 ms (Catacombs firefight: ~800 ms of stalls in 45 s), and an
// unsynchronised map of the same storage races the GPU (flicker). Instead each
// such engine buffer gets a ring of real buffers: the first map of a frame moves
// to the next one (written unsynchronised: the GPU finished with it frames ago).
// Binds translate engine ids to the current ring member.
struct SBufRing { GLuint ids[4]; int n; int cur; };
static std::unordered_map<GLuint, SBufRing> s_bufRings;
static inline GLuint RealBuf(GLuint b)
{
  if (!b || s_bufRings.empty()) return b;
  auto it = s_bufRings.find(b);
  return it == s_bufRings.end() ? b : it->second.ids[it->second.cur];
}
static inline void BindReal(GLenum t, GLuint b) { r_glBindBuffer(t, RealBuf(b)); }
static void DropRing(GLuint b)
{
  auto it = s_bufRings.find(b);
  if (it == s_bufRings.end()) return;
  if (it->second.n > 1) r_glDeleteBuffers(it->second.n - 1, &it->second.ids[1]); // ids[0] is the engine's own buffer
  s_bufRings.erase(it);
}
static GLuint s_userProgram = 0;

struct STexEnv
{
  GLenum mode, combRGB, combA;
  GLenum srcRGB[3], srcA[3], opRGB[3], opA[3];
  float scaleRGB, scaleA;
  float color[4];
};
struct SUnit
{
  bool en2D, enCube, en3D;
  STexEnv env;
  bool gen[4];
  GLenum genMode[4];
  float objPlane[4][4], eyePlane[4][4];
};
static SUnit s_unit[MAX_UNITS];
static GLuint s_bound2D[MAX_UNITS], s_boundCube[MAX_UNITS];
static std::unordered_map<GLuint, unsigned> s_texUploadFrame; // diagnostic (slow-draw log): frame of last upload per texture
static unsigned s_diagFrame = 0;
static inline void NoteTexUpload() { static const bool s_on = getenv("FARCRY_GLES3_SLOW_DRAW_MS") != NULL; if (s_on) { const int u = s_activeUnit < MAX_UNITS ? s_activeUnit : 0; s_texUploadFrame[s_bound2D[u]] = s_diagFrame; } }

struct SLight { bool on; float pos[4], amb[4], diff[4], spec[4], att[3]; };
static SLight s_light[MAX_LIGHTS];
static float s_matAmb[4] = {0.2f,0.2f,0.2f,1}, s_matDiff[4] = {0.8f,0.8f,0.8f,1}, s_matEmis[4] = {0,0,0,1};
static float s_sceneAmb[4] = {0.2f,0.2f,0.2f,1};
static bool s_lighting = false, s_colorMaterial = false, s_alphaTest = false, s_fog = false;
static GLenum s_alphaFunc = GL_ALWAYS; static float s_alphaRef = 0;
static GLenum s_fogMode = D_EXP; static float s_fogStart = 0, s_fogEnd = 1, s_fogDensity = 1, s_fogColor[4] = {0,0,0,0};
static bool s_stateDirty = true;

// Per-texture info needed at upload time.

///////////////////////////////////////////////////////////////////////////////
// Program generation
///////////////////////////////////////////////////////////////////////////////
#pragma pack(push, 1)
struct SKey
{
  uint8_t tex[MAX_UNITS];         // 0 off, 1 2D, 2 cube
  uint8_t mode[MAX_UNITS];        // 0 modulate 1 replace 2 decal 3 add 4 blend 5 combine
  uint8_t comb[MAX_UNITS][2];     // rgb / alpha function
  uint8_t src[MAX_UNITS][6];      // rgb0..2, a0..2
  uint8_t op[MAX_UNITS][6];
  uint8_t gen[MAX_UNITS][4];      // 0 none 1 object 2 eye 3 sphere 4 reflection 5 normal
  uint8_t texMat[MAX_UNITS];
  uint8_t alphaFunc;              // 0 off, else GL func - GL_NEVER + 1
  uint8_t fog;                    // 0 off 1 linear 2 exp 3 exp2
  uint8_t lights;                 // 0 off, else 1 + number of active lights
  uint8_t colorMaterial;
  uint8_t colorBrightness;        // primary colour = vec4(colour.aaa, 1)
  uint8_t audit;                  // diagnostic: output uAuditColor
};
#pragma pack(pop)

// Change counters: a program re-uploads a uniform group only when the
// counter differs from the value it last saw (avoids per-draw uploads).
static uint32_t s_verMV = 1, s_verP = 1, s_verAlpha = 1, s_verFog = 1, s_verLight = 1;
static uint32_t s_verTex[MAX_UNITS] = {1,1,1,1}, s_verEnv[MAX_UNITS] = {1,1,1,1}, s_verGen[MAX_UNITS] = {1,1,1,1};

struct SProgram
{
  uint32_t vMV, vP, vAlpha, vFog, vLight, vTex[MAX_UNITS], vEnv[MAX_UNITS], vGen[MAX_UNITS];
  bool samplersSet;
  GLuint prog;
  GLint uMV, uP, uNormalMat, uTexMat[MAX_UNITS], uTex[MAX_UNITS], uEnvColor[MAX_UNITS], uEnvScale[MAX_UNITS];
  GLint uAlphaRef, uFogParams, uFogColor, uGenObj[MAX_UNITS], uGenEye[MAX_UNITS];
  GLint uLightPos, uLightAmb, uLightDiff, uLightAtt, uMatAmb, uMatDiff, uMatEmis, uSceneAmb;
  GLint uAuditColor;
};

struct SKeyHash { size_t operator()(const std::string& s) const { return std::hash<std::string>()(s); } };
static std::unordered_map<std::string, SProgram> s_programs;
static SProgram* s_curProg = 0;
static SKey s_curKeyStruct;
static GLuint s_boundProg = 0;

static uint8_t CombIndex(GLenum f)
{
  switch (f)
  {
    case D_REPLACE: return 0; case D_MODULATE: return 1; case D_ADD: return 2; case D_ADD_SIGNED: return 3;
    case D_INTERPOLATE: return 4; case D_SUBTRACT: return 5;
    case D_DOT3_RGB: case D_DOT3_RGB_EXT: return 6; case D_DOT3_RGBA: case D_DOT3_RGBA_EXT: return 7;
  }
  return 1;
}
static uint8_t SrcIndex(GLenum s)
{
  if (s >= D_TEXTURE0 && s < D_TEXTURE0 + MAX_UNITS) return (uint8_t)(4 + (s - D_TEXTURE0));
  switch (s) { case D_TEXTURE: return 0; case D_CONSTANT: return 1; case D_PRIMARY_COLOR: return 2; case D_PREVIOUS: return 3; }
  return 3;
}
static uint8_t OpIndex(GLenum o)
{
  switch (o) { case D_SRC_COLOR: return 0; case D_ONE_MINUS_SRC_COLOR: return 1; case D_SRC_ALPHA: return 2; case D_ONE_MINUS_SRC_ALPHA: return 3; }
  return 0;
}
static uint8_t GenIndex(GLenum m)
{
  switch (m) { case D_OBJECT_LINEAR: return 1; case D_EYE_LINEAR: return 2; case D_SPHERE_MAP: return 3; case D_REFLECTION_MAP: return 4; case D_NORMAL_MAP: return 5; }
  return 1;
}

static int ActiveLightCount()
{
  int n = 0;
  for (int i = 0; i < MAX_LIGHTS; ++i) if (s_light[i].on) ++n;
  return n;
}

static int s_dumpState = 0; // 0 waiting, 1 dumping this frame, 2 done
static bool s_audit = false;
static unsigned s_auditId = 0;

static SKey BuildKey()
{
  SKey k; memset(&k, 0, sizeof(k));
  for (int u = 0; u < MAX_UNITS; ++u)
  {
    const SUnit& U = s_unit[u];
    k.tex[u] = U.enCube ? 2 : (U.en2D ? 1 : 0);
    if (!k.tex[u]) continue;
    const STexEnv& e = U.env;
    switch (e.mode) { case D_REPLACE: k.mode[u] = 1; break; case D_DECAL: k.mode[u] = 2; break; case D_ADD: k.mode[u] = 3; break;
                      case D_BLEND: k.mode[u] = 4; break; case D_COMBINE: k.mode[u] = 5; break; default: k.mode[u] = 0; }
    if (k.mode[u] == 5)
    {
      k.comb[u][0] = CombIndex(e.combRGB); k.comb[u][1] = CombIndex(e.combA);
      for (int i = 0; i < 3; ++i)
      {
        k.src[u][i] = SrcIndex(e.srcRGB[i]); k.src[u][3+i] = SrcIndex(e.srcA[i]);
        k.op[u][i] = OpIndex(e.opRGB[i]); k.op[u][3+i] = OpIndex(e.opA[i]);
      }
    }
    for (int c = 0; c < 4; ++c)
      k.gen[u][c] = U.gen[c] ? GenIndex(U.genMode[c]) : 0;
    k.texMat[u] = MatIsIdentity(TexMatrix(u)) ? 0 : 1;
  }
  static const bool s_noAlphaTest = getenv("FARCRY_GLES3_NOALPHATEST") != NULL; // diagnostic only
  if (s_alphaTest && s_alphaFunc != GL_ALWAYS && !s_noAlphaTest) k.alphaFunc = (uint8_t)(s_alphaFunc - GL_NEVER + 1);
  if (s_fog) k.fog = s_fogMode == D_LINEAR_FOG ? 1 : (s_fogMode == D_EXP2 ? 3 : 2);
  if (s_lighting) k.lights = (uint8_t)(1 + ActiveLightCount());
  k.colorMaterial = s_lighting && s_colorMaterial;
  k.colorBrightness = s_colorArr.on ? (uint8_t)s_colorArr.semantic : 0;
  k.audit = (s_audit && s_dumpState == 1) ? 1 : 0;
  // Diagnostic: FARCRY_GLES3_FLAT=1 draws every pixel in a constant colour
  // (no texture sampling, no per-pixel shading, no discard): GPU cost bisection.
  static const bool s_flat = getenv("FARCRY_GLES3_FLAT") != NULL;
  if (s_flat) { k.audit = 1; k.alphaFunc = 0; }
  return k;
}

static void AppendArg(std::string& fs, int u, uint8_t src, uint8_t op, bool alpha)
{
  std::string v;
  if (src == 0) { char b[16]; snprintf(b, sizeof(b), "t%d", u); v = b; }
  else if (src == 1) { char b[24]; snprintf(b, sizeof(b), "uEnvColor%d", u); v = b; }
  else if (src == 2) v = "vColor";
  else if (src == 3) v = "prev";
  else { char b[16]; snprintf(b, sizeof(b), "t%d", src - 4); v = b; }
  const char* sw = alpha ? ".a" : ".rgb";
  switch (op)
  {
    case 0: fs += v + sw; break;
    case 1: fs += std::string("(1.0 - ") + v + sw + ")"; break;
    case 2: fs += alpha ? v + ".a" : std::string("vec3(") + v + ".a)"; break;
    case 3: fs += alpha ? std::string("(1.0 - ") + v + ".a)" : std::string("vec3(1.0 - ") + v + ".a)"; break;
  }
}

static void AppendCombine(std::string& fs, int u, const SKey& k, bool alpha)
{
  const uint8_t f = k.comb[u][alpha ? 1 : 0];
  const int o = alpha ? 3 : 0;
  std::string a0, a1, a2;
  AppendArg(a0, u, k.src[u][o+0], k.op[u][o+0], alpha);
  AppendArg(a1, u, k.src[u][o+1], k.op[u][o+1], alpha);
  AppendArg(a2, u, k.src[u][o+2], k.op[u][o+2], alpha);
  switch (f)
  {
    case 0: fs += a0; break;
    case 1: fs += "(" + a0 + " * " + a1 + ")"; break;
    case 2: fs += "(" + a0 + " + " + a1 + ")"; break;
    case 3: fs += "(" + a0 + " + " + a1 + " - 0.5)"; break;
    case 4: fs += "mix(" + a1 + ", " + a0 + ", " + a2 + ")"; break;
    case 5: fs += "(" + a0 + " - " + a1 + ")"; break;
    default:
      fs += alpha ? std::string("1.0")
                  : "vec3(4.0 * dot(" + a0 + " - 0.5, " + a1 + " - 0.5))";
      break;
  }
}

static GLuint CompileStage(GLenum type, const std::string& src)
{
  GLuint sh = r_glCreateShader(type);
  const char* p = src.c_str();
  r_glShaderSource(sh, 1, &p, NULL);
  r_glCompileShader(sh);
  GLint ok = 0; r_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[2048]; log[0] = 0; r_glGetShaderInfoLog(sh, sizeof(log), NULL, log);
    FFPLog("GLES3FFP: %s shader compile failed: %s", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
    FFPLog("GLES3FFP: source:\n%s", src.c_str());
  }
  return sh;
}

// Persistent program cache (FARCRY_PROGCACHE, default <FARCRY_TEXCACHE>/../programs):
// linked program binaries keyed by generated source + driver, and an index of
// state keys so every program seen before is rebuilt at level load instead of
// being compiled mid-game (Mali compiles cost hundreds of ms: visible hitches).
static std::string s_progCacheDir;
static bool s_progCacheInit = false;
static unsigned s_progFromCache = 0, s_progCompiled = 0;
static const std::string& ProgCacheDir()
{
  if (!s_progCacheInit)
  {
    s_progCacheInit = true;
    if (const char* d = getenv("FARCRY_PROGCACHE")) s_progCacheDir = d;
    else if (const char* t = getenv("FARCRY_TEXCACHE")) s_progCacheDir = std::string(t) + "/../programs";
    if (!r_glProgramBinary || !r_glGetProgramBinary) s_progCacheDir.clear();
    if (!s_progCacheDir.empty()) mkdir(s_progCacheDir.c_str(), 0755);
  }
  return s_progCacheDir;
}
static std::string ProgCachePath(const std::string& vs, const std::string& fs)
{
  const std::string& dir = ProgCacheDir();
  if (dir.empty()) return std::string();
  uint64_t h = 1469598103934665603ULL;
  auto mix = [&h](const char* p, size_t n) { for (size_t i = 0; i < n; ++i) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; } };
  mix(vs.data(), vs.size()); mix(fs.data(), fs.size());
  const char* r = (const char*)r_glGetString(GL_RENDERER); const char* v = (const char*)r_glGetString(GL_VERSION);
  if (r) mix(r, strlen(r)); if (v) mix(v, strlen(v));
  char name[64]; snprintf(name, sizeof(name), "/%016llx.bin", (unsigned long long)h);
  return dir + name;
}
static bool LoadProgramBinary(GLuint prog, const std::string& path)
{
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  uint32_t fmt = 0; std::vector<char> data;
  bool ok = fread(&fmt, 4, 1, f) == 1;
  if (ok) { fseek(f, 0, SEEK_END); long n = ftell(f) - 4; fseek(f, 4, SEEK_SET); ok = n > 0; if (ok) { data.resize(n); ok = fread(data.data(), 1, n, f) == (size_t)n; } }
  fclose(f);
  if (!ok) return false;
  r_glProgramBinary(prog, fmt, data.data(), (GLsizei)data.size());
  GLint linked = 0; r_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
  if (!linked) remove(path.c_str()); // stale (driver update): rebuild
  return linked != 0;
}
static void SaveProgramBinary(GLuint prog, const std::string& path)
{
  GLint len = 0; r_glGetProgramiv(prog, GL_PROGRAM_BINARY_LENGTH, &len);
  if (len <= 0) return;
  std::vector<char> data(len); GLenum fmt = 0; GLsizei got = 0;
  r_glGetProgramBinary(prog, len, &got, &fmt, data.data());
  if (got <= 0) return;
  const std::string tmp = path + ".tmp";
  if (FILE* f = fopen(tmp.c_str(), "wb"))
  {
    const uint32_t f32 = fmt;
    const bool ok = fwrite(&f32, 4, 1, f) == 1 && fwrite(data.data(), 1, got, f) == (size_t)got;
    fclose(f);
    if (ok) rename(tmp.c_str(), path.c_str()); else remove(tmp.c_str());
  }
}
static void RecordProgramKey(const SKey& k)
{
  if (k.audit) return;
  if (FILE* f = fopen((ProgCacheDir() + "/keys.bin").c_str(), "ab")) { fwrite(&k, sizeof(k), 1, f); fclose(f); }
}

static unsigned s_progBuiltFrame = 0;
static SProgram BuildProgram(const SKey& k)
{
  ++s_progBuiltFrame;
  std::string vs = "#version 300 es\nprecision highp float;\n"
    "in vec4 aPos; in vec4 aColor; in vec3 aNormal; in vec4 aTex0; in vec4 aTex1; in vec4 aTex2; in vec4 aTex3;\n"
    "uniform mat4 uMV; uniform mat4 uP; uniform mat3 uNormalMat;\n"
    "out VP vec4 vColor; out float vFogDist;\n";
  // Diagnostic: FARCRY_GLES3_MEDIUMP_VARYINGS=1 passes colour and texture
  // coordinates at half precision (less per-vertex data for tile-based GPUs).
  static const char* s_vp = getenv("FARCRY_GLES3_MEDIUMP_VARYINGS") ? "#define VP mediump\n" : "#define VP highp\n";
  vs.insert(vs.find('\n', vs.find("precision")) + 1, s_vp);
  std::string fs = std::string("#version 300 es\nprecision highp float;\n") + s_vp + "in VP vec4 vColor; in float vFogDist;\nout vec4 oColor;\n";
  char b[256];
  bool needNormal = k.lights != 0;
  for (int u = 0; u < MAX_UNITS; ++u)
  {
    if (!k.tex[u]) continue;
    snprintf(b, sizeof(b), "uniform mat4 uTexMat%d; uniform mat4 uGenObj%d; uniform mat4 uGenEye%d; out VP vec4 vTex%d;\n", u, u, u, u); vs += b;
    snprintf(b, sizeof(b), "in VP vec4 vTex%d; uniform %s uTex%d; uniform vec4 uEnvColor%d; uniform vec2 uEnvScale%d;\n",
             u, k.tex[u] == 2 ? "samplerCube" : "sampler2D", u, u, u); fs += b;
    for (int c = 0; c < 4; ++c) if (k.gen[u][c] >= 3) needNormal = true;
  }
  if (k.lights)
    vs += "uniform vec4 uLightPos[4]; uniform vec4 uLightAmb[4]; uniform vec4 uLightDiff[4]; uniform vec3 uLightAtt[4];\n"
          "uniform vec4 uMatAmb; uniform vec4 uMatDiff; uniform vec4 uMatEmis; uniform vec4 uSceneAmb;\n";
  if (k.alphaFunc) fs += "uniform float uAlphaRef;\n";
  if (k.fog) fs += "uniform vec4 uFogParams; uniform vec4 uFogColor;\n";

  vs += "void main()\n{\n  vec4 eye = uMV * aPos;\n  gl_Position = uP * eye;\n  vFogDist = abs(eye.z);\n";
  vs += k.colorBrightness == 1 ? "  vec4 inColor = vec4(aColor.aaa * 2.0, 1.0); // terrain: original Modulate2X overbright\n"
      : (k.colorBrightness == 2 ? "  vec4 inColor = vec4(min(aColor.rgb * 2.0, vec3(1.0)), 1.0);\n" : "  vec4 inColor = aColor;\n");
  // Semantic 2: UpdateCustomLighting stores half-intensity light (overbright
  // compensation); the original vegetation pass doubled it.
  if (needNormal) vs += "  vec3 N = normalize(uNormalMat * aNormal);\n";
  if (k.lights)
  {
    vs += k.colorMaterial ? "  vec4 mAmb = inColor; vec4 mDiff = inColor;\n" : "  vec4 mAmb = uMatAmb; vec4 mDiff = uMatDiff;\n";
    vs += "  vec3 c = uMatEmis.rgb + uSceneAmb.rgb * mAmb.rgb;\n";
    snprintf(b, sizeof(b), "  for (int i = 0; i < %d; ++i) {\n", k.lights - 1); vs += b;
    vs += "    vec3 L; float att = 1.0;\n"
          "    if (uLightPos[i].w == 0.0) L = normalize(uLightPos[i].xyz);\n"
          "    else { vec3 d = uLightPos[i].xyz - eye.xyz; float dl = length(d); L = d / max(dl, 1e-6);\n"
          "           att = 1.0 / max(uLightAtt[i].x + uLightAtt[i].y * dl + uLightAtt[i].z * dl * dl, 1e-6); }\n"
          "    c += att * (uLightAmb[i].rgb * mAmb.rgb + max(dot(N, L), 0.0) * uLightDiff[i].rgb * mDiff.rgb);\n"
          "  }\n"
          "  vColor = vec4(clamp(c, 0.0, 1.0), mDiff.a);\n";
  }
  else
    vs += "  vColor = inColor;\n";
  for (int u = 0; u < MAX_UNITS; ++u)
  {
    if (!k.tex[u]) continue;
    snprintf(b, sizeof(b), "  vec4 tc%d = aTex%d;\n", u, u); vs += b;
    bool anyGen = false;
    for (int c = 0; c < 4; ++c) if (k.gen[u][c]) anyGen = true;
    if (anyGen)
    {
      if (needNormal) vs += "  vec3 R = reflect(normalize(eye.xyz), N);\n";
      static const char comp[4] = {'x','y','z','w'};
      for (int c = 0; c < 4; ++c)
      {
        switch (k.gen[u][c])
        {
          case 1: snprintf(b, sizeof(b), "  tc%d.%c = dot(aPos, uGenObj%d[%d]);\n", u, comp[c], u, c); break;
          case 2: snprintf(b, sizeof(b), "  tc%d.%c = dot(eye, uGenEye%d[%d]);\n", u, comp[c], u, c); break;
          case 3: snprintf(b, sizeof(b), "  { float m = 2.0 * sqrt(R.x*R.x + R.y*R.y + (R.z+1.0)*(R.z+1.0)); tc%d.%c = %s; }\n",
                           u, comp[c], c == 0 ? "R.x / m + 0.5" : (c == 1 ? "R.y / m + 0.5" : "0.0")); break;
          case 4: snprintf(b, sizeof(b), "  tc%d.%c = R.%c;\n", u, comp[c], comp[c < 3 ? c : 2]); break;
          case 5: snprintf(b, sizeof(b), "  tc%d.%c = N.%c;\n", u, comp[c], comp[c < 3 ? c : 2]); break;
          default: b[0] = 0;
        }
        vs += b;
      }
    }
    if (k.texMat[u]) { snprintf(b, sizeof(b), "  tc%d = uTexMat%d * tc%d;\n", u, u, u); vs += b; }
    snprintf(b, sizeof(b), "  vTex%d = tc%d;\n", u, u); vs += b;
  }
  vs += "}\n";

  fs += "void main()\n{\n  vec4 prev = vColor;\n";
  for (int u = 0; u < MAX_UNITS; ++u)
  {
    if (!k.tex[u]) continue;
    if (k.tex[u] == 2 && getenv("FARCRY_GLES3_CUBEWHITE")) snprintf(b, sizeof(b), "  vec4 t%d = vec4(1.0); // diagnostic\n", u);
    else if (k.tex[u] == 2) snprintf(b, sizeof(b), "  vec4 t%d = texture(uTex%d, vTex%d.xyz);\n", u, u, u);
    else snprintf(b, sizeof(b), "  vec4 t%d = textureProj(uTex%d, vTex%d.xyw);\n", u, u, u);
    fs += b;
  }
  for (int u = 0; u < MAX_UNITS; ++u)
  {
    if (!k.tex[u]) continue;
    char t[8]; snprintf(t, sizeof(t), "t%d", u);
    switch (k.mode[u])
    {
      case 0: fs += std::string("  prev = prev * ") + t + ";\n"; break;
      case 1: fs += std::string("  prev = ") + t + ";\n"; break;
      case 2: fs += std::string("  prev.rgb = mix(prev.rgb, ") + t + ".rgb, " + t + ".a);\n"; break;
      case 3: fs += std::string("  prev.rgb += ") + t + ".rgb; prev.a *= " + t + ".a;\n"; break;
      case 4: snprintf(b, sizeof(b), "  prev.rgb = mix(prev.rgb, uEnvColor%d.rgb, %s.rgb); prev.a *= %s.a;\n", u, t, t); fs += b; break;
      case 5:
      {
        fs += "  {\n    vec3 r = ";
        AppendCombine(fs, u, k, false);
        fs += ";\n    float a = ";
        AppendCombine(fs, u, k, true);
        snprintf(b, sizeof(b), ";\n    prev = clamp(vec4(r * uEnvScale%d.x, a * uEnvScale%d.y), 0.0, 1.0);\n", u, u); fs += b;
        if (k.comb[u][0] == 7) fs += "    prev.a = prev.r;\n";
        fs += "  }\n";
        break;
      }
    }
  }
  if (k.alphaFunc)
  {
    static const char* const cmp[8] = { "false", "prev.a < uAlphaRef", "prev.a == uAlphaRef", "prev.a <= uAlphaRef",
                                         "prev.a > uAlphaRef", "prev.a != uAlphaRef", "prev.a >= uAlphaRef", "true" };
    snprintf(b, sizeof(b), "  if (!(%s)) discard;\n", cmp[(k.alphaFunc - 1) & 7]); fs += b;
  }
  if (k.fog == 1) fs += "  float f = clamp((uFogParams.y - vFogDist) * uFogParams.z, 0.0, 1.0);\n";
  else if (k.fog == 2) fs += "  float f = clamp(exp(-uFogParams.w * vFogDist), 0.0, 1.0);\n";
  else if (k.fog == 3) fs += "  float f = clamp(exp(-(uFogParams.w * vFogDist) * (uFogParams.w * vFogDist)), 0.0, 1.0);\n";
  if (k.fog) fs += "  prev.rgb = mix(uFogColor.rgb, prev.rgb, f);\n";
  if (k.audit) fs = std::string("#version 300 es\nprecision highp float;\nuniform vec4 uAuditColor;\nout vec4 oColor;\n") +
                    "void main()\n{\n" + (k.alphaFunc ? "" : "") + "  oColor = uAuditColor;\n}\n";
  else if (getenv("FARCRY_GLES3_VCOLORONLY")) fs += "  oColor = vec4(vColor.rgb, 1.0); // diagnostic: interpolated primary colour\n}\n";
  else if (getenv("FARCRY_GLES3_TCONLY") && k.tex[0]) fs += "  vec2 tcd = vTex0.xy / vTex0.w; oColor = vec4(clamp(tcd, 0.0, 1.0), (tcd.x < 0.0 || tcd.y < 0.0 || tcd.x > 1.0 || tcd.y > 1.0) ? 1.0 : 0.0, 1.0); // diagnostic\n}\n";
  else if (getenv("FARCRY_GLES3_ALPHAONLY") && k.tex[0]) fs += "  oColor = vec4(vec3(t0.a), 1.0); // diagnostic: unit-0 alpha\n}\n";
  else if (getenv("FARCRY_GLES3_TEXONLY") && k.tex[0]) fs += "  oColor = vec4(t0.rgb, 1.0); // diagnostic: unit-0 texture only\n}\n";
  else fs += "  oColor = prev;\n}\n";

  if (getenv("FARCRY_GLES3_DUMP_SHADERS"))
    fprintf(stderr, "GLES3FFP SHADER #%d\n--- VS ---\n%s--- FS ---\n%s--- END ---\n", (int)s_programs.size() + 1, vs.c_str(), fs.c_str());
  SProgram p; memset(&p, 0, sizeof(p));
  p.prog = r_glCreateProgram();
  GLint ok = 0;
  const std::string binPath = ProgCachePath(vs, fs);
  if (!binPath.empty() && LoadProgramBinary(p.prog, binPath))
    ok = 1, ++s_progFromCache;
  else
  {
    GLuint v = CompileStage(GL_VERTEX_SHADER, vs), f = CompileStage(GL_FRAGMENT_SHADER, fs);
    r_glAttachShader(p.prog, v); r_glAttachShader(p.prog, f);
    r_glBindAttribLocation(p.prog, ATTR_POS, "aPos");
    r_glBindAttribLocation(p.prog, ATTR_COLOR, "aColor");
    r_glBindAttribLocation(p.prog, ATTR_NORMAL, "aNormal");
    for (int u = 0; u < MAX_UNITS; ++u) { snprintf(b, sizeof(b), "aTex%d", u); r_glBindAttribLocation(p.prog, ATTR_TEX0 + u, b); }
    if (!binPath.empty() && r_glProgramParameteri) r_glProgramParameteri(p.prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    r_glLinkProgram(p.prog);
    r_glDeleteShader(v); r_glDeleteShader(f);
    r_glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
      char log[2048]; log[0] = 0; r_glGetProgramInfoLog(p.prog, sizeof(log), NULL, log);
      FFPLog("GLES3FFP: program link failed: %s", log);
    }
    else if (!binPath.empty())
    {
      SaveProgramBinary(p.prog, binPath);
      RecordProgramKey(k);
    }
    ++s_progCompiled;
  }
  p.uMV = r_glGetUniformLocation(p.prog, "uMV");
  p.uP = r_glGetUniformLocation(p.prog, "uP");
  p.uNormalMat = r_glGetUniformLocation(p.prog, "uNormalMat");
  for (int u = 0; u < MAX_UNITS; ++u)
  {
    snprintf(b, sizeof(b), "uTexMat%d", u); p.uTexMat[u] = r_glGetUniformLocation(p.prog, b);
    snprintf(b, sizeof(b), "uTex%d", u); p.uTex[u] = r_glGetUniformLocation(p.prog, b);
    snprintf(b, sizeof(b), "uEnvColor%d", u); p.uEnvColor[u] = r_glGetUniformLocation(p.prog, b);
    snprintf(b, sizeof(b), "uEnvScale%d", u); p.uEnvScale[u] = r_glGetUniformLocation(p.prog, b);
    snprintf(b, sizeof(b), "uGenObj%d", u); p.uGenObj[u] = r_glGetUniformLocation(p.prog, b);
    snprintf(b, sizeof(b), "uGenEye%d", u); p.uGenEye[u] = r_glGetUniformLocation(p.prog, b);
  }
  p.uAlphaRef = r_glGetUniformLocation(p.prog, "uAlphaRef");
  p.uFogParams = r_glGetUniformLocation(p.prog, "uFogParams");
  p.uFogColor = r_glGetUniformLocation(p.prog, "uFogColor");
  p.uLightPos = r_glGetUniformLocation(p.prog, "uLightPos");
  p.uLightAmb = r_glGetUniformLocation(p.prog, "uLightAmb");
  p.uLightDiff = r_glGetUniformLocation(p.prog, "uLightDiff");
  p.uLightAtt = r_glGetUniformLocation(p.prog, "uLightAtt");
  p.uMatAmb = r_glGetUniformLocation(p.prog, "uMatAmb");
  p.uMatDiff = r_glGetUniformLocation(p.prog, "uMatDiff");
  p.uMatEmis = r_glGetUniformLocation(p.prog, "uMatEmis");
  p.uSceneAmb = r_glGetUniformLocation(p.prog, "uSceneAmb");
  p.uAuditColor = r_glGetUniformLocation(p.prog, "uAuditColor");
  if (s_programs.size() < 8 || (s_programs.size() % 64) == 0)
    FFPLog("GLES3FFP: built program #%d", (int)s_programs.size() + 1);
  return p;
}

static void ApplyProgram()
{
  if (s_stateDirty || !s_curProg)
  {
    SKey k = BuildKey();
    if (!s_curProg || memcmp(&k, &s_curKeyStruct, sizeof(k)) != 0)
    {
      std::string key((const char*)&k, sizeof(k));
      auto it = s_programs.find(key);
      if (it == s_programs.end())
        it = s_programs.emplace(key, BuildProgram(k)).first;
      s_curProg = &it->second;
      s_curKeyStruct = k;
    }
    s_stateDirty = false;
  }
  SProgram& p = *s_curProg;
  static const bool s_noUniformCache = getenv("FARCRY_GLES3_NOUNIFORMCACHE") != NULL; // diagnostic
  if (s_noUniformCache)
  {
    p.vMV = p.vP = p.vAlpha = p.vFog = p.vLight = 0;
    for (int u = 0; u < MAX_UNITS; ++u) p.vTex[u] = p.vEnv[u] = p.vGen[u] = 0;
  }
  if (s_boundProg != p.prog) { r_glUseProgram(p.prog); s_boundProg = p.prog; }
  if (p.vMV != s_verMV)
  {
    r_glUniformMatrix4fv(p.uMV, 1, GL_FALSE, Modelview());
    if (p.uNormalMat >= 0)
    {
      float inv[16]; MatInverse(inv, Modelview());
      float n[9] = { inv[0], inv[4], inv[8], inv[1], inv[5], inv[9], inv[2], inv[6], inv[10] }; // transpose of inverse
      r_glUniformMatrix3fv(p.uNormalMat, 1, GL_FALSE, n);
    }
    p.vMV = s_verMV;
  }
  if (p.vP != s_verP) { r_glUniformMatrix4fv(p.uP, 1, GL_FALSE, Projection()); p.vP = s_verP; }
  for (int u = 0; u < MAX_UNITS; ++u)
  {
    // A stage that only combines PREVIOUS/CONSTANT has its sampler optimised
    // out (location -1) but still needs its env colour and scale.
    if (p.uTex[u] >= 0 && !p.samplersSet) r_glUniform1i(p.uTex[u], u);
    if (p.uTexMat[u] >= 0 && p.vTex[u] != s_verTex[u]) { r_glUniformMatrix4fv(p.uTexMat[u], 1, GL_FALSE, TexMatrix(u)); p.vTex[u] = s_verTex[u]; }
    if (p.vEnv[u] != s_verEnv[u])
    {
      if (p.uEnvColor[u] >= 0) r_glUniform4fv(p.uEnvColor[u], 1, s_unit[u].env.color);
      if (p.uEnvScale[u] >= 0) { float sc[2] = { s_unit[u].env.scaleRGB, s_unit[u].env.scaleA }; r_glUniform2fv(p.uEnvScale[u], 1, sc); }
      p.vEnv[u] = s_verEnv[u];
    }
    if (p.vGen[u] != s_verGen[u])
    {
      // Planes are stored one per row; uploaded untransposed so that column
      // uGen*[i] in GLSL is plane i (dot(pos, uGenObj0[0]) = S).
      if (p.uGenObj[u] >= 0) r_glUniformMatrix4fv(p.uGenObj[u], 1, GL_FALSE, &s_unit[u].objPlane[0][0]);
      if (p.uGenEye[u] >= 0) r_glUniformMatrix4fv(p.uGenEye[u], 1, GL_FALSE, &s_unit[u].eyePlane[0][0]);
      p.vGen[u] = s_verGen[u];
    }
  }
  p.samplersSet = true;
  if (p.uAuditColor >= 0)
  {
    const unsigned id = s_auditId; // id+1 encoded in RGB so 0 means "no draw"
    float c[4] = { ((id + 1) & 255) / 255.0f, (((id + 1) >> 8) & 255) / 255.0f, (((id + 1) >> 16) & 255) / 255.0f, 1.0f };
    r_glUniform4fv(p.uAuditColor, 1, c);
  }
  if (p.uAlphaRef >= 0 && p.vAlpha != s_verAlpha) { r_glUniform1f(p.uAlphaRef, s_alphaRef); p.vAlpha = s_verAlpha; }
  if (p.uFogParams >= 0 && p.vFog != s_verFog)
  {
    float range = s_fogEnd - s_fogStart;
    float fp[4] = { s_fogStart, s_fogEnd, range != 0.0f ? 1.0f / range : 0.0f, s_fogDensity };
    r_glUniform4fv(p.uFogParams, 1, fp);
    r_glUniform4fv(p.uFogColor, 1, s_fogColor);
    p.vFog = s_verFog;
  }
  if (p.uLightPos >= 0 && p.vLight != s_verLight)
  {
    float pos[MAX_LIGHTS*4], amb[MAX_LIGHTS*4], diff[MAX_LIGHTS*4], att[MAX_LIGHTS*3];
    int n = 0;
    for (int i = 0; i < MAX_LIGHTS; ++i)
    {
      if (!s_light[i].on) continue;
      memcpy(&pos[n*4], s_light[i].pos, 16); memcpy(&amb[n*4], s_light[i].amb, 16);
      memcpy(&diff[n*4], s_light[i].diff, 16); memcpy(&att[n*3], s_light[i].att, 12);
      ++n;
    }
    if (n)
    {
      r_glUniform4fv(p.uLightPos, n, pos); r_glUniform4fv(p.uLightAmb, n, amb); r_glUniform4fv(p.uLightDiff, n, diff);
      if (p.uLightAtt >= 0) r_glUniform3fv(p.uLightAtt, n, att);
    }
    r_glUniform4fv(p.uMatAmb, 1, s_matAmb); r_glUniform4fv(p.uMatDiff, 1, s_matDiff);
    r_glUniform4fv(p.uMatEmis, 1, s_matEmis); r_glUniform4fv(p.uSceneAmb, 1, s_sceneAmb);
    p.vLight = s_verLight;
  }
}


///////////////////////////////////////////////////////////////////////////////
// Vertex attributes for a draw
///////////////////////////////////////////////////////////////////////////////
static bool s_attrOn[NUM_ATTRS];

struct SAttrCache { bool valid, array; GLuint buffer; const void* ptr; GLint size; GLenum type; GLsizei stride; GLboolean norm; float c[4]; };
static SAttrCache s_attrCache[NUM_ATTRS];
static void InvalidateAttr(GLuint attr) { if (attr < NUM_ATTRS) s_attrCache[attr].valid = false; }

static void SetAttr(int attr, const SClientArray* a, bool normalizeBytes, const float* constant)
{
  SAttrCache& c = s_attrCache[attr];
  if (a && a->on && a->type != D_DOUBLE)
  {
    const GLint size = a->size == 0x80E1 ? 4 : a->size;
    const GLboolean norm = (normalizeBytes && (a->type == GL_UNSIGNED_BYTE || a->type == GL_BYTE)) ? GL_TRUE : GL_FALSE;
    if (!(c.valid && c.array && c.buffer == a->buffer && c.ptr == a->ptr && c.size == size && c.type == a->type && c.stride == a->stride && c.norm == norm))
    {
      if (a->buffer != s_arrayBuffer) BindReal(GL_ARRAY_BUFFER, a->buffer);
      r_glVertexAttribPointer(attr, size, a->type, norm, a->stride, a->ptr);
      if (a->buffer != s_arrayBuffer) BindReal(GL_ARRAY_BUFFER, s_arrayBuffer);
      c.valid = true; c.array = true; c.buffer = a->buffer; c.ptr = a->ptr; c.size = size; c.type = a->type; c.stride = a->stride; c.norm = norm;
    }
    if (!s_attrOn[attr]) { r_glEnableVertexAttribArray(attr); s_attrOn[attr] = true; }
  }
  else
  {
    if (s_attrOn[attr]) { r_glDisableVertexAttribArray(attr); s_attrOn[attr] = false; }
    if (constant && !(c.valid && !c.array && memcmp(c.c, constant, 16) == 0))
    {
      r_glVertexAttrib4f(attr, constant[0], constant[1], constant[2], constant[3]);
      c.valid = true; c.array = false; memcpy(c.c, constant, 16);
    }
  }
}

// Engine-side generic attribute calls bypass the cache: keep it truthful.
static void APIENTRY f_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr)
{ InvalidateAttr(i); r_glVertexAttribPointer(i, size, type, norm, stride, ptr); }
static void APIENTRY f_glEnableVertexAttribArray(GLuint i) { if (i < NUM_ATTRS) s_attrOn[i] = true; InvalidateAttr(i); r_glEnableVertexAttribArray(i); }
static void APIENTRY f_glDisableVertexAttribArray(GLuint i) { if (i < NUM_ATTRS) s_attrOn[i] = false; InvalidateAttr(i); r_glDisableVertexAttribArray(i); }

// FARCRY_GLES3_DUMP_FRAME=<seconds>: log every fixed-function draw of the first
// frame after that time (colour source, lighting, texture units, alpha test).
static Uint32 s_dumpStartTicks = 0;
static unsigned s_lastFrameDraws = 0;
static unsigned s_frameCounter = 1; // frames presented (big-buffer orphaning) // previous frame, for FARCRY_GLES3_DUMP_MIN_DRAWS
static void (*s_auditHook)() = NULL;
void GLES3FFPSetAuditHook(void (*hook)()) { s_auditHook = hook; }
static bool s_dumpRequested = false; // set by the SIGUSR2 snapshot (live debugging)
void GLES3FFPRequestDump() { s_dumpRequested = true; }
// GPU profiler state (FARCRY_GPU_PROFILE, see SDrawTick).
static bool s_gpuProfFrame = false;
static Uint64 s_gpuProfLastEnd = 0;
static std::map<std::string, std::pair<double, int> > s_gpuProf;
static double GpuProfMs(Uint64 a, Uint64 b) { return (double)(b - a) * 1000.0 / (double)SDL_GetPerformanceFrequency(); }
void GLES3FFPFrameBoundary()
{
  ++s_frameCounter; ++s_diagFrame;
  {
    static const bool s_on = getenv("FARCRY_GPU_PROFILE") != NULL;
    if (s_gpuProfFrame)
    {
      s_gpuProfFrame = false;
      r_glFinish();
      if (s_gpuProfLastEnd) s_gpuProf["(between draws)"].first += GpuProfMs(s_gpuProfLastEnd, SDL_GetPerformanceCounter());
      std::vector<std::pair<double, std::string> > v; double total = 0;
      for (auto& e : s_gpuProf) { v.push_back(std::make_pair(e.second.first, e.first + " x" + std::to_string(e.second.second))); total += e.second.first; }
      std::sort(v.rbegin(), v.rend());
      fprintf(stderr, "PORT gpuprof frame=%u total=%.2fms\n", s_diagFrame, total);
      for (size_t i = 0; i < v.size() && i < 25; ++i) fprintf(stderr, "PORT gpuprof   %7.2fms %s\n", v[i].first, v[i].second.c_str());
      s_gpuProf.clear(); s_gpuProfLastEnd = 0;
    }
    static Uint32 s_nextProf = 0;
    if (s_on && SDL_GetTicks() >= s_nextProf)
    {
      if (s_nextProf) { s_gpuProfFrame = true; r_glFinish(); }
      s_nextProf = SDL_GetTicks() + 10000;
    }
  }
  static const char* s_env = getenv("FARCRY_GLES3_DUMP_FRAME");
  if (s_dumpRequested && s_dumpState != 1)
  {
    s_dumpRequested = false;
    s_dumpState = 1; s_audit = true; s_auditId = 0; s_stateDirty = true;
    fprintf(stderr, "GLES3FFP DUMP: begin frame (requested)\n");
    return;
  }
  if ((!s_env && s_dumpState != 1) || s_dumpState == 2) return;
  if (!s_dumpStartTicks) s_dumpStartTicks = SDL_GetTicks();
  if (s_dumpState == 1)
  {
    s_dumpState = 2;
    if (s_audit)
    {
      GLint vp[4] = {0, 0, 640, 480}; r_glGetIntegerv(GL_VIEWPORT, vp);
      std::vector<unsigned char> px((size_t)vp[2] * vp[3] * 4);
      r_glReadPixels(0, 0, vp[2], vp[3], GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      const char* dir = getenv("FARCRY_CAPTURE_DIR");
      char path[1024]; snprintf(path, sizeof(path), "%s/audit.ppm", dir ? dir : ".");
      if (FILE* f = fopen(path, "wb"))
      {
        fprintf(f, "P6\n%d %d\n255\n", vp[2], vp[3]);
        for (int y = vp[3] - 1; y >= 0; --y) for (int x = 0; x < vp[2]; ++x) fwrite(&px[((size_t)y * vp[2] + x) * 4], 1, 3, f);
        fclose(f);
      }
      s_stateDirty = true;
    }
    fprintf(stderr, "GLES3FFP DUMP: end of frame\n");
    if (s_audit && s_auditHook) s_auditHook();
    return;
  }
  const double dumpT = PortBenchRelative() ? (g_PortGameplayStartMs > 0 ? (PortNowMs() - g_PortGameplayStartMs) / 1000.0 : -1.0)
                                            : (SDL_GetTicks() - s_dumpStartTicks) / 1000.0;
  // FARCRY_GLES3_DUMP_MIN_DRAWS=N: only dump once a frame (after the time)
  // has more than N draws (catches rare heavy frames, e.g. Steam's fight).
  static const int s_minDraws = getenv("FARCRY_GLES3_DUMP_MIN_DRAWS") ? atoi(getenv("FARCRY_GLES3_DUMP_MIN_DRAWS")) : 0;
  if (s_minDraws > 0 && (int)s_lastFrameDraws <= s_minDraws) return;
  if (dumpT >= 0 && dumpT >= atof(s_env)) { s_dumpState = 1; s_audit = getenv("FARCRY_GLES3_AUDIT") != NULL; s_auditId = 0; s_stateDirty = true; fprintf(stderr, "GLES3FFP DUMP: begin frame\n"); FFPLog("GLES3FFP DUMP: begin frame"); }
}
static void DumpDraw(const char* kind, GLsizei count)
{
  if (s_dumpState != 1) return;
  char tex[1024]; int o = 0;
  for (int u = 0; u < MAX_UNITS; ++u)
  {
    const SUnit& U = s_unit[u];
    if (!U.en2D && !U.enCube) continue;
    GLint minF = 0, maxL = 0;
    if (!U.enCube)
    {
      r_glActiveTexture(GL_TEXTURE0 + u);
      r_glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minF);
      r_glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxL);
      r_glActiveTexture(GL_TEXTURE0 + s_activeUnit);
    }
    else
    {
      typedef void (APIENTRY *PLevelParam)(GLenum, GLint, GLenum, GLint*);
      static PLevelParam levelParam = (PLevelParam)SDL_GL_GetProcAddress("glGetTexLevelParameteriv");
      r_glActiveTexture(GL_TEXTURE0 + u);
      r_glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, &minF);
      r_glGetTexParameteriv(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, &maxL);
      GLint fw[6] = {0}, fmt = 0, w1 = 0;
      for (int f = 0; f < 6 && levelParam; ++f) levelParam(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0x1000 /*GL_TEXTURE_WIDTH*/, &fw[f]);
      if (levelParam) { levelParam(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, 0x1003 /*GL_TEXTURE_INTERNAL_FORMAT*/, &fmt); levelParam(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 1, 0x1000, &w1); }
      r_glActiveTexture(GL_TEXTURE0 + s_activeUnit);
      o += snprintf(tex + o, sizeof(tex) - o, " cubefaces=%d,%d,%d,%d,%d,%d fmt=0x%x lvl1w=%d", fw[0], fw[1], fw[2], fw[3], fw[4], fw[5], fmt, w1);
    }
    if (U.gen[0] || U.gen[1])
      o += snprintf(tex + o, sizeof(tex) - o, " genmode=%x,%x eyeS=(%.3f %.3f %.3f %.3f) eyeT=(%.3f %.3f %.3f %.3f)", U.genMode[0], U.genMode[1],
                    U.eyePlane[0][0], U.eyePlane[0][1], U.eyePlane[0][2], U.eyePlane[0][3], U.eyePlane[1][0], U.eyePlane[1][1], U.eyePlane[1][2], U.eyePlane[1][3]);
    if (U.gen[0] || U.gen[1])
      o += snprintf(tex + o, sizeof(tex) - o, " planeS=(%.4f %.4f %.4f %.4f) planeT=(%.4f %.4f %.4f %.4f)",
                    U.objPlane[0][0], U.objPlane[0][1], U.objPlane[0][2], U.objPlane[0][3], U.objPlane[1][0], U.objPlane[1][1], U.objPlane[1][2], U.objPlane[1][3]);
    o += snprintf(tex + o, sizeof(tex) - o, " min=0x%x maxlvl=%d tcArr=%d(sz%d st%d buf%u off%ld) texmat=%s cur=(%.2f %.2f)", minF, maxL,
                  (int)s_texArr[u].on, s_texArr[u].size, (int)s_texArr[u].stride, s_texArr[u].buffer, (long)(size_t)s_texArr[u].ptr,
                  MatIsIdentity(TexMatrix(u)) ? "id" : "set", s_curTex[u][0], s_curTex[u][1]);
    o += snprintf(tex + o, sizeof(tex) - o, " u%d:%s#%u env=0x%x comb=0x%x/0x%x src=%x,%x op=%x,%x scale=%.1f const=(%.2f %.2f %.2f %.2f) gen=%d%d%d%d", u, U.enCube ? "cube" : "2d",
                  U.enCube ? s_boundCube[u] : s_bound2D[u], U.env.mode, U.env.combRGB, U.env.combA,
                  U.env.srcRGB[0], U.env.srcRGB[1], U.env.opRGB[0], U.env.opRGB[1], U.env.scaleRGB,
                  U.env.color[0], U.env.color[1], U.env.color[2], U.env.color[3], U.gen[0], U.gen[1], U.gen[2], U.gen[3]);
  }
  unsigned firstColor = 0;
  if (s_colorArr.on && !s_colorArr.buffer && s_colorArr.ptr && s_colorArr.type == GL_UNSIGNED_BYTE) memcpy(&firstColor, s_colorArr.ptr, 4);
  if (s_colorArr.on && s_colorArr.buffer)
  {
    BindReal(GL_ARRAY_BUFFER, s_colorArr.buffer);
    if (void* m = r_glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)(size_t)s_colorArr.ptr, 4, GL_MAP_READ_BIT)) { memcpy(&firstColor, m, 4); r_glUnmapBuffer(GL_ARRAY_BUFFER); }
    BindReal(GL_ARRAY_BUFFER, s_arrayBuffer);
  }
  ++s_auditId;
  fprintf(stderr, "GLES3FFP SHADER id=%u %s\n", s_auditId + 0, s_dumpLabel);
  fprintf(stderr, "GLES3FFP DUMP id=%u %s sem=%d vstride=%d coloff=%ld n=%d colorArr=%d(%s first=%08x) cur=(%.2f %.2f %.2f %.2f) light=%d/%d alpha=%d(0x%x %.2f) fog=%d%s\n",
         s_auditId, kind, s_colorArr.semantic, (int)s_vertexArr.stride, (long)((const char*)s_colorArr.ptr - (const char*)s_vertexArr.ptr), (int)count, (int)s_colorArr.on, s_colorArr.buffer ? "vbo" : "client", firstColor,
         s_curColor[0], s_curColor[1], s_curColor[2], s_curColor[3], (int)s_lighting, ActiveLightCount(),
         (int)s_alphaTest, s_alphaFunc, s_alphaRef, (int)s_fog, o ? tex : " notex");
  {
    GLint bs = 0, bd = 0; const GLboolean be = r_glIsEnabled(GL_BLEND);
    r_glGetIntegerv(GL_BLEND_SRC_RGB, &bs); r_glGetIntegerv(GL_BLEND_DST_RGB, &bd);
    GLint df = 0; r_glGetIntegerv(GL_DEPTH_FUNC, &df);
    fprintf(stderr, "GLES3FFP DUMP   state blend=%d(0x%x,0x%x) depthfunc=0x%x\n", (int)be, bs, bd, df);
  }
  if (s_fog) fprintf(stderr, "GLES3FFP DUMP   fog mode=0x%x start=%.1f end=%.1f density=%.4f color=(%.2f %.2f %.2f)\n",
                     s_fogMode, s_fogStart, s_fogEnd, s_fogDensity, s_fogColor[0], s_fogColor[1], s_fogColor[2]);
}

static void SetupClientArrays()
{
  SetAttr(ATTR_POS, &s_vertexArr, false, NULL);
  SetAttr(ATTR_COLOR, &s_colorArr, true, s_curColor);
  float n4[4] = { s_curNormal[0], s_curNormal[1], s_curNormal[2], 0 };
  SetAttr(ATTR_NORMAL, &s_normalArr, false, n4);
  for (int u = 0; u < MAX_UNITS; ++u)
    SetAttr(ATTR_TEX0 + u, &s_texArr[u], false, s_curTex[u]);
}

///////////////////////////////////////////////////////////////////////////////
// Primitive conversion (desktop quads / polygons -> triangles)
///////////////////////////////////////////////////////////////////////////////
static bool IsDesktopPrim(GLenum m) { return m == D_QUADS || m == D_QUAD_STRIP || m == D_POLYGON; }

template <class F> static void EmitTriangles(GLenum mode, int count, std::vector<uint32_t>& out, F idx)
{
  out.clear();
  if (mode == D_QUADS)
    for (int q = 0; q + 3 < count; q += 4)
    { out.push_back(idx(q)); out.push_back(idx(q+1)); out.push_back(idx(q+2));
      out.push_back(idx(q)); out.push_back(idx(q+2)); out.push_back(idx(q+3)); }
  else if (mode == D_QUAD_STRIP)
    for (int i = 0; i + 3 < count; i += 2)
    { out.push_back(idx(i)); out.push_back(idx(i+1)); out.push_back(idx(i+3));
      out.push_back(idx(i)); out.push_back(idx(i+3)); out.push_back(idx(i+2)); }
  else // polygon
    for (int i = 1; i + 1 < count; ++i)
    { out.push_back(idx(0)); out.push_back(idx(i)); out.push_back(idx(i+1)); }
}

static std::vector<uint32_t> s_tmpIndices;

static void DrawConvertedIndices()
{
  if (s_tmpIndices.empty()) return;
  if (s_elementBuffer) BindReal(GL_ELEMENT_ARRAY_BUFFER, 0);
  r_glDrawElements(GL_TRIANGLES, (GLsizei)s_tmpIndices.size(), GL_UNSIGNED_INT, s_tmpIndices.data());
  if (s_elementBuffer) BindReal(GL_ELEMENT_ARRAY_BUFFER, s_elementBuffer);
}

///////////////////////////////////////////////////////////////////////////////
// Draw calls
///////////////////////////////////////////////////////////////////////////////
static bool FFPActive() { return s_userProgram == 0; }
static unsigned s_statDraws = 0, s_statVerts = 0;
static Uint64 s_ticksFFP = 0, s_ticksGL = 0;
static unsigned s_statClientDraws = 0, s_statClientVerts = 0, s_statImmDraws = 0;
void GLES3FFPTakeArrayStats(unsigned& clientDraws, unsigned& clientVerts, unsigned& immDraws)
{ clientDraws = s_statClientDraws; clientVerts = s_statClientVerts; immDraws = s_statImmDraws; s_statClientDraws = s_statClientVerts = s_statImmDraws = 0; }
static void CountArraySource(GLsizei count) { if (s_vertexArr.on && !s_vertexArr.buffer) { ++s_statClientDraws; s_statClientVerts += (unsigned)count; } }
// Upload accounting (frame log columns up_ms / up_kb): buffer and texture
// uploads, copies and readbacks issued by the engine.
static Uint64 s_ticksUp = 0;
static uint64_t s_upBytes = 0;
struct SUpScope
{
  Uint64 t0; uint64_t n;
  explicit SUpScope(uint64_t bytes) : t0(SDL_GetPerformanceCounter()), n(bytes) { s_upBytes += bytes; }
  ~SUpScope()
  {
    const Uint64 dt = SDL_GetPerformanceCounter() - t0; s_ticksUp += dt;
    static const bool s_log = getenv("FARCRY_LOG_BIG_UPLOADS") != NULL; // diagnostic
    if (s_log && n >= 256 * 1024)
    {
      void* bt[6]; int c = backtrace(bt, 6);
      extern char __executable_start;
      const uintptr_t base = (uintptr_t)&__executable_start;
      fprintf(stderr, "PORT big upload %llu KB %.1f ms from %lx %lx %lx\n", (unsigned long long)(n / 1024), dt * 1000.0 / SDL_GetPerformanceFrequency(),
              (unsigned long)(c > 2 ? (uintptr_t)bt[2] - base : 0), (unsigned long)(c > 3 ? (uintptr_t)bt[3] - base : 0), (unsigned long)(c > 4 ? (uintptr_t)bt[4] - base : 0));
    }
  }
};
unsigned GLES3FFPTakeProgramsBuilt() { unsigned n = s_progBuiltFrame; s_progBuiltFrame = 0; return n; }
void GLES3FFPTakeUploadStats(double& ms, double& kb)
{
  ms = s_ticksUp * 1000.0 / (double)SDL_GetPerformanceFrequency(); kb = s_upBytes / 1024.0;
  s_ticksUp = 0; s_upBytes = 0;
}
void GLES3FFPTakeDrawStats(unsigned& draws, unsigned& verts) { s_lastFrameDraws = s_statDraws; draws = s_statDraws; verts = s_statVerts; s_statDraws = s_statVerts = 0; }
void GLES3FFPTakeTimes(double& ffpMs, double& glMs)
{
  const double f = 1000.0 / (double)SDL_GetPerformanceFrequency();
  ffpMs = s_ticksFFP * f; glMs = s_ticksGL * f; s_ticksFFP = s_ticksGL = 0;
}
// Audit frame: draw ids without blending so the last pass covering a pixel
// (including multiplicative passes) is the one recorded.
// Diagnostic: FARCRY_GLES3_SKIPMUL=1 drops multiplicative (DST_COLOR) passes.
static bool SkipMulDraw()
{
  static const bool on = getenv("FARCRY_GLES3_SKIPMUL") != NULL, all = getenv("FARCRY_GLES3_SKIPBLEND") != NULL;
  if ((!on && !all) || !r_glIsEnabled(GL_BLEND)) return false;
  if (all) return true;
  GLint src = 0; r_glGetIntegerv(GL_BLEND_SRC_RGB, &src);
  return src == GL_DST_COLOR;
}
struct SAuditNoBlend
{
  bool restore;
  SAuditNoBlend() : restore(s_audit && s_dumpState == 1 && r_glIsEnabled(GL_BLEND)) { if (restore) r_glDisable(GL_BLEND); }
  ~SAuditNoBlend() { if (restore) r_glEnable(GL_BLEND); }
};
struct STickScope { Uint64& acc; Uint64 t0; explicit STickScope(Uint64& a) : acc(a), t0(SDL_GetPerformanceCounter()) {} ~STickScope() { acc += SDL_GetPerformanceCounter() - t0; } };
// Diagnostic: log draw calls that block in the driver (FARCRY_GLES3_SLOW_DRAW_MS=<ms>).
static double SlowDrawThresholdMs() { static const double t = getenv("FARCRY_GLES3_SLOW_DRAW_MS") ? atof(getenv("FARCRY_GLES3_SLOW_DRAW_MS")) : 0.0; return t; }
// Diagnostic: FARCRY_GPU_PROFILE=1 profiles one frame every 10 s: a
// glFinish before and after every draw gives each draw's GPU time, summed
// per shader (label) and logged as "PORT gpuprof ...". Work outside draws
// (clears, copies, presents) is reported as "(between draws)".
struct SDrawTick
{
  Uint64& acc; Uint64 t0; const char* kind; int count;
  SDrawTick(Uint64& a, const char* k, int n) : acc(a), t0(0), kind(k), count(n)
  {
    if (s_gpuProfFrame)
    {
      r_glFinish();
      const Uint64 now = SDL_GetPerformanceCounter();
      if (s_gpuProfLastEnd) { auto& e = s_gpuProf["(between draws)"]; e.first += GpuProfMs(s_gpuProfLastEnd, now); }
    }
    t0 = SDL_GetPerformanceCounter();
  }
  ~SDrawTick()
  {
    if (s_gpuProfFrame)
    {
      r_glFinish();
      s_gpuProfLastEnd = SDL_GetPerformanceCounter();
      auto& e = s_gpuProf[s_dumpLabel[0] ? s_dumpLabel : kind]; e.first += GpuProfMs(t0, s_gpuProfLastEnd); ++e.second;
    }
    const Uint64 d = SDL_GetPerformanceCounter() - t0; acc += d;
    // Diagnostic: FARCRY_GLES3_DRAW_TRACE=<part of a shader name> logs every such draw.
    static const char* s_trace = getenv("FARCRY_GLES3_DRAW_TRACE");
    if (s_trace && strstr(s_dumpLabel, s_trace))
    {
      static GLuint s_lastProg = 0; static unsigned s_lastFrame = 0; static int s_inFrame = 0;
      if (s_lastFrame != s_diagFrame) { s_lastFrame = s_diagFrame; s_inFrame = 0; }
      GLint src = 0, dst = 0; r_glGetIntegerv(GL_BLEND_SRC_RGB, &src); r_glGetIntegerv(GL_BLEND_DST_RGB, &dst);
      fprintf(stderr, "PORT drawtrace f=%u #%d %.3fms n=%d prog=%u%s vbo=%u ibo=%u tex0=%u blend=%x/%x\n", s_diagFrame, s_inFrame++,
              (double)d * 1000.0 / (double)SDL_GetPerformanceFrequency(), count, s_boundProg, s_boundProg != s_lastProg ? "*" : "",
              RealBuf(s_vertexArr.buffer), s_elementBuffer, s_bound2D[0], (unsigned)src, (unsigned)dst);
      s_lastProg = s_boundProg;
    }
    const double thr = SlowDrawThresholdMs();
    if (thr > 0)
    {
      const double ms = d * 1000.0 / (double)SDL_GetPerformanceFrequency();
      if (ms >= thr)
        { auto it = s_texUploadFrame.find(s_bound2D[0]); const int age = it == s_texUploadFrame.end() ? -1 : (int)(s_diagFrame - it->second);
        GLint vsz = 0, isz = 0, rb = 0, src = 0, dst = 0;
        r_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &rb);
        if (s_vertexArr.buffer) { BindReal(GL_ARRAY_BUFFER, s_vertexArr.buffer); r_glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &vsz); r_glBindBuffer(GL_ARRAY_BUFFER, rb); }
        if (s_elementBuffer) r_glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &isz);
        r_glGetIntegerv(GL_BLEND_SRC_RGB, &src); r_glGetIntegerv(GL_BLEND_DST_RGB, &dst);
        fprintf(stderr, "PORT slowdraw %.2fms %s n=%d shader=%s vbo=%u(%dKB real=%u) ibo=%u(%dKB) vtxClient=%d tex0=%u tex0age=%d blend=%x/%x frame=%u\n", ms, kind, count, s_dumpLabel,
                s_vertexArr.buffer, vsz / 1024, RealBuf(s_vertexArr.buffer), s_elementBuffer, isz / 1024, (int)(s_vertexArr.on && !s_vertexArr.buffer), s_bound2D[0], age, (unsigned)src, (unsigned)dst, s_diagFrame); }
    }
  }
};

// Diagnostic: FARCRY_GLES3_SKIP_SHADER=<a,b,...> drops draws whose shader
// label contains any of the parts (GPU cost bisection on tile-based GPUs).
static bool SkipShaderDraw()
{
  static const char* s_skip = getenv("FARCRY_GLES3_SKIP_SHADER");
  if (!s_skip || !*s_skip || !s_dumpLabel[0]) return false;
  const char* p = s_skip;
  while (*p)
  {
    const char* e = strchr(p, ',');
    const size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n) { std::string part(p, n); if (strstr(s_dumpLabel, part.c_str())) return true; }
    if (!e) break;
    p = e + 1;
  }
  return false;
}
static void APIENTRY f_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
  if (SkipShaderDraw()) return;
  if (FFPActive()) DumpDraw("arrays", count);
  SAuditNoBlend auditNoBlend;
  ++s_statDraws; s_statVerts += (unsigned)count; CountArraySource(count);
  if (FFPActive()) { STickScope ts(s_ticksFFP); ApplyProgram(); SetupClientArrays(); }
  if (IsDesktopPrim(mode))
  {
    EmitTriangles(mode, count, s_tmpIndices, [first](int i) { return (uint32_t)(first + i); });
    DrawConvertedIndices();
    return;
  }
  SDrawTick tg(s_ticksGL, "arrays", (int)count);
  r_glDrawArrays(mode, first, count);
}

static void ReadIndices(GLsizei count, GLenum type, const void* indices, std::vector<uint32_t>& out)
{
  out.resize(count);
  const void* src = indices;
  void* mapped = NULL;
  if (s_elementBuffer)
  {
    GLint size = 0; r_glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
    mapped = r_glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, size, GL_MAP_READ_BIT);
    src = (const char*)mapped + (size_t)indices;
  }
  for (GLsizei i = 0; i < count; ++i)
    out[i] = type == GL_UNSIGNED_INT ? ((const uint32_t*)src)[i] : (type == GL_UNSIGNED_SHORT ? ((const uint16_t*)src)[i] : ((const uint8_t*)src)[i]);
  if (mapped) r_glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
}

static std::vector<uint32_t> s_tmpRead;

// FARCRY_GLES3_VALIDATE=1: check that indexed draws stay inside the bound
// vertex buffers (out-of-range reads can reset desktop GPUs). Logs offenders.
static int s_validate = -1;
static void ValidateIndexed(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
  if (s_validate < 0) s_validate = getenv("FARCRY_GLES3_VALIDATE") ? 1 : 0;
  if (!s_validate || count <= 0) return;
  std::vector<uint32_t> idx;
  ReadIndices(count, type, indices, idx);
  uint32_t maxIdx = 0;
  for (uint32_t v : idx) if (v > maxIdx) maxIdx = v;
  const SClientArray* arrays[3] = { &s_vertexArr, &s_colorArr, &s_normalArr };
  const char* names[3] = { "vertex", "color", "normal" };
  for (int a = 0; a < 4 + 3; ++a)
  {
    const SClientArray* A = a < 3 ? arrays[a] : &s_texArr[a - 3];
    if (!A->on || !A->buffer) continue;          // client memory: size unknown
    GLint size = 0;
    BindReal(GL_ARRAY_BUFFER, A->buffer);
    r_glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
    BindReal(GL_ARRAY_BUFFER, s_arrayBuffer);
    const int elem = A->size * (A->type == GL_FLOAT ? 4 : (A->type == GL_SHORT || A->type == GL_UNSIGNED_SHORT ? 2 : 1));
    const size_t stride = A->stride ? (size_t)A->stride : (size_t)elem;
    const size_t need = (size_t)A->ptr + stride * maxIdx + elem;
    if (need > (size_t)size)
    {
      static int s_logged = 0;
      if (s_logged++ < 20)
        FFPLog("GLES3FFP VALIDATE: %s array overrun: max index %u needs %zu bytes, buffer %u has %d (offset %zu stride %zu mode 0x%x count %d)",
               a < 3 ? names[a] : "texcoord", maxIdx, need, A->buffer, size, (size_t)A->ptr, stride, mode, count);
    }
  }
}

static void DumpUniforms()
{
  if (s_dumpState != 1 || !s_curProg) return;
  static PFNGLGETUNIFORMFVPROC getU = (PFNGLGETUNIFORMFVPROC)SDL_GL_GetProcAddress("glGetUniformfv");
  SProgram& p = *s_curProg;
  float c0[4] = {0}, c1[4] = {0}, s0[2] = {0}, s1[2] = {0}, fp[4] = {0}, fc[4] = {0}, ar = -1;
  if (p.uEnvColor[0] >= 0) getU(p.prog, p.uEnvColor[0], c0);
  if (p.uEnvColor[1] >= 0) getU(p.prog, p.uEnvColor[1], c1);
  if (p.uEnvScale[0] >= 0) getU(p.prog, p.uEnvScale[0], s0);
  if (p.uEnvScale[1] >= 0) getU(p.prog, p.uEnvScale[1], s1);
  if (p.uFogParams >= 0) getU(p.prog, p.uFogParams, fp);
  if (p.uFogColor >= 0) getU(p.prog, p.uFogColor, fc);
  if (p.uAlphaRef >= 0) getU(p.prog, p.uAlphaRef, &ar);
  fprintf(stderr, "GLES3FFP DUMP   gpu prog=%u env0=(%.2f %.2f %.2f %.2f)x(%.2f %.2f) env1=(%.2f %.2f %.2f %.2f)x(%.2f %.2f) fog=(%.2f %.2f %.4f %.4f) fogc=(%.2f %.2f %.2f) aref=%.2f\n",
          p.prog, c0[0], c0[1], c0[2], c0[3], s0[0], s0[1], c1[0], c1[1], c1[2], c1[3], s1[0], s1[1], fp[0], fp[1], fp[2], fp[3], fc[0], fc[1], fc[2], ar);
}

static void APIENTRY f_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
  if (SkipShaderDraw()) return;
  if (SkipMulDraw()) return;
  ++s_statDraws; s_statVerts += (unsigned)count; CountArraySource(count);
  ValidateIndexed(mode, count, type, indices);
  if (FFPActive()) DumpDraw("elements", count);
  SAuditNoBlend auditNoBlend;
  if (FFPActive()) { STickScope ts(s_ticksFFP); ApplyProgram(); SetupClientArrays(); }
  if (FFPActive()) DumpUniforms();
  if (IsDesktopPrim(mode))
  {
    ReadIndices(count, type, indices, s_tmpRead);
    EmitTriangles(mode, count, s_tmpIndices, [](int i) { return s_tmpRead[i]; });
    DrawConvertedIndices();
    return;
  }
  SDrawTick tg(s_ticksGL, "elements", (int)count);
  // Diagnostic: FARCRY_GLES3_SPLIT_DRAWS=1 issues triangle draws as two halves
  // (same triangles, twice the draw calls): measures per-draw GPU cost.
  static const bool s_split = getenv("FARCRY_GLES3_SPLIT_DRAWS") != NULL;
  if (s_split && mode == GL_TRIANGLES && count >= 6)
  {
    const GLsizei a = (count / 6) * 3, isz = type == GL_UNSIGNED_INT ? 4 : (type == GL_UNSIGNED_SHORT ? 2 : 1);
    r_glDrawElements(mode, a, type, indices);
    r_glDrawElements(mode, count - a, type, (const char*)indices + (size_t)a * isz);
    return;
  }
  r_glDrawElements(mode, count, type, indices);
}

static void APIENTRY f_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices)
{
  if (SkipShaderDraw()) return;
  if (IsDesktopPrim(mode)) { f_glDrawElements(mode, count, type, indices); return; }
  ++s_statDraws; s_statVerts += (unsigned)count; CountArraySource(count);
  ValidateIndexed(mode, count, type, indices);
  if (FFPActive()) DumpDraw("range", count);
  SAuditNoBlend auditNoBlend;
  if (FFPActive()) { STickScope ts(s_ticksFFP); ApplyProgram(); SetupClientArrays(); }
  SDrawTick tg(s_ticksGL, "range", (int)count);
  static const bool s_split = getenv("FARCRY_GLES3_SPLIT_DRAWS") != NULL; // diagnostic, see f_glDrawElements
  if (s_split && mode == GL_TRIANGLES && count >= 6)
  {
    const GLsizei a = (count / 6) * 3, isz = type == GL_UNSIGNED_INT ? 4 : (type == GL_UNSIGNED_SHORT ? 2 : 1);
    r_glDrawRangeElements(mode, start, end, a, type, indices);
    r_glDrawRangeElements(mode, start, end, count - a, type, (const char*)indices + (size_t)a * isz);
    return;
  }
  r_glDrawRangeElements(mode, start, end, count, type, indices);
}

///////////////////////////////////////////////////////////////////////////////
// Immediate mode
///////////////////////////////////////////////////////////////////////////////
struct SImmVertex { float pos[4]; float color[4]; float normal[3]; float tex[MAX_UNITS][4]; };
static std::vector<SImmVertex> s_imm;
static GLenum s_immMode = 0;
static bool s_inBegin = false;
static GLuint s_immVBO = 0;

static void APIENTRY f_glBegin(GLenum mode) { s_immMode = mode; s_inBegin = true; s_imm.clear(); }

static void ImmVertex(float x, float y, float z, float w)
{
  SImmVertex v;
  v.pos[0] = x; v.pos[1] = y; v.pos[2] = z; v.pos[3] = w;
  memcpy(v.color, s_curColor, 16); memcpy(v.normal, s_curNormal, 12); memcpy(v.tex, s_curTex, sizeof(v.tex));
  s_imm.push_back(v);
}

static void APIENTRY f_glEnd()
{
  s_inBegin = false;
  if (s_imm.empty()) return;
  ++s_statDraws; s_statVerts += (unsigned)s_imm.size(); ++s_statImmDraws;
  DumpDraw("immediate", (GLsizei)s_imm.size());
  SAuditNoBlend auditNoBlend;
  if (!s_immVBO) r_glGenBuffers(1, &s_immVBO);
  BindReal(GL_ARRAY_BUFFER, s_immVBO);
  r_glBufferData(GL_ARRAY_BUFFER, s_imm.size() * sizeof(SImmVertex), s_imm.data(), GL_STREAM_DRAW);
  ApplyProgram();
  const GLsizei st = sizeof(SImmVertex);
  struct { int attr; size_t off; int n; } layout[] = {
    { ATTR_POS, offsetof(SImmVertex, pos), 4 }, { ATTR_COLOR, offsetof(SImmVertex, color), 4 },
    { ATTR_NORMAL, offsetof(SImmVertex, normal), 3 } };
  for (int a = 0; a < NUM_ATTRS; ++a) InvalidateAttr(a);
  for (auto& l : layout)
  {
    r_glVertexAttribPointer(l.attr, l.n, GL_FLOAT, GL_FALSE, st, (const void*)l.off);
    if (!s_attrOn[l.attr]) { r_glEnableVertexAttribArray(l.attr); s_attrOn[l.attr] = true; }
  }
  for (int u = 0; u < MAX_UNITS; ++u)
  {
    r_glVertexAttribPointer(ATTR_TEX0 + u, 4, GL_FLOAT, GL_FALSE, st, (const void*)(offsetof(SImmVertex, tex) + u * 16));
    if (!s_attrOn[ATTR_TEX0 + u]) { r_glEnableVertexAttribArray(ATTR_TEX0 + u); s_attrOn[ATTR_TEX0 + u] = true; }
  }
  if (IsDesktopPrim(s_immMode))
  {
    EmitTriangles(s_immMode, (int)s_imm.size(), s_tmpIndices, [](int i) { return (uint32_t)i; });
    DrawConvertedIndices();
  }
  else
    r_glDrawArrays(s_immMode, 0, (GLsizei)s_imm.size());
  BindReal(GL_ARRAY_BUFFER, s_arrayBuffer);
  s_imm.clear();
}

static void APIENTRY f_glVertex2f(GLfloat x, GLfloat y) { ImmVertex(x, y, 0, 1); }
static void APIENTRY f_glVertex2fv(const GLfloat* v) { ImmVertex(v[0], v[1], 0, 1); }
static void APIENTRY f_glVertex2i(GLint x, GLint y) { ImmVertex((float)x, (float)y, 0, 1); }
static void APIENTRY f_glVertex3f(GLfloat x, GLfloat y, GLfloat z) { ImmVertex(x, y, z, 1); }
static void APIENTRY f_glVertex3fv(const GLfloat* v) { ImmVertex(v[0], v[1], v[2], 1); }
static void APIENTRY f_glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { ImmVertex(x, y, z, w); }
static void APIENTRY f_glVertex4fv(const GLfloat* v) { ImmVertex(v[0], v[1], v[2], v[3]); }

static void SetColor(float r, float g, float b, float a) { s_curColor[0] = r; s_curColor[1] = g; s_curColor[2] = b; s_curColor[3] = a; }
static void APIENTRY f_glColor3f(GLfloat r, GLfloat g, GLfloat b) { SetColor(r, g, b, 1); }
static void APIENTRY f_glColor3fv(const GLfloat* v) { SetColor(v[0], v[1], v[2], 1); }
static void APIENTRY f_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { SetColor(r, g, b, a); }
static void APIENTRY f_glColor4fv(const GLfloat* v) { SetColor(v[0], v[1], v[2], v[3]); }
static void APIENTRY f_glColor3ub(GLubyte r, GLubyte g, GLubyte b) { SetColor(r/255.f, g/255.f, b/255.f, 1); }
static void APIENTRY f_glColor3ubv(const GLubyte* v) { SetColor(v[0]/255.f, v[1]/255.f, v[2]/255.f, 1); }
static void APIENTRY f_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { SetColor(r/255.f, g/255.f, b/255.f, a/255.f); }
static void APIENTRY f_glColor4ubv(const GLubyte* v) { SetColor(v[0]/255.f, v[1]/255.f, v[2]/255.f, v[3]/255.f); }

static void SetTex(int u, float s, float t, float r, float q)
{
  if (u < 0 || u >= MAX_UNITS) return;
  s_curTex[u][0] = s; s_curTex[u][1] = t; s_curTex[u][2] = r; s_curTex[u][3] = q;
}
static void APIENTRY f_glTexCoord1f(GLfloat s) { SetTex(0, s, 0, 0, 1); }
static void APIENTRY f_glTexCoord2f(GLfloat s, GLfloat t) { SetTex(0, s, t, 0, 1); }
static void APIENTRY f_glTexCoord2fv(const GLfloat* v) { SetTex(0, v[0], v[1], 0, 1); }
static void APIENTRY f_glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) { SetTex(0, s, t, r, 1); }
static void APIENTRY f_glTexCoord3fv(const GLfloat* v) { SetTex(0, v[0], v[1], v[2], 1); }
static void APIENTRY f_glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { SetTex(0, s, t, r, q); }
static void APIENTRY f_glTexCoord4fv(const GLfloat* v) { SetTex(0, v[0], v[1], v[2], v[3]); }
static void APIENTRY f_glMultiTexCoord2f(GLenum u, GLfloat s, GLfloat t) { SetTex(u - D_TEXTURE0, s, t, 0, 1); }
static void APIENTRY f_glMultiTexCoord2fv(GLenum u, const GLfloat* v) { SetTex(u - D_TEXTURE0, v[0], v[1], 0, 1); }
static void APIENTRY f_glMultiTexCoord3f(GLenum u, GLfloat s, GLfloat t, GLfloat r) { SetTex(u - D_TEXTURE0, s, t, r, 1); }
static void APIENTRY f_glMultiTexCoord3fv(GLenum u, const GLfloat* v) { SetTex(u - D_TEXTURE0, v[0], v[1], v[2], 1); }
static void APIENTRY f_glMultiTexCoord4f(GLenum u, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { SetTex(u - D_TEXTURE0, s, t, r, q); }
static void APIENTRY f_glMultiTexCoord4fv(GLenum u, const GLfloat* v) { SetTex(u - D_TEXTURE0, v[0], v[1], v[2], v[3]); }
static void APIENTRY f_glNormal3f(GLfloat x, GLfloat y, GLfloat z) { s_curNormal[0] = x; s_curNormal[1] = y; s_curNormal[2] = z; }
static void APIENTRY f_glNormal3fv(const GLfloat* v) { f_glNormal3f(v[0], v[1], v[2]); }

///////////////////////////////////////////////////////////////////////////////
// Client arrays and buffers
///////////////////////////////////////////////////////////////////////////////
static void SetArray(SClientArray& a, GLint size, GLenum type, GLsizei stride, const void* ptr)
{
  a.size = size; a.type = type; a.stride = stride; a.ptr = ptr; a.buffer = s_arrayBuffer;
}
static void APIENTRY f_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* ptr) { SetArray(s_vertexArr, size, type, stride, ptr); }
static void APIENTRY f_glColorPointer(GLint size, GLenum type, GLsizei stride, const void* ptr)
{
  SetArray(s_colorArr, size, type, stride, ptr);
  if (s_colorArr.semantic != s_pendingColorSemantic) s_stateDirty = true;
  s_colorArr.semantic = s_pendingColorSemantic;
  s_pendingColorSemantic = 0;
}
void GLES3FFPMarkNextColorArray(int semantic)
{
  s_pendingColorSemantic = semantic;
  if (GLR::Active()) GLR::SetColorSemantic(semantic);   // native backend: same colour semantics
}
static void APIENTRY f_glNormalPointer(GLenum type, GLsizei stride, const void* ptr) { SetArray(s_normalArr, 3, type, stride, ptr); }
static void APIENTRY f_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* ptr)
{
  if (s_clientUnit < MAX_UNITS) SetArray(s_texArr[s_clientUnit], size, type, stride, ptr);
}
static void APIENTRY f_glSecondaryColorPointer(GLint size, GLenum type, GLsizei stride, const void* ptr) { SetArray(s_color2Arr, size, type, stride, ptr); }

static SClientArray* ClientArrayFor(GLenum cap)
{
  switch (cap)
  {
    case D_VERTEX_ARRAY: return &s_vertexArr;
    case D_COLOR_ARRAY: return &s_colorArr;
    case D_NORMAL_ARRAY: return &s_normalArr;
    case D_SECONDARY_COLOR_ARRAY: return &s_color2Arr;
    case D_TEXTURE_COORD_ARRAY: return s_clientUnit < MAX_UNITS ? &s_texArr[s_clientUnit] : NULL;
  }
  return NULL;
}
static void APIENTRY f_glEnableClientState(GLenum cap) { if (SClientArray* a = ClientArrayFor(cap)) { if (a == &s_colorArr && !a->on) s_stateDirty = true; a->on = true; } }
static void APIENTRY f_glDisableClientState(GLenum cap) { if (SClientArray* a = ClientArrayFor(cap)) { if (a == &s_colorArr && a->on) s_stateDirty = true; a->on = false; } }
static void APIENTRY f_glClientActiveTexture(GLenum u) { s_clientUnit = (int)(u - D_TEXTURE0); }

static void APIENTRY f_glBindBuffer(GLenum target, GLuint buffer)
{
  if (target == GL_ARRAY_BUFFER) s_arrayBuffer = buffer;
  else if (target == GL_ELEMENT_ARRAY_BUFFER) s_elementBuffer = buffer;
  BindReal(target, buffer);
}

// Engine vertex/index buffers are rewritten through glMapBuffer every frame
// (skinned characters, particles). A synchronised map of a buffer the GPU is
// still reading makes Mali wait for the GPU (Rebellion on the H700: 37 ms per
// frame, 17 FPS). Write maps go to a CPU shadow copy instead; unmap orphans the
// buffer with glBufferData from that copy, so the driver never waits and the
// whole buffer stays valid (unlike an unsynchronised map, which races the GPU).
struct SShadowBuf { std::vector<uint8_t> data; GLenum usage; };
static std::unordered_map<GLuint, SShadowBuf> s_shadowBufs;
static GLuint s_mappedShadow[2] = { 0, 0 }; // [0] array, [1] element
static GLuint BoundBufferFor(GLenum target) { return target == GL_ELEMENT_ARRAY_BUFFER ? s_elementBuffer : s_arrayBuffer; }
static void* APIENTRY f_glMapBuffer(GLenum target, GLenum access)
{
  GLint size = 0;
  r_glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
  if (size <= 0) return NULL;
  const GLuint buf = BoundBufferFor(target);
  static const bool s_noShadow = getenv("FARCRY_GLES3_NO_SHADOW_MAP") != NULL; // diagnostic
  // Large shared pools (particles, decals, dynamic geometry) are written a
  // small range at a time, ring-buffer style (Direct3D NOOVERWRITE): a full
  // shadow re-upload per unmap cost ~20 MB/frame in combat. Map them
  // unsynchronised instead; small buffers (skinned meshes, rewritten whole)
  // keep the shadow copy.
  static const GLint s_bigBuffer = getenv("FARCRY_GLES3_SHADOW_MAX_KB") ? atoi(getenv("FARCRY_GLES3_SHADOW_MAX_KB")) * 1024 : 256 * 1024;
  if (access == D_WRITE_ONLY && size > s_bigBuffer && !s_noShadow)
  {
    SUpScope sc(0);
    // Orphan (default): fresh storage, so draws still in flight keep the old
    // contents (D3D DISCARD). Writing into storage the GPU may still read
    // (FARCRY_GLES3_BIGMAP=unsync) made geometry flicker for a frame.
    static const bool s_unsync = getenv("FARCRY_GLES3_BIGMAP") && !strcmp(getenv("FARCRY_GLES3_BIGMAP"), "unsync");
    if (s_unsync)
      return r_glMapBufferRange(target, 0, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    // Default: ring of real buffers (see SBufRing). FARCRY_GLES3_BIGMAP=orphan keeps the old orphaning.
    static const bool s_orphanMode = getenv("FARCRY_GLES3_BIGMAP") && !strcmp(getenv("FARCRY_GLES3_BIGMAP"), "orphan");
    static const int s_ringSize = []{ const char* v = getenv("FARCRY_GLES3_BUFFER_RING"); int n = v ? atoi(v) : 3; return n < 2 ? 2 : (n > 4 ? 4 : n); }();
    if (!s_orphanMode)
    {
      static std::unordered_map<GLuint, unsigned> s_ringFrame;
      unsigned& lastR = s_ringFrame[buf];
      if (lastR != s_frameCounter)
      {
        lastR = s_frameCounter;
        SBufRing& r = s_bufRings[buf];
        if (r.n == 0)
        {
          GLint usage = GL_DYNAMIC_DRAW; r_glGetBufferParameteriv(target, GL_BUFFER_USAGE, &usage);
          r.n = s_ringSize; r.cur = 0; r.ids[0] = buf;
          r_glGenBuffers(r.n - 1, &r.ids[1]);
          for (int k = 1; k < r.n; ++k) { r_glBindBuffer(target, r.ids[k]); r_glBufferData(target, size, NULL, (GLenum)usage); }
        }
        r.cur = (r.cur + 1) % r.n;
        r_glBindBuffer(target, r.ids[r.cur]);
      }
      return r_glMapBufferRange(target, 0, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    }
    // Orphan only on the first map of a buffer in a frame; later maps in the
    // same frame append (D3D DISCARD then NOOVERWRITE) and must not
    // reallocate ~1 MB per particle draw (Steam firefight: 36 per frame).
    static std::unordered_map<GLuint, unsigned> s_orphanFrame;
    unsigned& last = s_orphanFrame[buf];
    if (last != s_frameCounter)
    {
      last = s_frameCounter;
      GLint usage = GL_DYNAMIC_DRAW; r_glGetBufferParameteriv(target, GL_BUFFER_USAGE, &usage);
      r_glBufferData(target, size, NULL, (GLenum)usage);
      return r_glMapBufferRange(target, 0, size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    }
    return r_glMapBufferRange(target, 0, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
  }
  if (access != D_READ_ONLY && buf && !s_noShadow && (target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER))
  {
    auto it = s_shadowBufs.find(buf);
    if (it == s_shadowBufs.end() || it->second.data.size() != (size_t)size)
    {
      // First map: one synchronised read to seed the copy.
      SShadowBuf& sb = s_shadowBufs[buf];
      sb.data.resize(size);
      GLint usage = GL_DYNAMIC_DRAW; r_glGetBufferParameteriv(target, GL_BUFFER_USAGE, &usage); sb.usage = (GLenum)usage;
      if (void* m = r_glMapBufferRange(target, 0, size, GL_MAP_READ_BIT)) { memcpy(sb.data.data(), m, size); r_glUnmapBuffer(target); }
      it = s_shadowBufs.find(buf);
    }
    s_mappedShadow[target == GL_ELEMENT_ARRAY_BUFFER] = buf;
    return it->second.data.data();
  }
  GLbitfield f = access == D_READ_ONLY ? GL_MAP_READ_BIT : (access == D_WRITE_ONLY ? GL_MAP_WRITE_BIT : (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT));
  SUpScope sc(0);
  return r_glMapBufferRange(target, 0, size, f);
}
static GLboolean APIENTRY f_glUnmapBuffer(GLenum target)
{
  const int slot = target == GL_ELEMENT_ARRAY_BUFFER;
  if ((target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER) && s_mappedShadow[slot] && s_mappedShadow[slot] == BoundBufferFor(target))
  {
    SShadowBuf& sb = s_shadowBufs[s_mappedShadow[slot]];
    s_mappedShadow[slot] = 0;
    SUpScope sc(sb.data.size());
    r_glBufferData(target, (GLsizeiptr)sb.data.size(), sb.data.data(), sb.usage);
    return GL_TRUE;
  }
  return r_glUnmapBuffer(target);
}
static void APIENTRY f_glDeleteBuffers(GLsizei n, const GLuint* bufs)
{
  for (GLsizei i = 0; i < n; ++i)
  {
    s_shadowBufs.erase(bufs[i]);
    DropRing(bufs[i]);
  }
  r_glDeleteBuffers(n, bufs);
}

static void APIENTRY f_glUseProgram(GLuint prog) { s_userProgram = prog; s_boundProg = prog; r_glUseProgram(prog); }


///////////////////////////////////////////////////////////////////////////////
// Matrices
///////////////////////////////////////////////////////////////////////////////
static void APIENTRY f_glMatrixMode(GLenum mode) { s_matrixMode = mode == D_PROJECTION ? 1 : (mode == D_TEXTURE ? 2 : 0); }
static void TouchMatrix()
{
  if (s_matrixMode == 0) ++s_verMV;
  else if (s_matrixMode == 1) ++s_verP;
  else { ++s_verTex[s_activeUnit < MAX_UNITS ? s_activeUnit : 0]; s_stateDirty = true; }
}
static void APIENTRY f_glLoadIdentity() { MatIdentity(CurMat()); TouchMatrix(); }
static void APIENTRY f_glLoadMatrixf(const GLfloat* m) { memcpy(CurMat(), m, 64); TouchMatrix(); }
static void APIENTRY f_glLoadMatrixd(const GLdouble* m) { float f[16]; for (int i = 0; i < 16; ++i) f[i] = (float)m[i]; f_glLoadMatrixf(f); }
static void APIENTRY f_glMultMatrixf(const GLfloat* m) { float* c = CurMat(); MatMul(c, c, m); TouchMatrix(); }
static void APIENTRY f_glMultMatrixd(const GLdouble* m) { float f[16]; for (int i = 0; i < 16; ++i) f[i] = (float)m[i]; f_glMultMatrixf(f); }
static void APIENTRY f_glPushMatrix()
{
  int idx = s_matrixMode < 2 ? s_matrixMode : 2 + s_activeUnit;
  if (s_stack[idx].size() < 64) s_stack[idx].push_back(s_stack[idx].back());
}
static void APIENTRY f_glPopMatrix()
{
  int idx = s_matrixMode < 2 ? s_matrixMode : 2 + s_activeUnit;
  if (s_stack[idx].size() > 1) s_stack[idx].pop_back();
  TouchMatrix();
}
static void APIENTRY f_glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{ float t[16]; MatIdentity(t); t[12] = x; t[13] = y; t[14] = z; f_glMultMatrixf(t); }
static void APIENTRY f_glTranslated(GLdouble x, GLdouble y, GLdouble z) { f_glTranslatef((float)x, (float)y, (float)z); }
static void APIENTRY f_glScalef(GLfloat x, GLfloat y, GLfloat z)
{ float s[16]; MatIdentity(s); s[0] = x; s[5] = y; s[10] = z; f_glMultMatrixf(s); }
static void APIENTRY f_glScaled(GLdouble x, GLdouble y, GLdouble z) { f_glScalef((float)x, (float)y, (float)z); }
static void APIENTRY f_glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
  float len = sqrtf(x*x + y*y + z*z);
  if (len < 1e-12f) return;
  x /= len; y /= len; z /= len;
  float a = angle * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a), ic = 1.0f - c;
  float r[16] = { x*x*ic + c,   y*x*ic + z*s, x*z*ic - y*s, 0,
                  x*y*ic - z*s, y*y*ic + c,   y*z*ic + x*s, 0,
                  x*z*ic + y*s, y*z*ic - x*s, z*z*ic + c,   0,
                  0, 0, 0, 1 };
  f_glMultMatrixf(r);
}
static void APIENTRY f_glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) { f_glRotatef((float)a, (float)x, (float)y, (float)z); }
static void APIENTRY f_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
  float o[16]; MatIdentity(o);
  o[0] = (float)(2.0 / (r - l)); o[5] = (float)(2.0 / (t - b)); o[10] = (float)(-2.0 / (f - n));
  o[12] = (float)(-(r + l) / (r - l)); o[13] = (float)(-(t + b) / (t - b)); o[14] = (float)(-(f + n) / (f - n));
  f_glMultMatrixf(o);
}
static void APIENTRY f_glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
  float m[16]; memset(m, 0, 64);
  m[0] = (float)(2*n / (r - l)); m[5] = (float)(2*n / (t - b));
  m[8] = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b)); m[10] = (float)(-(f + n) / (f - n)); m[11] = -1;
  m[14] = (float)(-2*f*n / (f - n));
  f_glMultMatrixf(m);
}

///////////////////////////////////////////////////////////////////////////////
// Capabilities
///////////////////////////////////////////////////////////////////////////////
static bool PassThroughCap(GLenum cap)
{
  switch (cap)
  {
    case GL_BLEND: case GL_CULL_FACE: case GL_DEPTH_TEST: case GL_DITHER: case GL_POLYGON_OFFSET_FILL:
    case GL_SAMPLE_ALPHA_TO_COVERAGE: case GL_SAMPLE_COVERAGE: case GL_SCISSOR_TEST: case GL_STENCIL_TEST:
    case GL_RASTERIZER_DISCARD: case GL_PRIMITIVE_RESTART_FIXED_INDEX:
      return true;
  }
  return false;
}

static void SetCap(GLenum cap, bool on)
{
  // Diagnostic: FARCRY_GLES3_NO_DEPTH=1 keeps the depth test (and so depth
  // writes) off: GPU cost bisection of depth-buffer traffic.
  static const bool s_noDepth = getenv("FARCRY_GLES3_NO_DEPTH") != NULL;
  if (s_noDepth && cap == GL_DEPTH_TEST) on = false;
  if (PassThroughCap(cap)) { if (on) r_glEnable(cap); else r_glDisable(cap); return; }
  SUnit& U = s_unit[s_activeUnit < MAX_UNITS ? s_activeUnit : 0];
  switch (cap)
  {
    case GL_TEXTURE_2D: case D_TEXTURE_RECTANGLE: U.en2D = on; break;
    case D_TEXTURE_CUBE_MAP: U.enCube = on; break;
    case D_TEXTURE_3D: U.en3D = on; break;
    case D_ALPHA_TEST: s_alphaTest = on; break;
    case D_FOG: s_fog = on; break;
    case D_LIGHTING: s_lighting = on; break;
    case D_COLOR_MATERIAL: s_colorMaterial = on; break;
    case D_TEXTURE_GEN_S: case D_TEXTURE_GEN_T: case D_TEXTURE_GEN_R: case D_TEXTURE_GEN_Q: U.gen[cap - D_TEXTURE_GEN_S] = on; break;
    default:
      if (cap >= D_LIGHT0 && cap < D_LIGHT0 + MAX_LIGHTS) { s_light[cap - D_LIGHT0].on = on; ++s_verLight; }
      else return; // other desktop-only caps (normalize, clip planes, smoothing, ARB programs): no effect yet
  }
  s_stateDirty = true;
}
static void APIENTRY f_glEnable(GLenum cap) { SetCap(cap, true); }
static void APIENTRY f_glDisable(GLenum cap) { SetCap(cap, false); }
static GLboolean APIENTRY f_glIsEnabled(GLenum cap)
{
  if (PassThroughCap(cap)) return r_glIsEnabled(cap);
  const SUnit& U = s_unit[s_activeUnit < MAX_UNITS ? s_activeUnit : 0];
  switch (cap)
  {
    case GL_TEXTURE_2D: return U.en2D; case D_TEXTURE_CUBE_MAP: return U.enCube; case D_TEXTURE_3D: return U.en3D;
    case D_ALPHA_TEST: return s_alphaTest; case D_FOG: return s_fog; case D_LIGHTING: return s_lighting;
    case D_COLOR_MATERIAL: return s_colorMaterial;
  }
  if (cap >= D_LIGHT0 && cap < D_LIGHT0 + MAX_LIGHTS) return s_light[cap - D_LIGHT0].on;
  return GL_FALSE;
}

static void APIENTRY f_glActiveTexture(GLenum unit)
{
  s_activeUnit = (int)(unit - GL_TEXTURE0);
  r_glActiveTexture(unit);
}

///////////////////////////////////////////////////////////////////////////////
// Texture environment, texgen, alpha test, fog, lighting
///////////////////////////////////////////////////////////////////////////////
static STexEnv& CurEnv() { return s_unit[s_activeUnit < MAX_UNITS ? s_activeUnit : 0].env; }
// Log each texture-environment value the core does not model, once.
static void NoteUnknownEnv(const char* what, GLenum pname, GLenum v)
{
  static std::unordered_map<uint64_t, bool> s_seen;
  const uint64_t key = ((uint64_t)pname << 32) | v;
  if (s_seen.emplace(key, true).second)
    FFPLog("GLES3FFP: unmodelled texenv %s pname=0x%x value=0x%x", what, pname, v);
}
static bool KnownEnvMode(GLenum m) { return m == D_MODULATE || m == D_REPLACE || m == D_DECAL || m == D_ADD || m == D_BLEND || m == D_COMBINE; }
static bool KnownComb(GLenum f) { return f == D_REPLACE || f == D_MODULATE || f == D_ADD || f == D_ADD_SIGNED || f == D_INTERPOLATE || f == D_SUBTRACT || f == D_DOT3_RGB || f == D_DOT3_RGBA || f == D_DOT3_RGB_EXT || f == D_DOT3_RGBA_EXT; }
static bool KnownSrc(GLenum s) { return s == D_TEXTURE || s == D_CONSTANT || s == D_PRIMARY_COLOR || s == D_PREVIOUS || (s >= D_TEXTURE0 && s < D_TEXTURE0 + MAX_UNITS); }

static void TexEnvValue(GLenum pname, float v)
{
  STexEnv& e = CurEnv();
  GLenum ev = (GLenum)v;
  switch (pname)
  {
    case D_TEXTURE_ENV_MODE: if (!KnownEnvMode(ev)) NoteUnknownEnv("mode", pname, ev); e.mode = ev; break;
    case D_COMBINE_RGB: if (!KnownComb(ev)) NoteUnknownEnv("combine", pname, ev); e.combRGB = ev; break;
    case D_COMBINE_ALPHA: if (!KnownComb(ev)) NoteUnknownEnv("combine", pname, ev); e.combA = ev; break;
    case D_RGB_SCALE:
      if (v >= 3.9f && getenv("FARCRY_GLES3_TRACE_SCALE4")) { static int n = 0; if (n++ < 3) { void* bt[16]; int c = backtrace(bt, 16); fprintf(stderr, "GLES3FFP RGB_SCALE=4 unit %d\n", s_activeUnit); backtrace_symbols_fd(bt, c, 2); } }
      e.scaleRGB = v; ++s_verEnv[s_activeUnit < MAX_UNITS ? s_activeUnit : 0]; return; // uniform, not part of the key
    case D_ALPHA_SCALE: e.scaleA = v; ++s_verEnv[s_activeUnit < MAX_UNITS ? s_activeUnit : 0]; return;
    default:
      if ((pname >= D_SOURCE0_RGB && pname < D_SOURCE0_RGB + 4) || (pname >= D_SOURCE0_ALPHA && pname < D_SOURCE0_ALPHA + 4))
        if (!KnownSrc(ev)) NoteUnknownEnv("source", pname, ev);
      if (pname == D_SOURCE0_RGB + 3 || pname == D_SOURCE0_ALPHA + 3 || pname == D_OPERAND0_RGB + 3 || pname == D_OPERAND0_ALPHA + 3)
        NoteUnknownEnv("combine4 slot", pname, ev);
      if (pname >= D_SOURCE0_RGB && pname < D_SOURCE0_RGB + 3) e.srcRGB[pname - D_SOURCE0_RGB] = ev;
      else if (pname >= D_SOURCE0_ALPHA && pname < D_SOURCE0_ALPHA + 3) e.srcA[pname - D_SOURCE0_ALPHA] = ev;
      else if (pname >= D_OPERAND0_RGB && pname < D_OPERAND0_RGB + 3) e.opRGB[pname - D_OPERAND0_RGB] = ev;
      else if (pname >= D_OPERAND0_ALPHA && pname < D_OPERAND0_ALPHA + 3) e.opA[pname - D_OPERAND0_ALPHA] = ev;
      else { NoteUnknownEnv("pname", pname, ev); return; }
  }
  s_stateDirty = true;
}
static void APIENTRY f_glTexEnvi(GLenum target, GLenum pname, GLint v) { if (target == D_TEXTURE_ENV) TexEnvValue(pname, (float)v); }
static void APIENTRY f_glTexEnvf(GLenum target, GLenum pname, GLfloat v) { if (target == D_TEXTURE_ENV) TexEnvValue(pname, v); }
static void APIENTRY f_glTexEnvfv(GLenum target, GLenum pname, const GLfloat* v)
{
  if (target != D_TEXTURE_ENV) return;
  if (pname == D_TEXTURE_ENV_COLOR)
  {
    memcpy(CurEnv().color, v, 16);
    if (s_activeUnit == 0 && s_forceEnvAlpha0 >= 0) CurEnv().color[3] = s_forceEnvAlpha0;
    ++s_verEnv[s_activeUnit < MAX_UNITS ? s_activeUnit : 0];
  }
  else TexEnvValue(pname, v[0]);
}
static void APIENTRY f_glTexEnviv(GLenum target, GLenum pname, const GLint* v)
{
  if (target != D_TEXTURE_ENV) return;
  if (pname == D_TEXTURE_ENV_COLOR) { for (int i = 0; i < 4; ++i) CurEnv().color[i] = v[i] / 2147483647.0f; ++s_verEnv[s_activeUnit < MAX_UNITS ? s_activeUnit : 0]; }
  else TexEnvValue(pname, (float)v[0]);
}

static void APIENTRY f_glTexGeni(GLenum coord, GLenum pname, GLint v)
{
  if (pname != D_TEXTURE_GEN_MODE || coord < D_S || coord > D_Q) return;
  s_unit[s_activeUnit < MAX_UNITS ? s_activeUnit : 0].genMode[coord - D_S] = (GLenum)v;
  s_stateDirty = true;
}
static void APIENTRY f_glTexGenf(GLenum coord, GLenum pname, GLfloat v) { f_glTexGeni(coord, pname, (GLint)v); }
static void APIENTRY f_glTexGenfv(GLenum coord, GLenum pname, const GLfloat* v)
{
  if (coord < D_S || coord > D_Q) return;
  SUnit& U = s_unit[s_activeUnit < MAX_UNITS ? s_activeUnit : 0];
  const int c = coord - D_S;
  ++s_verGen[s_activeUnit < MAX_UNITS ? s_activeUnit : 0];
  if (pname == D_OBJECT_PLANE) memcpy(U.objPlane[c], v, 16);
  else if (pname == D_EYE_PLANE)
  {
    // Eye planes are transformed by the inverse modelview at specification time.
    float inv[16]; MatInverse(inv, Modelview());
    for (int j = 0; j < 4; ++j)
      U.eyePlane[c][j] = v[0]*inv[j*4+0] + v[1]*inv[j*4+1] + v[2]*inv[j*4+2] + v[3]*inv[j*4+3];
  }
  else if (pname == D_TEXTURE_GEN_MODE) f_glTexGeni(coord, pname, (GLint)v[0]);
}

static void APIENTRY f_glAlphaFunc(GLenum func, GLclampf ref) { if (func != s_alphaFunc) s_stateDirty = true; if (ref != s_alphaRef) ++s_verAlpha; s_alphaFunc = func; s_alphaRef = ref; }

static void FogValue(GLenum pname, float v)
{
  ++s_verFog;
  switch (pname)
  {
    case D_FOG_MODE: if ((GLenum)v != s_fogMode) s_stateDirty = true; s_fogMode = (GLenum)v; break;
    case D_FOG_START: s_fogStart = v; break;
    case D_FOG_END: s_fogEnd = v; break;
    case D_FOG_DENSITY: s_fogDensity = v; break;
  }
}
static void APIENTRY f_glFogf(GLenum pname, GLfloat v) { FogValue(pname, v); }
static void APIENTRY f_glFogi(GLenum pname, GLint v) { FogValue(pname, (float)v); }
static void APIENTRY f_glFogfv(GLenum pname, const GLfloat* v) { if (pname == D_FOG_COLOR) { memcpy(s_fogColor, v, 16); ++s_verFog; } else FogValue(pname, v[0]); }
static void APIENTRY f_glFogiv(GLenum pname, const GLint* v) { if (pname != D_FOG_COLOR) FogValue(pname, (float)v[0]); }

static void APIENTRY f_glLightfv(GLenum light, GLenum pname, const GLfloat* v)
{
  if (light < D_LIGHT0 || light >= D_LIGHT0 + MAX_LIGHTS) return;
  ++s_verLight;
  SLight& L = s_light[light - D_LIGHT0];
  switch (pname)
  {
    case D_AMBIENT: memcpy(L.amb, v, 16); break;
    case D_DIFFUSE: memcpy(L.diff, v, 16); break;
    case D_SPECULAR: memcpy(L.spec, v, 16); break;
    case D_POSITION:
    {
      const float* m = Modelview();
      for (int i = 0; i < 4; ++i) L.pos[i] = m[0*4+i]*v[0] + m[1*4+i]*v[1] + m[2*4+i]*v[2] + m[3*4+i]*v[3];
      break;
    }
    case D_CONSTANT_ATTENUATION: L.att[0] = v[0]; break;
    case D_LINEAR_ATTENUATION: L.att[1] = v[0]; break;
    case D_QUADRATIC_ATTENUATION: L.att[2] = v[0]; break;
  }
}
static void APIENTRY f_glLightf(GLenum light, GLenum pname, GLfloat v) { f_glLightfv(light, pname, &v); }
static void APIENTRY f_glLightModelfv(GLenum pname, const GLfloat* v) { if (pname == D_LIGHT_MODEL_AMBIENT) { memcpy(s_sceneAmb, v, 16); ++s_verLight; } }
static void APIENTRY f_glLightModeli(GLenum, GLint) {}
static void APIENTRY f_glMaterialfv(GLenum, GLenum pname, const GLfloat* v)
{
  ++s_verLight;
  switch (pname)
  {
    case D_AMBIENT: memcpy(s_matAmb, v, 16); break;
    case D_DIFFUSE: memcpy(s_matDiff, v, 16); break;
    case D_AMBIENT_AND_DIFFUSE: memcpy(s_matAmb, v, 16); memcpy(s_matDiff, v, 16); break;
    case D_EMISSION: memcpy(s_matEmis, v, 16); break;
  }
}
static void APIENTRY f_glMaterialf(GLenum face, GLenum pname, GLfloat v) { f_glMaterialfv(face, pname, &v); }
static void APIENTRY f_glColorMaterial(GLenum, GLenum) {}

///////////////////////////////////////////////////////////////////////////////
// Textures
///////////////////////////////////////////////////////////////////////////////
static GLuint BoundTexture(GLenum target)
{
  int u = s_activeUnit < MAX_UNITS ? s_activeUnit : 0;
  return target == GL_TEXTURE_2D ? s_bound2D[u] : s_boundCube[u];
}
static void APIENTRY f_glBindTexture(GLenum target, GLuint tex)
{
  int u = s_activeUnit < MAX_UNITS ? s_activeUnit : 0;
  if (target == D_TEXTURE_RECTANGLE) target = GL_TEXTURE_2D;
  if (target == GL_TEXTURE_2D) s_bound2D[u] = tex; else if (target == D_TEXTURE_CUBE_MAP) s_boundCube[u] = tex;
  r_glBindTexture(target, tex);
}

static GLenum TexTarget(GLenum t) { return t == D_TEXTURE_RECTANGLE ? GL_TEXTURE_2D : t; }

static void APIENTRY f_glTexParameteri(GLenum target, GLenum pname, GLint v) { GLTex::Param(BoundTexture(TexTarget(target)), target, pname, v); }
static void APIENTRY f_glTexParameterf(GLenum target, GLenum pname, GLfloat v) { GLTex::ParamF(BoundTexture(TexTarget(target)), target, pname, v); }
static void APIENTRY f_glTexParameteriv(GLenum target, GLenum pname, const GLint* v) { f_glTexParameteri(target, pname, v[0]); }
static void APIENTRY f_glTexParameterfv(GLenum target, GLenum pname, const GLfloat* v) { f_glTexParameterf(target, pname, v[0]); }

// GLTexUpload backend of the layer: uploads target the texture bound on the
// active unit (re-bound around the operation, the engine's binding restored).
static GLenum s_texOpTarget = 0;
static void LayerTexBind(unsigned target, unsigned tex)
{
  GLenum t = TexTarget(target);
  if (t >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && t <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) t = GL_TEXTURE_CUBE_MAP;
  s_texOpTarget = t;
  r_glBindTexture(t, tex);
}
static void LayerTexDone()
{
  if (s_texOpTarget) r_glBindTexture(s_texOpTarget, BoundTexture(s_texOpTarget));
  s_texOpTarget = 0;
}
static void LayerUploadStats(double ms, uint64_t bytes) { s_ticksUp += (Uint64)(ms * SDL_GetPerformanceFrequency() / 1000.0); s_upBytes += bytes; }
static void LayerTexLog(const char* m) { FFPLog("%s", m); }
static GLTex::Backend LayerTexBackend()
{
  GLTex::Backend b; b.bind = LayerTexBind; b.done = LayerTexDone; b.uploadStats = LayerUploadStats; b.log = LayerTexLog;
  return b;
}
// Uploads: shared with the native backend (GLTexUpload.cpp); the layer
// passes the texture bound on the active unit.
static void APIENTRY f_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void* pixels)
{ NoteTexUpload(); GLTex::Image2D(BoundTexture(TexTarget(target)), target, level, internal, w, h, border, format, type, pixels); }
static void APIENTRY f_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels)
{ NoteTexUpload(); GLTex::SubImage2D(BoundTexture(TexTarget(target)), target, level, x, y, w, h, format, type, pixels); }
static void APIENTRY f_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border, GLsizei size, const void* data)
{ NoteTexUpload(); GLTex::CompressedImage2D(BoundTexture(TexTarget(target)), target, level, internal, w, h, border, size, data); }
static void APIENTRY f_glCompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLsizei size, const void* data)
{ NoteTexUpload(); GLTex::CompressedSubImage2D(BoundTexture(TexTarget(target)), target, level, x, y, w, h, fmt, size, data); }

// Read-back with desktop format conversion: shared with the native backend (GLTexUpload.cpp).
static void APIENTRY f_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels)
{ GLTex::ReadPixels(x, y, w, h, format, type, pixels); }

// Texture names: allocated by GLTexUpload.cpp (outside the engine's own range).
static void APIENTRY f_glGenTextures(GLsizei n, GLuint* out) { GLTex::GenNames(n, out); }
static void APIENTRY f_glDeleteTextures(GLsizei n, const GLuint* tex) { GLTex::DeleteNames(n, tex); }
static void PumpTextures(bool block) { GLTex::Pump(block); }


static Uint32 s_loadEndTicks = 0;

void GLES3FFPPumpTextures()
{
  // Shortly after a level load the engine still precaches resources; finish
  // their conversions before drawing instead of showing placeholders.
  const bool afterLoad = s_loadEndTicks && SDL_GetTicks() - s_loadEndTicks < 10000;
  PumpTextures(afterLoad && TexConv_Pending() > 0);
}

// Build every program recorded in the cache index (from binaries: fast), so
// gameplay never compiles a program that an earlier run already needed.
static void PrewarmPrograms()
{
  const std::string& dir = ProgCacheDir();
  if (dir.empty()) return;
  FILE* f = fopen((dir + "/keys.bin").c_str(), "rb");
  if (!f) return;
  const Uint64 t0 = SDL_GetPerformanceCounter();
  const unsigned c0 = s_progCompiled, b0 = s_progFromCache;
  SKey k; int added = 0;
  while (fread(&k, sizeof(k), 1, f) == 1)
  {
    std::string key((const char*)&k, sizeof(k));
    if (s_programs.find(key) != s_programs.end()) continue;
    s_programs.emplace(key, BuildProgram(k)); ++added;
  }
  fclose(f);
  if (r_glUseProgram) r_glUseProgram(s_boundProg);
  FFPLog("GLES3FFP: prewarmed %d program(s) (%u from binary cache, %u compiled) in %.0f ms", added,
         s_progFromCache - b0, s_progCompiled - c0, (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency());
}

void GLES3FFPFinishTextures()
{
  PrewarmPrograms();
  PumpTextures(true);
  s_loadEndTicks = SDL_GetTicks();
  if (!s_loadEndTicks) s_loadEndTicks = 1;
}

///////////////////////////////////////////////////////////////////////////////
// Queries and simple desktop-only calls
///////////////////////////////////////////////////////////////////////////////
static void APIENTRY f_glGetFloatv(GLenum pname, GLfloat* v)
{
  switch (pname)
  {
    case D_MODELVIEW_MATRIX: memcpy(v, Modelview(), 64); return;
    case D_PROJECTION_MATRIX: memcpy(v, Projection(), 64); return;
    case D_TEXTURE_MATRIX: memcpy(v, TexMatrix(s_activeUnit < MAX_UNITS ? s_activeUnit : 0), 64); return;
    case D_CURRENT_COLOR: memcpy(v, s_curColor, 16); return;
    case D_FOG_COLOR: memcpy(v, s_fogColor, 16); return;
  }
  r_glGetFloatv(pname, v);
}
static void APIENTRY f_glGetDoublev(GLenum pname, GLdouble* v)
{
  float f[16] = {0};
  f_glGetFloatv(pname, f);
  int n = (pname == D_MODELVIEW_MATRIX || pname == D_PROJECTION_MATRIX || pname == D_TEXTURE_MATRIX) ? 16 : 4;
  for (int i = 0; i < n; ++i) v[i] = f[i];
}
static void APIENTRY f_glGetIntegerv(GLenum pname, GLint* v)
{
  switch (pname)
  {
    case D_MAX_TEXTURE_UNITS: case D_MAX_TEXTURE_COORDS: *v = MAX_UNITS; return;
    case D_MATRIX_MODE: *v = s_matrixMode == 1 ? D_PROJECTION : (s_matrixMode == 2 ? D_TEXTURE : D_MODELVIEW); return;
    // Desktop fixed-function limits (INVALID_ENUM on ES): report what this
    // layer emulates.
    case 0x0D32: /* GL_MAX_CLIP_PLANES */ *v = 6; return;
    case 0x0D31: /* GL_MAX_LIGHTS */ *v = 8; return;
    case 0x0D36: /* GL_MAX_MODELVIEW_STACK_DEPTH */ *v = 32; return;
    case 0x0D38: /* GL_MAX_PROJECTION_STACK_DEPTH */ *v = 4; return;
    case 0x0D39: /* GL_MAX_TEXTURE_STACK_DEPTH */ *v = 4; return;
  }
  r_glGetIntegerv(pname, v);
}

static void APIENTRY f_glClearDepth(GLdouble d) { r_glClearDepthf((float)d); }
static void APIENTRY f_glDepthRange(GLdouble n, GLdouble f) { r_glDepthRangef((float)n, (float)f); }
static void APIENTRY f_glNoop1(GLenum) {}
static void APIENTRY f_glNoop2(GLenum, GLenum) {}
static void APIENTRY f_glPointSize(GLfloat) {}
static void APIENTRY f_glClipPlane(GLenum, const GLdouble*) {}

///////////////////////////////////////////////////////////////////////////////
// Table
///////////////////////////////////////////////////////////////////////////////
struct SOverride { const char* name; void* fn; };
#define O(n, f) { n, (void*)&f }
// Diagnostic: FARCRY_GLES3_EQUAL_AS_LEQUAL=1 relaxes GL_EQUAL depth tests.
// Water fallback (GLRendPipeline): force stage-0 constant alpha while set.
void GLES3FFPForceEnvAlpha0(float a)
{
  s_forceEnvAlpha0 = a;
  if (a >= 0) { s_unit[0].env.color[3] = a; ++s_verEnv[0]; }
}
// Desktop-only hints (perspective correction, fog, point/line smooth,
// multisample filter) are INVALID_ENUM on ES; pass only the ES ones.
static void APIENTRY f_glHint(GLenum target, GLenum mode)
{
  if (target == GL_GENERATE_MIPMAP_HINT || target == GL_FRAGMENT_SHADER_DERIVATIVE_HINT) r_glHint(target, mode);
}
static void APIENTRY f_glDepthFunc(GLenum f)
{
  static const bool s_relax = getenv("FARCRY_GLES3_EQUAL_AS_LEQUAL") != NULL;
  if (s_relax && f == GL_EQUAL) f = GL_LEQUAL;
  r_glDepthFunc(f);
}
static void APIENTRY f_glBufferData(GLenum t, GLsizeiptr n, const void* d, GLenum u)
{
  SUpScope sc((uint64_t)n);
  if (!s_bufRings.empty() && (t == GL_ARRAY_BUFFER || t == GL_ELEMENT_ARRAY_BUFFER))
  {
    const GLuint eb = BoundBufferFor(t);
    if (s_bufRings.count(eb)) { DropRing(eb); r_glBindBuffer(t, eb); } // back to the engine's own storage
  }
  auto it = s_shadowBufs.find(BoundBufferFor(t));
  if (it != s_shadowBufs.end()) { it->second.data.assign(n, 0); if (d) memcpy(it->second.data.data(), d, n); it->second.usage = u; }
  r_glBufferData(t, n, d, u);
}
static void APIENTRY f_glBufferSubData(GLenum t, GLintptr o, GLsizeiptr n, const void* d)
{
  SUpScope sc((uint64_t)n);
  auto it = s_shadowBufs.find(BoundBufferFor(t));
  if (it != s_shadowBufs.end() && d && (size_t)(o + n) <= it->second.data.size()) memcpy(it->second.data.data() + o, d, n);
  r_glBufferSubData(t, o, n, d);
}
static void APIENTRY f_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
  SUpScope sc((uint64_t)w * h * 4); r_glCopyTexSubImage2D(TexTarget(t), l, xo, yo, x, y, w, h); }

static const SOverride s_overrides[] =
{
  O("glDrawArrays", f_glDrawArrays), O("glDrawElements", f_glDrawElements),
  O("glDrawRangeElements", f_glDrawRangeElements), O("glDrawRangeElementsEXT", f_glDrawRangeElements),
  O("glBegin", f_glBegin), O("glEnd", f_glEnd),
  O("glVertex2f", f_glVertex2f), O("glVertex2fv", f_glVertex2fv), O("glVertex2i", f_glVertex2i),
  O("glVertex3f", f_glVertex3f), O("glVertex3fv", f_glVertex3fv), O("glVertex4f", f_glVertex4f), O("glVertex4fv", f_glVertex4fv),
  O("glColor3f", f_glColor3f), O("glColor3fv", f_glColor3fv), O("glColor4f", f_glColor4f), O("glColor4fv", f_glColor4fv),
  O("glColor3ub", f_glColor3ub), O("glColor3ubv", f_glColor3ubv), O("glColor4ub", f_glColor4ub), O("glColor4ubv", f_glColor4ubv),
  O("glTexCoord1f", f_glTexCoord1f), O("glTexCoord2f", f_glTexCoord2f), O("glTexCoord2fv", f_glTexCoord2fv),
  O("glTexCoord3f", f_glTexCoord3f), O("glTexCoord3fv", f_glTexCoord3fv), O("glTexCoord4f", f_glTexCoord4f), O("glTexCoord4fv", f_glTexCoord4fv),
  O("glMultiTexCoord2fARB", f_glMultiTexCoord2f), O("glMultiTexCoord2fvARB", f_glMultiTexCoord2fv),
  O("glMultiTexCoord3fARB", f_glMultiTexCoord3f), O("glMultiTexCoord3fvARB", f_glMultiTexCoord3fv),
  O("glMultiTexCoord4fARB", f_glMultiTexCoord4f), O("glMultiTexCoord4fvARB", f_glMultiTexCoord4fv),
  O("glNormal3f", f_glNormal3f), O("glNormal3fv", f_glNormal3fv),
  O("glVertexPointer", f_glVertexPointer), O("glColorPointer", f_glColorPointer), O("glNormalPointer", f_glNormalPointer),
  O("glTexCoordPointer", f_glTexCoordPointer), O("glSecondaryColorPointerEXT", f_glSecondaryColorPointer),
  O("glEnableClientState", f_glEnableClientState), O("glDisableClientState", f_glDisableClientState),
  O("glClientActiveTextureARB", f_glClientActiveTexture), O("glClientActiveTexture", f_glClientActiveTexture),
  O("glBindBuffer", f_glBindBuffer), O("glBindBufferARB", f_glBindBuffer),
  O("glMapBufferARB", f_glMapBuffer), O("glMapBuffer", f_glMapBuffer), O("glUnmapBuffer", f_glUnmapBuffer), O("glUnmapBufferARB", f_glUnmapBuffer),
  O("glDeleteBuffers", f_glDeleteBuffers), O("glDeleteBuffersARB", f_glDeleteBuffers), O("glUseProgram", f_glUseProgram),
  O("glVertexAttribPointer", f_glVertexAttribPointer), O("glVertexAttribPointerARB", f_glVertexAttribPointer),
  O("glEnableVertexAttribArray", f_glEnableVertexAttribArray), O("glEnableVertexAttribArrayARB", f_glEnableVertexAttribArray),
  O("glDisableVertexAttribArray", f_glDisableVertexAttribArray), O("glDisableVertexAttribArrayARB", f_glDisableVertexAttribArray),
  O("glMatrixMode", f_glMatrixMode), O("glLoadIdentity", f_glLoadIdentity), O("glLoadMatrixf", f_glLoadMatrixf),
  O("glLoadMatrixd", f_glLoadMatrixd), O("glMultMatrixf", f_glMultMatrixf), O("glMultMatrixd", f_glMultMatrixd),
  O("glPushMatrix", f_glPushMatrix), O("glPopMatrix", f_glPopMatrix), O("glTranslatef", f_glTranslatef),
  O("glTranslated", f_glTranslated), O("glScalef", f_glScalef), O("glScaled", f_glScaled), O("glRotatef", f_glRotatef),
  O("glRotated", f_glRotated), O("glOrtho", f_glOrtho), O("glFrustum", f_glFrustum),
  O("glEnable", f_glEnable), O("glDisable", f_glDisable), O("glIsEnabled", f_glIsEnabled),
  O("glActiveTexture", f_glActiveTexture), O("glActiveTextureARB", f_glActiveTexture),
  O("glTexEnvi", f_glTexEnvi), O("glTexEnvf", f_glTexEnvf), O("glTexEnvfv", f_glTexEnvfv), O("glTexEnviv", f_glTexEnviv),
  O("glTexGeni", f_glTexGeni), O("glTexGenf", f_glTexGenf), O("glTexGenfv", f_glTexGenfv),
  O("glAlphaFunc", f_glAlphaFunc),
  O("glFogf", f_glFogf), O("glFogi", f_glFogi), O("glFogfv", f_glFogfv), O("glFogiv", f_glFogiv),
  O("glLightf", f_glLightf), O("glLightfv", f_glLightfv), O("glLightModelfv", f_glLightModelfv), O("glLightModeli", f_glLightModeli),
  O("glMaterialf", f_glMaterialf), O("glMaterialfv", f_glMaterialfv), O("glColorMaterial", f_glColorMaterial),
  O("glBindTexture", f_glBindTexture), O("glTexParameteri", f_glTexParameteri), O("glTexParameterf", f_glTexParameterf),
  O("glTexParameteriv", f_glTexParameteriv), O("glTexParameterfv", f_glTexParameterfv),
  O("glDepthFunc", f_glDepthFunc), O("glHint", f_glHint), O("glBufferData", f_glBufferData), O("glBufferDataARB", f_glBufferData), O("glBufferSubData", f_glBufferSubData), O("glBufferSubDataARB", f_glBufferSubData),
  O("glCopyTexSubImage2D", f_glCopyTexSubImage2D), O("glTexImage2D", f_glTexImage2D), O("glTexSubImage2D", f_glTexSubImage2D),
  O("glGenTextures", f_glGenTextures), O("glDeleteTextures", f_glDeleteTextures), O("glReadPixels", f_glReadPixels),
  O("glCompressedTexImage2D", f_glCompressedTexImage2D), O("glCompressedTexImage2DARB", f_glCompressedTexImage2D),
  O("glCompressedTexSubImage2D", f_glCompressedTexSubImage2D), O("glCompressedTexSubImage2DARB", f_glCompressedTexSubImage2D),
  O("glGetFloatv", f_glGetFloatv), O("glGetDoublev", f_glGetDoublev), O("glGetIntegerv", f_glGetIntegerv),
  O("glClearDepth", f_glClearDepth), O("glDepthRange", f_glDepthRange),
  O("glShadeModel", f_glNoop1), O("glDrawBuffer", f_glNoop1), O("glPolygonMode", f_glNoop2),
  O("glPointSize", f_glPointSize), O("glClipPlane", f_glClipPlane),
  { NULL, NULL }
};
#undef O

void* GLES3FFPOverride(const char* name)
{
  for (int i = 0; s_overrides[i].name; ++i)
    if (!strcmp(s_overrides[i].name, name))
      return s_overrides[i].fn;
  return NULL;
}

bool GLES3FFPInit(void (*log)(const char*))
{
  s_log = log;
  bool ok = true;
#define FFP_LOAD_REAL(n) r_##n = (decltype(r_##n))SDL_GL_GetProcAddress(#n); if (!r_##n) { FFPLog("GLES3FFP: missing driver entry point %s", #n); ok = false; }
  FFP_REAL_LIST(FFP_LOAD_REAL)
#undef FFP_LOAD_REAL
  for (int i = 0; i < 2 + MAX_UNITS; ++i) { s_stack[i].assign(1, SMat()); MatIdentity(s_stack[i].back().m); }
  for (int u = 0; u < MAX_UNITS; ++u)
  {
    SUnit& U = s_unit[u]; memset(&U, 0, sizeof(U));
    U.env.mode = D_MODULATE; U.env.combRGB = U.env.combA = D_MODULATE;
    U.env.srcRGB[0] = U.env.srcA[0] = D_TEXTURE; U.env.srcRGB[1] = U.env.srcA[1] = D_PREVIOUS; U.env.srcRGB[2] = U.env.srcA[2] = D_CONSTANT;
    U.env.opRGB[0] = U.env.opRGB[1] = D_SRC_COLOR; U.env.opRGB[2] = D_SRC_ALPHA;
    U.env.opA[0] = U.env.opA[1] = U.env.opA[2] = D_SRC_ALPHA;
    U.env.scaleRGB = U.env.scaleA = 1.0f;
    for (int c = 0; c < 4; ++c) { U.genMode[c] = D_EYE_LINEAR; U.objPlane[c][c] = 1; U.eyePlane[c][c] = 1; }
    s_curTex[u][3] = 1.0f;
  }
  for (int i = 0; i < MAX_LIGHTS; ++i)
  {
    SLight& L = s_light[i]; memset(&L, 0, sizeof(L));
    L.pos[2] = 1; L.amb[3] = 1; L.att[0] = 1;
    if (i == 0) { L.diff[0] = L.diff[1] = L.diff[2] = L.diff[3] = 1; }
  }
  r_glBindVertexArray(0); // client-side arrays require the default vertex array object
  {
    const char* ext = r_glGetString ? (const char*)r_glGetString(GL_EXTENSIONS) : NULL;
    const bool s3tc = ext && (strstr(ext, "GL_EXT_texture_compression_s3tc") || strstr(ext, "GL_EXT_texture_compression_dxt1"));
    const bool s_decodeDXT = !s3tc || getenv("FARCRY_GLES3_FORCE_DXT_DECODE");
    const bool dxtRGBA8 = getenv("FARCRY_GLES3_DXT_DECODE_RGBA8") != NULL;
    double s_uploadBudgetMs = 2.0;
    const char* conv = getenv("FARCRY_GLES3_TEXCONV");
    if (s_decodeDXT && (!conv || atoi(conv) != 0))
    {
      const char* th = getenv("FARCRY_TEXCONV_THREADS");
      if (const char* b = getenv("FARCRY_TEXCONV_UPLOAD_MS")) s_uploadBudgetMs = atof(b);
      TexConv_Start(th ? atoi(th) : 1, getenv("FARCRY_TEXCACHE"), s_log);
    }
    GLTex::Init(LayerTexBackend(), s_decodeDXT, dxtRGBA8, s_uploadBudgetMs);
    FFPLog("GLES3FFP: S3TC/DXT %s by the driver%s", s3tc ? "supported" : "NOT supported",
           s_decodeDXT ? "; DXT textures are converted (ETC2 in background, 16-bit fallback)" : "");
  }
  s_stateDirty = true;
  FFPLog("GLES3FFP: fixed-function core ready (%s)", ok ? "all driver entry points found" : "some entry points missing");
  return ok;
}

void GLES3FFPShutdown()
{
  TexConv_Stop();
  for (auto& it : s_programs) if (r_glDeleteProgram) r_glDeleteProgram(it.second.prog);
  s_programs.clear();
  s_curProg = NULL;
  if (s_immVBO && r_glDeleteBuffers) { r_glDeleteBuffers(1, &s_immVBO); s_immVBO = 0; }
}
