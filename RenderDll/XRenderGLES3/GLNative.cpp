/*=============================================================================
  GLNative.cpp : native OpenGL ES 3.0 rendering core. See GLNative.h.
=============================================================================*/
#include "GLNativeCore.h"
#include "GLN2D.h"
#include "GLNMesh.h"
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

namespace GLN
{
Funcs gl;

static bool s_ready = false;
static unsigned s_stages = 0;
static void (*s_log)(const char*) = nullptr;

void Log(const char* fmt, ...)
{
  char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  if (s_log) s_log(b);
  fprintf(stderr, "%s\n", b);   // also on stdout/stderr: the engine log drops some messages during play
}

static unsigned ParseStages(const char* s)
{
  if (!s || !*s) return 0;
  if (!strcmp(s, "all")) return ~0u;
  static const struct { const char* name; unsigned bit; } names[] = {
    { "2d", GLN_STAGE_2D }, { "mesh", GLN_STAGE_MESH }, { "pass", GLN_STAGE_PASS }, { "rt", GLN_STAGE_RT } };
  unsigned m = 0;
  std::string str(s);
  size_t p = 0;
  while (p <= str.size())
  {
    size_t e = str.find(',', p); if (e == std::string::npos) e = str.size();
    const std::string tok = str.substr(p, e - p);
    for (auto& n : names) if (tok == n.name) m |= n.bit;
    p = e + 1;
  }
  return m;
}

// ---------------------------------------------------------------- streams
struct Stream
{
  GLenum target = 0;
  GLuint buffer = 0;
  size_t capacity = 0;
  std::vector<uint8_t> cpu;       // written by callers, uploaded by CommitStreams
  size_t used = 0, committed = 0;
};
static Stream s_vtx, s_idx;

static void StreamReset(Stream& s, GLenum target, size_t initial)
{
  s.target = target;
  if (!s.buffer) { gl.glGenBuffers(1, &s.buffer); s.capacity = 0; }
  if (s.cpu.size() < initial) s.cpu.resize(initial);
  // Orphan: fresh storage each frame, draws still in flight keep the old one.
  gl.glBindBuffer(target, s.buffer);
  gl.glBufferData(target, (GLsizeiptr)s.cpu.size(), nullptr, GL_STREAM_DRAW);
  s.capacity = s.cpu.size();
  s.used = s.committed = 0;
}

static StreamAlloc StreamAllocBytes(Stream& s, size_t bytes)
{
  const size_t off = (s.used + 15) & ~(size_t)15;
  if (off + bytes > s.cpu.size())
  {
    // Grow for the next frame; this frame's overflow goes to a fresh buffer
    // after uploading what was written so far.
    CommitStreams();
    size_t n = s.cpu.size() ? s.cpu.size() : (size_t)1 << 20;
    while (n < bytes * 2) n *= 2;
    s.cpu.resize(n * 2);
    gl.glBindBuffer(s.target, s.buffer);
    gl.glBufferData(s.target, (GLsizeiptr)s.cpu.size(), nullptr, GL_STREAM_DRAW);
    s.capacity = s.cpu.size();
    s.used = s.committed = 0;
    return StreamAllocBytes(s, bytes);
  }
  s.used = off + bytes;
  return StreamAlloc{ s.cpu.data() + off, s.buffer, off };
}

// Streams are created on first use (draws can happen before the first frame
// boundary, e.g. on the loading screen).
StreamAlloc AllocVertices(size_t bytes) { if (!s_vtx.buffer) StreamReset(s_vtx, GL_ARRAY_BUFFER, (size_t)4 << 20); return StreamAllocBytes(s_vtx, bytes); }
StreamAlloc AllocIndices(size_t bytes) { if (!s_idx.buffer) StreamReset(s_idx, GL_ELEMENT_ARRAY_BUFFER, (size_t)1 << 20); return StreamAllocBytes(s_idx, bytes); }

static void StreamCommit(Stream& s)
{
  if (s.used <= s.committed) return;
  gl.glBindBuffer(s.target, s.buffer);
  gl.glBufferSubData(s.target, (GLintptr)s.committed, (GLsizeiptr)(s.used - s.committed), s.cpu.data() + s.committed);
  s.committed = s.used;
}
void CommitStreams() { StreamCommit(s_vtx); StreamCommit(s_idx); }

// ---------------------------------------------------------------- vertex arrays
void Layout::Add(GLNAttr a, int size, GLenum type, bool norm, int offset)
{
  if (count >= GLN_NUM_ATTRS) return;
  attrib[count++] = AttribDesc{ (uint8_t)a, (uint8_t)size, (uint16_t)type, (uint8_t)(norm ? 1 : 0), (uint8_t)offset };
}
uint64_t Layout::Key() const
{
  uint64_t h = 1469598103934665603ULL;
  auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
  mix(stride); mix(count);
  for (int i = 0; i < count; ++i)
    mix(attrib[i].attr | (attrib[i].size << 4) | ((uint64_t)attrib[i].type << 8) | ((uint64_t)attrib[i].normalized << 24) | ((uint64_t)attrib[i].offset << 32));
  return h;
}

struct VaoKey { uint64_t layout; GLuint vbo; size_t off; GLuint ibo; bool operator==(const VaoKey& o) const { return layout == o.layout && vbo == o.vbo && off == o.off && ibo == o.ibo; } };
struct VaoHash { size_t operator()(const VaoKey& k) const { return (size_t)(k.layout ^ ((uint64_t)k.vbo << 32) ^ k.off * 31 ^ ((uint64_t)k.ibo << 16)); } };
static std::unordered_map<VaoKey, GLuint, VaoHash> s_vaos;

// Fast-path state (a copy of the layout: callers pass stack objects).
static uint64_t s_lastLayout = 0; static GLuint s_lastVbo = 0, s_lastIbo = 0, s_lastVao = 0; static size_t s_lastOff = 0;
static Layout s_lastL; static bool s_lastValid = false;

// A deleted buffer's name can be reused for new storage while cached VAOs
// still reference the deleted object: drop every VAO that uses it.
void ForgetBuffer(GLuint b)
{
  if (!b) return;
  for (auto it = s_vaos.begin(); it != s_vaos.end();)
  {
    if (it->first.vbo == b || it->first.ibo == b) { gl.glDeleteVertexArrays(1, &it->second); it = s_vaos.erase(it); }
    else ++it;
  }
  s_lastValid = false;
}

GLuint BindVertexArray(const Layout& l, GLuint vbo, size_t vboOffset, GLuint ibo)
{
  // Streaming geometry changes offset every draw: one VAO per layout for the
  // stream buffers, re-pointed each time; static buffers get a cached VAO.
  const bool streamed = vbo == s_vtx.buffer;
  // Fast path: the same static buffer set as the previous call.
  if (!streamed && s_lastValid && vbo == s_lastVbo && ibo == s_lastIbo && vboOffset == s_lastOff && l.stride == s_lastL.stride && l.count == s_lastL.count &&
      !memcmp(l.attrib, s_lastL.attrib, sizeof(AttribDesc) * l.count))
  {
    gl.glBindVertexArray(s_lastVao);
    return s_lastVao;
  }
  VaoKey key{ l.Key(), vbo, streamed ? 0 : vboOffset, ibo };
  auto it = s_vaos.find(key);
  GLuint vao;
  bool setup = streamed;
  if (it == s_vaos.end()) { gl.glGenVertexArrays(1, &vao); s_vaos[key] = vao; setup = true; }
  else vao = it->second;
  gl.glBindVertexArray(vao);
  if (setup)
  {
    gl.glBindBuffer(GL_ARRAY_BUFFER, vbo);
    bool on[GLN_NUM_ATTRS] = {};
    for (int i = 0; i < l.count; ++i)
    {
      const AttribDesc& a = l.attrib[i];
      gl.glVertexAttribPointer(a.attr, a.size, a.type, a.normalized ? GL_TRUE : GL_FALSE, l.stride, (const void*)(vboOffset + a.offset));
      gl.glEnableVertexAttribArray(a.attr);
      on[a.attr] = true;
    }
    for (int a = 0; a < GLN_NUM_ATTRS; ++a) if (!on[a]) gl.glDisableVertexAttribArray(a);
    if (ibo) gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
  }
  if (!streamed)
  {
    s_lastL = l; s_lastValid = true;
    s_lastLayout = key.layout; s_lastVbo = vbo; s_lastIbo = ibo; s_lastOff = vboOffset; s_lastVao = vao;
  }
  return vao;
}

// ---------------------------------------------------------------- programs
static std::string ProgramCacheDir()
{
  static std::string dir;
  static bool init = false;
  if (!init)
  {
    init = true;
    if (const char* d = getenv("FARCRY_NATIVE_PROGCACHE")) dir = d;
    else if (const char* t = getenv("FARCRY_TEXCACHE")) dir = std::string(t) + "/../native-programs";
    if (!dir.empty()) mkdir(dir.c_str(), 0755);
  }
  return dir;
}

static uint64_t Fnv(const std::string& s, uint64_t h = 1469598103934665603ULL)
{
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
  return h;
}

static GLuint Compile(GLenum type, const std::string& src, const char* name)
{
  GLuint sh = gl.glCreateShader(type);
  const char* p = src.c_str();
  gl.glShaderSource(sh, 1, &p, nullptr);
  gl.glCompileShader(sh);
  GLint ok = 0; gl.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[2048] = ""; gl.glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
    Log("GLNative: %s %s shader failed: %s", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
    gl.glDeleteShader(sh);
    return 0;
  }
  return sh;
}

bool Program::Build(const char* name, const std::string& vs, const std::string& fs)
{
  const char* ren = (const char*)gl.glGetString(GL_RENDERER);
  const char* ver = (const char*)gl.glGetString(GL_VERSION);
  const uint64_t h = Fnv(fs, Fnv(vs, Fnv(std::string(ren ? ren : "") + (ver ? ver : ""))));
  const std::string dir = ProgramCacheDir();
  char path[512] = "";
  if (!dir.empty()) snprintf(path, sizeof(path), "%s/%016llx.bin", dir.c_str(), (unsigned long long)h);
  m_prog = gl.glCreateProgram();
  m_uniforms.clear();
  // Cached binary first.
  if (path[0])
    if (FILE* f = fopen(path, "rb"))
    {
      GLenum fmt = 0; std::vector<char> data;
      if (fread(&fmt, sizeof(fmt), 1, f) == 1)
      {
        fseek(f, 0, SEEK_END); const long n = ftell(f) - (long)sizeof(fmt); fseek(f, sizeof(fmt), SEEK_SET);
        if (n > 0) { data.resize(n); if (fread(data.data(), 1, n, f) != (size_t)n) data.clear(); }
      }
      fclose(f);
      if (!data.empty())
      {
        gl.glProgramBinary(m_prog, fmt, data.data(), (GLsizei)data.size());
        GLint ok = 0; gl.glGetProgramiv(m_prog, GL_LINK_STATUS, &ok);
        if (ok) return true;
      }
    }
  GLuint v = Compile(GL_VERTEX_SHADER, vs, name), f = v ? Compile(GL_FRAGMENT_SHADER, fs, name) : 0;
  if (!v || !f) { if (v) gl.glDeleteShader(v); gl.glDeleteProgram(m_prog); m_prog = 0; return false; }
  gl.glAttachShader(m_prog, v); gl.glAttachShader(m_prog, f);
  static const char* attrNames[GLN_NUM_ATTRS] = { "aPos", "aColor", "aNormal", "aTex0", "aTex1", "aTex2", "aTex3", "aColor2" };
  for (int a = 0; a < GLN_NUM_ATTRS; ++a) gl.glBindAttribLocation(m_prog, a, attrNames[a]);
  if (path[0]) gl.glProgramParameteri(m_prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
  gl.glLinkProgram(m_prog);
  gl.glDeleteShader(v); gl.glDeleteShader(f);
  GLint ok = 0; gl.glGetProgramiv(m_prog, GL_LINK_STATUS, &ok);
  if (!ok)
  {
    char log[2048] = ""; gl.glGetProgramInfoLog(m_prog, sizeof(log), nullptr, log);
    Log("GLNative: %s link failed: %s", name, log);
    gl.glDeleteProgram(m_prog); m_prog = 0;
    return false;
  }
  if (path[0])
  {
    GLint len = 0; gl.glGetProgramiv(m_prog, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len > 0)
    {
      std::vector<char> data(len); GLenum fmt = 0; GLsizei got = 0;
      gl.glGetProgramBinary(m_prog, len, &got, &fmt, data.data());
      const std::string tmp = std::string(path) + ".tmp";
      if (FILE* out = fopen(tmp.c_str(), "wb"))
      {
        const bool w = fwrite(&fmt, sizeof(fmt), 1, out) == 1 && fwrite(data.data(), 1, got, out) == (size_t)got;
        fclose(out);
        if (w) rename(tmp.c_str(), path); else remove(tmp.c_str());
      }
    }
  }
  return true;
}

GLint Program::Uniform(const char* name)
{
  auto it = m_uniforms.find(name);
  if (it != m_uniforms.end()) return it->second;
  const GLint loc = m_prog ? gl.glGetUniformLocation(m_prog, name) : -1;
  m_uniforms[name] = loc;
  return loc;
}

void Program::Use() { gl.glUseProgram(m_prog); }

// ---------------------------------------------------------------- init / frame
bool Init(void (*log)(const char*))
{
  s_log = log;
  bool ok = true;
#define GLN_LOAD(n) gl.n = (decltype(gl.n))SDL_GL_GetProcAddress(#n); if (!gl.n) { Log("GLNative: missing driver entry point %s", #n); ok = false; }
  GLN_FUNCS(GLN_LOAD)
#undef GLN_LOAD
  s_ready = ok;
  s_stages = ParseStages(getenv("FARCRY_NATIVE"));
  Log("GLNative: core %s, stages 0x%x (FARCRY_NATIVE=%s)", ok ? "ready" : "unavailable", s_stages, getenv("FARCRY_NATIVE") ? getenv("FARCRY_NATIVE") : "");
  return ok;
}

void Shutdown()
{
  if (!s_ready) return;
  for (auto& v : s_vaos) gl.glDeleteVertexArrays(1, &v.second);
  s_vaos.clear();
  if (s_vtx.buffer) gl.glDeleteBuffers(1, &s_vtx.buffer);
  if (s_idx.buffer) gl.glDeleteBuffers(1, &s_idx.buffer);
  s_vtx = Stream(); s_idx = Stream();
  s_ready = false;
}

bool Enabled(unsigned stageMask) { return s_ready && (s_stages & stageMask) != 0; }

void FrameBoundary()
{
  if (s_ready && (s_stages & GLN_STAGE_2D)) GLN2D_FrameStats();
  if (s_ready && (s_stages & GLN_STAGE_MESH)) GLNMesh_FrameStats();
  if (!s_ready || (!s_stages && !getenv("FARCRY_NATIVE_SELFTEST"))) return;
  GLNSection section;
  StreamReset(s_vtx, GL_ARRAY_BUFFER, (size_t)4 << 20);
  StreamReset(s_idx, GL_ELEMENT_ARRAY_BUFFER, (size_t)1 << 20);
}

// A coloured, textured-free test pattern in the top-left corner: proves
// programs, streams, VAOs and the section hand-over on each driver.
void SelfTest(int w, int h)
{
  static const bool s_on = getenv("FARCRY_NATIVE_SELFTEST") != nullptr;
  if (!s_on || !s_ready) return;
  static Program prog;
  static bool built = false, okBuilt = false;
  if (!built)
  {
    built = true;
    okBuilt = prog.Build("selftest",
      "#version 300 es\nin vec4 aPos; in vec4 aColor; uniform vec2 uScale; out vec4 vColor;\n"
      "void main() { gl_Position = vec4(aPos.xy * uScale - 1.0, 0.0, 1.0); gl_Position.y = -gl_Position.y; vColor = aColor; }\n",
      "#version 300 es\nprecision mediump float; in vec4 vColor; out vec4 oColor;\nvoid main() { oColor = vColor; }\n");
  }
  if (!okBuilt) return;
  GLNSection section;
  struct V { float x, y; uint8_t c[4]; };
  StreamAlloc va = AllocVertices(4 * sizeof(V));
  V* v = (V*)va.cpu;
  const float s = 48;
  v[0] = { 8, 8, { 255, 0, 0, 255 } }; v[1] = { 8 + s, 8, { 0, 255, 0, 255 } };
  v[2] = { 8, 8 + s, { 0, 0, 255, 255 } }; v[3] = { 8 + s, 8 + s, { 255, 255, 255, 255 } };
  StreamAlloc ia = AllocIndices(6 * sizeof(uint16_t));
  uint16_t* idx = (uint16_t*)ia.cpu; idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 2; idx[4] = 1; idx[5] = 3;
  CommitStreams();
  Layout l; l.stride = sizeof(V);
  l.Add(GLN_POS, 2, GL_FLOAT, false, 0);
  l.Add(GLN_COLOR, 4, GL_UNSIGNED_BYTE, true, 8);
  prog.Use();
  const float scale[2] = { 2.0f / (float)w, 2.0f / (float)h };
  gl.glUniform2fv(prog.Uniform("uScale"), 1, scale);
  BindVertexArray(l, va.buffer, va.offset, ia.buffer);
  const GLboolean depth = gl.glIsEnabled(GL_DEPTH_TEST), blend = gl.glIsEnabled(GL_BLEND), cull = gl.glIsEnabled(GL_CULL_FACE);
  gl.glDisable(GL_DEPTH_TEST); gl.glDisable(GL_BLEND); gl.glDisable(GL_CULL_FACE);
  gl.glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void*)ia.offset);
  if (depth) gl.glEnable(GL_DEPTH_TEST);
  if (blend) gl.glEnable(GL_BLEND);
  if (cull) gl.glEnable(GL_CULL_FACE);
}
} // namespace GLN

GLNSection::GLNSection() {}
GLNSection::~GLNSection()
{
  // VAO 0 now (engine buffer binds that follow must land in the layer's VAO);
  // the layer restores its program/texture/buffer bindings lazily at its next
  // draw (GLES3FFPMarkNativeDirty), so consecutive native draws pay nothing.
  GLN::gl.glBindVertexArray(0);
  GLES3FFPMarkNativeDirty();
}
void GLNForgetBuffer(unsigned buffer) { GLN::ForgetBuffer(buffer); }
