/*=============================================================================
  GLES3FFP.h : fixed-function state core for the GLES3 renderer.

  The engine's GL renderer was written against OpenGL 1.x-2.x and drives a
  large part of its drawing through fixed-function state (matrix stacks,
  immediate mode, client arrays, texture environment, fog, alpha test,
  texgen). OpenGL ES 3 has none of that. This core keeps that state on the
  CPU and turns each draw into a generated GLSL ES 3.00 program, so those
  code paths render while they are progressively replaced by native GLES3
  code. Draws issued while the engine has its own shader program bound pass
  straight through.

  Self-contained: uses only <GLES3/gl3.h> and SDL; the GL function table in
  GLSystem.cpp asks GLES3FFPOverride() for an implementation first.
=============================================================================*/
#pragma once

// Implementation for a GL table entry, or NULL when the core does not
// handle that entry point.
void* GLES3FFPOverride(const char* name);

// Load the driver entry points the core needs. Call once with the ES context
// current, before the GL function table is filled.
bool GLES3FFPInit(void (*log)(const char*));

// Upload finished background texture conversions. Call at the start of each
// frame on the render thread (budgeted; blocking shortly after a level load).
void GLES3FFPPumpTextures();

// End of level loading: wait for every queued conversion and upload it, so
// gameplay never shows placeholder textures.
void GLES3FFPFinishTextures();

// Draw calls and vertices submitted since the last call (then reset).
void GLES3FFPTakeDrawStats(unsigned& draws, unsigned& verts);
// Time spent in fixed-function per-draw setup and in driver draw calls (ms, then reset).
void GLES3FFPTakeTimes(double& ffpMs, double& glMs);
void GLES3FFPTakeUploadStats(double& ms, double& kb);
unsigned GLES3FFPTakeProgramsBuilt();
// Draws whose vertices come from client memory, their vertices, and immediate-mode batches.
void GLES3FFPTakeArrayStats(unsigned& clientDraws, unsigned& clientVerts, unsigned& immDraws);

// Diagnostics: renderer callback run after a draw-ID audit frame.
void GLES3FFPSetAuditHook(void (*hook)());

// Diagnostics: call once per presented frame.
void GLES3FFPFrameBoundary();

// Tag the next glColorPointer: 1 = terrain vertex colours (detail-layer masks in
// RGB, brightness in alpha). The fixed-function tier then uses the brightness,
// as the engine's terrain vertex program did; 0 = ordinary colour.
void GLES3FFPMarkNextColorArray(int semantic);

// Called when the renderer frees GL resources (context teardown).
void GLES3FFPShutdown();

void GLES3FFPForceEnvAlpha0(float a); // <0 disables
