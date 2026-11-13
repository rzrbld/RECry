/*=============================================================================
  GLNative.h : native OpenGL ES 3.0 rendering core.

  The renderer is being moved off the fixed-function translation layer
  (GLES3FFP) subsystem by subsystem. Native code binds real GLSL ES programs,
  vertex array objects, buffers and textures itself, through driver entry
  points loaded here (never through the translation layer's overrides).

  Coexistence: native code runs inside a GLNSection. Its vertex state lives
  in its own VAOs; when the section ends, VAO 0 and the layer's program,
  texture and buffer bindings are restored (GLES3FFPRestoreState), so the
  layer and the engine's own state caches stay valid. Render state (blend,
  depth, cull, stencil) keeps going through the engine's EF_SetState.

  Stages are switched on with FARCRY_NATIVE=<stage>[,<stage>...] or "all"
  (names: see GLNStage). Default: none, i.e. today's renderer.
=============================================================================*/
#pragma once
// Internal to the native implementation files (GLN*.cpp): includes the real
// GLES 3 header, which engine renderer files cannot (their GL names are the
// engine's own function-pointer table). Engine code includes GLNative.h.
#include <GLES3/gl3.h>
#include "GLNative.h"
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <unordered_map>


// Fixed vertex attribute slots shared by every native program.
enum GLNAttr { GLN_POS = 0, GLN_COLOR = 1, GLN_NORMAL = 2, GLN_TEX0 = 3, GLN_TEX1 = 4, GLN_TEX2 = 5, GLN_TEX3 = 6, GLN_COLOR2 = 7, GLN_NUM_ATTRS = 8 };

namespace GLN
{

  // ---------------------------------------------------------------- programs
  class Program
  {
  public:
    // Compile and link (binary cache in <texture cache>/../native-programs).
    bool Build(const char* name, const std::string& vs, const std::string& fs);
    GLuint Id() const { return m_prog; }
    GLint Uniform(const char* name);        // cached location, -1 if absent
    void Use();
  private:
    GLuint m_prog = 0;
    std::unordered_map<std::string, GLint> m_uniforms;
  };

  // ---------------------------------------------------------------- vertex layout
  struct AttribDesc { uint8_t attr, size; uint16_t type; uint8_t normalized; uint8_t offset; };
  struct Layout
  {
    AttribDesc attrib[GLN_NUM_ATTRS];
    uint8_t count = 0;
    uint16_t stride = 0;
    void Add(GLNAttr a, int size, GLenum type, bool norm, int offset);
    uint64_t Key() const;
  };

  // ---------------------------------------------------------------- streaming geometry
  // Per-frame append-only vertex and index data (orphaned at frame start).
  struct StreamAlloc { void* cpu; GLuint buffer; size_t offset; };
  StreamAlloc AllocVertices(size_t bytes);
  StreamAlloc AllocIndices(size_t bytes);
  void CommitStreams();                     // upload what was written since the last commit

  // VAO for a layout over (vertex buffer, base offset) and an index buffer.
  GLuint BindVertexArray(const Layout& l, GLuint vbo, size_t vboOffset, GLuint ibo);
  void ForgetBuffer(GLuint b);   // a buffer was deleted: drop VAOs that reference it

  // ---------------------------------------------------------------- driver entry points
  struct Funcs
  {
#define GLN_FUNCS(X) \
    X(glUseProgram) X(glCreateShader) X(glShaderSource) X(glCompileShader) X(glGetShaderiv) X(glGetShaderInfoLog) \
    X(glCreateProgram) X(glAttachShader) X(glBindAttribLocation) X(glLinkProgram) X(glGetProgramiv) X(glGetProgramInfoLog) \
    X(glDeleteShader) X(glDeleteProgram) X(glGetUniformLocation) X(glUniform1i) X(glUniform1f) X(glUniform2fv) X(glUniform3fv) \
    X(glUniform4fv) X(glUniformMatrix3fv) X(glUniformMatrix4fv) X(glProgramBinary) X(glGetProgramBinary) X(glProgramParameteri) \
    X(glGenBuffers) X(glDeleteBuffers) X(glBindBuffer) X(glBufferData) X(glBufferSubData) \
    X(glFinish) X(glGenVertexArrays) X(glDeleteVertexArrays) X(glBindVertexArray) X(glVertexAttribPointer) X(glEnableVertexAttribArray) \
    X(glDisableVertexAttribArray) X(glVertexAttrib4f) X(glDrawArrays) X(glDrawElements) X(glDrawRangeElements) \
    X(glActiveTexture) X(glBindTexture) X(glGetString) X(glGetError) X(glViewport) X(glEnable) X(glDisable) X(glBlendFunc) X(glIsEnabled)
#define GLN_DECLARE(n) decltype(&::n) n = nullptr;
    GLN_FUNCS(GLN_DECLARE)
#undef GLN_DECLARE
  };
  extern Funcs gl;
}

