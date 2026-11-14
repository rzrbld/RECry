/*=============================================================================
  GLRCore.h : internal to the native backend (GLR*.cpp). Includes the real
  GLES 3 header; engine files include GLR.h only.
=============================================================================*/
#pragma once
#include <GLES3/gl3.h>
#include "GLR.h"
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace GLR
{
void Log(const char* fmt, ...);

// ---------------------------------------------------------------- driver entry points
struct Funcs
{
#define GLR_FUNCS(X) \
  X(glUseProgram) X(glCreateShader) X(glShaderSource) X(glCompileShader) X(glGetShaderiv) X(glGetShaderInfoLog) \
  X(glCreateProgram) X(glAttachShader) X(glBindAttribLocation) X(glLinkProgram) X(glGetProgramiv) X(glGetProgramInfoLog) \
  X(glDeleteShader) X(glDeleteProgram) X(glGetUniformLocation) X(glUniform1i) X(glUniform1f) X(glUniform2fv) X(glUniform3fv) \
  X(glUniform4fv) X(glUniformMatrix3fv) X(glUniformMatrix4fv) X(glProgramBinary) X(glGetProgramBinary) X(glProgramParameteri) \
  X(glGenBuffers) X(glDeleteBuffers) X(glBindBuffer) X(glBufferData) X(glBufferSubData) X(glMapBufferRange) X(glUnmapBuffer) \
  X(glFenceSync) X(glClientWaitSync) X(glDeleteSync) \
  X(glGenVertexArrays) X(glDeleteVertexArrays) X(glBindVertexArray) X(glVertexAttribPointer) X(glEnableVertexAttribArray) \
  X(glDisableVertexAttribArray) X(glVertexAttrib4fv) X(glDrawArrays) X(glDrawElements) X(glDrawRangeElements) \
  X(glActiveTexture) X(glBindTexture) X(glGetString) X(glGetError) X(glViewport) X(glScissor) X(glEnable) X(glDisable) \
  X(glBlendFunc) X(glDepthMask) X(glDepthFunc) X(glColorMask) X(glCullFace) X(glFrontFace) X(glDepthRangef) X(glPolygonOffset) \
  X(glClear) X(glClearColor) X(glClearDepthf) X(glClearStencil) X(glStencilMask) X(glFinish) X(glFlush) X(glGetIntegerv) X(glIsBuffer)
#define GLR_DECLARE(n) decltype(&::n) n = nullptr;
  GLR_FUNCS(GLR_DECLARE)
#undef GLR_DECLARE
};
extern Funcs gl;

// ---------------------------------------------------------------- programs
// Material key: everything that changes the generated GLSL. Built from the
// current stage state at record time.
#pragma pack(push, 1)
struct ProgramKey
{
  uint8_t stages;
  uint8_t co[4], ao[4], ca[4], aa[4];
  uint8_t colorArray, colorSemantic, fog;
  uint8_t alphaTest;     // 0 off, 1 >, 2 <, 3 >=, 4 <=, 5 ==, 6 !=, 7 never
  uint8_t texStream;     // bit u: stage u has a texcoord attribute
  uint8_t lights;        // 0 unlit, else 1 + number of lights
  uint8_t colorMaterial;
  uint8_t gen[4][4];     // per stage, s/t/r/q: 0 none 1 object 2 eye 3 sphere 4 reflection 5 normal
  uint8_t texMat;        // bit u: texture matrix on stage u
  uint8_t cube;          // bit u: stage u samples a cube map
  uint8_t normalArray;   // a normal attribute (else the current normal)
};
#pragma pack(pop)

// Per-draw uniform values (recorded with the draw; uploaded on playback only
// where they differ from what the program last had).
struct DrawUniforms
{
  float mv[16], proj[16];
  float curColor[4];
  float curTex[4][4];    // texcoords of stages without a texcoord stream
  float stageColor[4][4];
  float stageScale[4][2];
  float alphaRef;
  float fog[4];          // start, end, 1/(end-start), density
  float fogColor[3];
  // lighting (eye space), material, scene ambient, normal matrix
  float lightPos[4][4], lightAmb[4][4], lightDiff[4][4], lightAtt[4][3];
  float matAmb[4], matDiff[4], matEmis[4], sceneAmb[4];
  float normalMat[9];
  float curNormal[3];
  // texgen planes (object, eye) and texture matrices per stage
  float genObj[4][16], genEye[4][16], texMat[4][16];
};

struct Program
{
  GLuint prog = 0;
  bool ok = false;
  GLint uMV = -1, uP = -1, uCurColor = -1, uCurTex = -1, uAlphaRef = -1, uFog = -1, uFogColor = -1;
  GLint uStageColor[4] = { -1, -1, -1, -1 }, uStageScale[4] = { -1, -1, -1, -1 };
  GLint uLightPos = -1, uLightAmb = -1, uLightDiff = -1, uLightAtt = -1, uMatAmb = -1, uMatDiff = -1, uMatEmis = -1, uSceneAmb = -1;
  GLint uNormalMat = -1, uCurNormal = -1;
  GLint uGenObj[4] = { -1, -1, -1, -1 }, uGenEye[4] = { -1, -1, -1, -1 }, uTexMat[4] = { -1, -1, -1, -1 };
  DrawUniforms last;     // what the program currently has
  bool fresh = true;
  // Engine Cg programs (GLRCgPrograms.cpp): register arrays and their used size.
  GLint uCv = -1, uCf = -1; int nCv = 0, nCf = 0;
  uint64_t lastCg = ~0ull;  // segment << 32 | snapshot uploaded last
};
Program* GetProgram(const ProgramKey& k);
bool BuildProgram(Program& p, const char* name, const std::string& vs, const std::string& fs);
// Setup of engine (Cg) programs: register array sizes and sampler units.
struct ProgramMeta { int nCv = 0, nCf = 0; std::vector<std::pair<std::string, int>> samplers; };
// Program for this source pair, shared by content; built once (see WarmUp).
Program* ProgramFor(const char* name, const std::string& vs, const std::string& fs, const ProgramMeta* meta);
void ProgramsShutdown();
size_t ProgramCount();

// ---------------------------------------------------------------- recorded commands
// Raster state: everything outside the program that a draw or clear needs.
struct Raster
{
  uint8_t blend, depthTest, depthWrite, stencilTest, cullOn, scissorOn, polyOn, colorMask;   // colorMask: bits r g b a
  uint16_t blendSrc, blendDst, depthFunc, cullFace, frontFace;
  int viewport[4], scissor[4];
  float depthRange[2], poly[2];
};
enum CmdType : uint8_t { CMD_DRAW, CMD_CLEAR, CMD_BUFFER, CMD_CALL };   // CMD_CALL: Segment::calls[first]
struct TexBind { uint8_t target; unsigned name; };
// One vertex attribute of a draw: buffer (or the segment's ring) + format.
struct VtxBinding { GLuint buf; size_t off; uint16_t type; uint8_t size, norm; uint16_t stride; };
const GLuint RING_VTX = 0xFFFFFFFFu, RING_IDX = 0xFFFFFFFEu;
struct Cmd
{
  CmdType type;
  uint8_t prim, colorSemantic;
  uint16_t attrMask;
  Raster r;
  // draw
  Program* prog;
  TexBind tex[4];
  VtxBinding vb[ATTR_COUNT];
  GLuint ibo;                // 0: non-indexed
  size_t iboOffset;
  uint16_t indexType;
  int count, first;          // indices (indexed) or vertices from first
  uint32_t uniforms;         // index into the frame's uniform blocks
  uint32_t cgParams;         // engine program registers snapshot, ~0u: none
  // clear
  unsigned clearMask;
  float clearColor[4], clearDepth; int clearStencil;
  // buffer upload (recorded because pending draws use the buffer)
  GLuint buf; uint16_t bufTarget, bufUsage; uint8_t whole;
  size_t bufOffset, bufSize, arenaOffset;
};

// ---------------------------------------------------------------- engine Cg programs
const int kCgVRegs = 96, kCgFRegs = 32;
struct CgParams { float v[kCgVRegs][4]; float f[kCgFRegs][4]; };
struct CgProgram;                       // GLRCgPrograms.cpp
CgProgram* CgFind(unsigned id, bool vertex);
unsigned CgBound(bool vertex);
void CgMemStats(size_t& glslBytes, int& programs, int& pairs);
// Linked program for this vertex/fragment combination (null side: generated
// from the key, as for fixed function). Null when it can't be built.
// texUnits/texCube: units the fragment program samples (bit per unit).
Program* CgPair(CgProgram* vp, CgProgram* fp, const ProgramKey& k, uint8_t& texUnits, uint8_t& texCube);
std::string BuildVS(const ProgramKey& k);
std::string BuildFS(const ProgramKey& k);

// Frame ring for per-frame vertex/index data (see GLRCore.cpp).
struct RingAlloc { uint8_t* cpu; GLuint buffer; size_t offset; };

const Layout& GetLayout(int id);
}
