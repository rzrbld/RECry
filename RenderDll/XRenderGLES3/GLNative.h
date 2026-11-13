/*=============================================================================
  GLNative.h : native OpenGL ES 3.0 rendering core, public part.

  The renderer is being moved off the fixed-function translation layer
  (GLES3FFP) subsystem by subsystem. Native code lives in GLN*.cpp files
  (GLNativeCore.h: programs, vertex layouts, streaming buffers, driver entry
  points) and binds real GLSL ES programs, VAOs, buffers and textures itself.
  This header has no GL includes: engine renderer files use their own GL
  function table names and include only this.

  Coexistence: native code runs inside a GLNSection; on exit VAO 0 and the
  translation layer's program/texture/buffer bindings are restored
  (GLES3FFPRestoreState), so the layer's and the engine's state caches stay
  valid. Render state (blend, depth, cull, stencil) keeps going through the
  engine's EF_SetState.

  Stages: FARCRY_NATIVE=<stage>[,<stage>...] or "all" (2d, mesh, pass, rt).
  Default none = today's renderer.
=============================================================================*/
#pragma once

enum GLNStage
{
  GLN_STAGE_2D   = 1 << 0,   // 2D, UI, fonts, video
  GLN_STAGE_MESH = 1 << 1,   // general shader passes: terrain, objects, vegetation, characters
  GLN_STAGE_PASS = 1 << 2,   // detail/fog/light overlay passes, flares, sprites
  GLN_STAGE_RT   = 1 << 3,   // render targets (FBOs)
};

namespace GLN
{
  // Load driver entry points (call once, ES context current). Returns false
  // if any is missing; native stages then stay off.
  bool Init(void (*log)(const char*));
  void Shutdown();
  bool Enabled(unsigned stageMask);         // stage requested and core initialised
  void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  void FrameBoundary();                     // once per presented frame (stream buffer reset)
  void SelfTest(int w, int h);              // FARCRY_NATIVE_SELFTEST: draw a test pattern natively
}

// Native code runs inside a section; on exit the translation layer's view of
// GL state is restored.
struct GLNSection
{
  GLNSection();
  ~GLNSection();
};

// Implemented in GLES3FFP.cpp: rebind VAO 0 and the layer's program, texture
// and array-buffer bindings after native code changed them.
void GLES3FFPRestoreState();
// Mark the layer's real bindings stale; it calls GLES3FFPRestoreState before
// its next draw.
void GLES3FFPMarkNativeDirty();
// The layer deleted a GL buffer: drop native VAOs that reference it.
void GLNForgetBuffer(unsigned buffer);
