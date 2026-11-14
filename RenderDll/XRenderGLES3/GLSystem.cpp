/*=============================================================================
  GLSystem.cpp : HW depended OpenGL functions and extensions handling.
  Copyright (c) 2001 Crytek Studios. All Rights Reserved.

  Revision history:
    * Created by Honitch Andrey

=============================================================================*/

#undef THIS_FILE
static char THIS_FILE[] = __FILE__;


#include "RenderPCH.h"
#include "GL_Renderer.h"
#include "GLAudit.h"
#include "GLR.h"
#include "GLTexUpload.h"
#include "CG/cgGL.h"
#include "GLCGVProgram.h"
#include "GLCGPShader.h"


#ifdef USE_3DC
#include "../Common/3Dc/CompressorLib.h"
void (*DeleteDataATI)(void *pData);
COMPRESSOR_ERROR (*CompressTextureATI)(DWORD width,
                                                 DWORD height,
                                                 UNCOMPRESSED_FORMAT sourceFormat,
                                                 COMPRESSED_FORMAT destinationFormat,
                                                 void    *inputData,
                                                 void    **dataOut,
                                                 DWORD   *outDataSize);
#endif

#ifdef GLES3_RENDERER
///////////////////////////////////////////////////////////////////
// GLES3 platform layer (replaces Win32 window/WGL context handling)
///////////////////////////////////////////////////////////////////
#include <SDL.h>
#include <vector>
#include <algorithm>
#include "GLES3CoreFuncs.h"
#include "GLES3FFP.h"
#include "../../port/platform/PortTiming.h"

static void GLES3LogBridge(const char* s) { if (iLog) iLog->Log("%s", s); }

// Audit aid: GL texture name -> engine texture name (printed after a draw-ID audit frame).
static void GLES3AuditTextureNames()
{
  if (!gRenDev || !gRenDev->m_TexMan) return;
  for (int i = 0; i < gRenDev->m_TexMan->m_Textures.Num(); ++i)
  {
    STexPic* tp = gRenDev->m_TexMan->m_Textures[i];
    if (tp) fprintf(stderr, "GLES3TEX bind=%d name=%s %dx%d flags=0x%x\n", tp->m_Bind, tp->m_SearchName.c_str(), tp->m_Width, tp->m_Height, tp->m_Flags);
  }
}

// Diagnostics: engine texture name of a GL texture (only used on errors).
static const char* GLES3TexNameOf(unsigned tex)
{
  if (!gRenDev || !gRenDev->m_TexMan) return "?";
  for (int i = 0; i < gRenDev->m_TexMan->m_Textures.Num(); ++i)
  {
    STexPic* tp = gRenDev->m_TexMan->m_Textures[i];
    if (tp && (unsigned)tp->m_Bind == tex) return tp->m_SearchName.c_str();
  }
  return "?";
}

static SDL_Window*   g_GLES3Window  = NULL;
static SDL_GLContext g_GLES3Context = NULL;

// Optional frame captures for validation: FARCRY_CAPTURE_TIMES=10,20 (seconds
// after the first presented frame), written as PPM into FARCRY_CAPTURE_DIR.
// On-demand capture: SIGUSR2 saves the next 3 frames to
// <FARCRY_CAPTURE_DIR or logs>/snap-<n>-<k>.ppm (live debugging on device).
#include <signal.h>
static volatile sig_atomic_t s_snapRequest = 0;
static void OnSnapSignal(int) { s_snapRequest = 3; }
// Size of what the game rendered: the offscreen scene when one is used, else the window.
static int s_sceneReadW = 0, s_sceneReadH = 0;
static void GLES3_ReadSize(int& w, int& h)
{
  if (s_sceneReadW > 0) { w = s_sceneReadW; h = s_sceneReadH; return; }
  SDL_GL_GetDrawableSize(g_GLES3Window, &w, &h);
}

// Captures are port tooling: they read through the driver directly, never
// through the engine's GL table (trapped by the native backend).
static void GLES3_RealReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* p)
{
  typedef void (APIENTRY *PRead)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
  static PRead rd = (PRead)SDL_GL_GetProcAddress("glReadPixels");
  rd(x, y, w, h, f, t, p);
}
static void GLES3_SnapIfRequested()
{
  static bool s_init = false; static int s_series = 0;
  if (!s_init) { s_init = true; signal(SIGUSR2, OnSnapSignal); }
  if (!s_snapRequest) return;
  const int k = 3 - (int)s_snapRequest; if (k == 0) ++s_series;
  if (k == 2) { extern void GLES3FFPRequestDump(); GLES3FFPRequestDump(); } // after the 3 clean frames
  if (k == 0 && GLR::Active()) GLR::TraceNextDraws(600);   // native: log the next frame's draws
  if (k == 0 && gRenDev)
  {
    // Where the snapshot was taken (FARCRY_BENCH_POS reproduces it: x y z yaw).
    const CCamera& cam = gRenDev->GetCamera();
    const Vec3 p = cam.GetPos(), a = cam.GetAngles();
    fprintf(stderr, "PORT snap: camera %.2f %.2f %.2f angles %.1f %.1f %.1f (bench: \"%.2f %.2f %.2f %.1f\")\n",
            p.x, p.y, p.z, a.x, a.y, a.z, p.x, p.y, p.z - 1.7f, a.z);
  }
  --s_snapRequest;
  const char* dir = getenv("FARCRY_SNAP_DIR"); if (!dir) dir = getenv("FARCRY_CAPTURE_DIR"); if (!dir) dir = ".";
  int w = 0, h = 0; GLES3_ReadSize(w, h);
  std::vector<unsigned char> px((size_t)w * h * 4);
  GLES3_RealReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  char path[1024]; snprintf(path, sizeof(path), "%s/snap-%02d-%d.ppm", dir, s_series, k);
  if (FILE* f = fopen(path, "wb"))
  {
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y) for (int x = 0; x < w; ++x) fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
    fclose(f);
  }
}

static void GLES3_CaptureIfDue(int demoFrame, int demoLoop)
{
  GLES3_SnapIfRequested();
  static std::vector<float> s_times; static size_t s_next = 0; static Uint32 s_start = 0; static bool s_init = false;
  if (!s_init)
  {
    s_init = true;
    if (const char* t = getenv("FARCRY_CAPTURE_TIMES"))
      for (const char* p = t; *p; ) { s_times.push_back((float)atof(p)); while (*p && *p != ',') ++p; if (*p) ++p; }
    s_start = SDL_GetTicks();
  }
  // FARCRY_CAPTURE_DEMO_FRAMES=300,1200: capture when the time demo's first
  // loop renders those frames (demo-<frame>.ppm): frame-exact image compares
  // between renderers, independent of frame rate.
  {
    static std::vector<int> s_demoFrames; static size_t s_demoNext = 0; static bool s_demoInit = false;
    if (!s_demoInit)
    {
      s_demoInit = true;
      if (const char* t = getenv("FARCRY_CAPTURE_DEMO_FRAMES"))
        for (const char* p = t; *p; ) { s_demoFrames.push_back(atoi(p)); while (*p && *p != ',') ++p; if (*p) ++p; }
    }
    if (s_demoNext < s_demoFrames.size() && demoLoop == 0 && demoFrame >= s_demoFrames[s_demoNext])
    {
      const char* dir = getenv("FARCRY_CAPTURE_DIR");
      int w = 0, h = 0; GLES3_ReadSize(w, h);
      std::vector<unsigned char> px((size_t)w * h * 4);
      GLES3_RealReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      char path[1024]; snprintf(path, sizeof(path), "%s/demo-%05d.ppm", dir ? dir : ".", s_demoFrames[s_demoNext]);
      if (FILE* f = fopen(path, "wb"))
      {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; --y)
          for (int x = 0; x < w; ++x) fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
        fclose(f);
        fprintf(stderr, "PORT capture: %s (demo frame %d)\n", path, demoFrame);
      }
      ++s_demoNext;
    }
  }
  // FARCRY_CAPTURE_FRAMES=N: from the first capture time on, capture N consecutive frames.
  static const int s_burst = getenv("FARCRY_CAPTURE_FRAMES") ? atoi(getenv("FARCRY_CAPTURE_FRAMES")) : 0;
  const bool bInBurst = s_burst > 0 && s_next > 0 && s_next < (size_t)s_burst;
  if (!bInBurst)
  {
    if (s_next >= s_times.size()) return;
    if (PortBenchRelative())
    {
      if (g_PortGameplayStartMs == 0 || (PortNowMs() - g_PortGameplayStartMs) / 1000.0 < s_times[s_next]) return;
    }
    else if ((SDL_GetTicks() - s_start) / 1000.0f < s_times[s_next]) return;
  }
  const char* dir = getenv("FARCRY_CAPTURE_DIR");
  int w = 0, h = 0; GLES3_ReadSize(w, h);
  std::vector<unsigned char> px((size_t)w * h * 4);
  GLES3_RealReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  char path[1024]; snprintf(path, sizeof(path), "%s/frame-%02d.ppm", dir ? dir : ".", (int)s_next);
  if (FILE* f = fopen(path, "wb"))
  {
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y)
      for (int x = 0; x < w; ++x) fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
    fclose(f);
    if (iLog) iLog->Log("GLES3: captured %s (%dx%d)", path, w, h);
  }
  ++s_next;
}

// Optional per-frame timing: FARCRY_GLES_FRAMELOG=<csv> (frame,ms,t_s), with
// FARCRY_GLES_FRAMELOG_WARMUP seconds skipped. Flushed every 120 frames.
static void GLES3_LogFrameTime()
{
  static FILE* s_f = NULL; static bool s_init = false; static Uint64 s_prev = 0, s_t0 = 0; static unsigned s_frame = 0;
  static double s_warmup = 0;
  if (!s_init)
  {
    s_init = true;
    if (const char* p = getenv("FARCRY_GLES_FRAMELOG")) { s_f = fopen(p, "w"); if (s_f) fprintf(s_f, "frame,ms,t_s,sys_ms,game_ms,render_ms,end_ms,swap_ms,draws,verts,ffp_ms,gl_ms,client_draws,client_verts,imm_draws,up_ms,up_kb,progs,cpu_ms\n"); }
    if (const char* w = getenv("FARCRY_GLES_FRAMELOG_WARMUP")) s_warmup = atof(w);
    s_t0 = s_prev = SDL_GetPerformanceCounter();
    return;
  }
  if (!s_f) return;
  const Uint64 now = SDL_GetPerformanceCounter();
  const double freq = (double)SDL_GetPerformanceFrequency();
  const double ms = (now - s_prev) * 1000.0 / freq, t = (now - s_t0) / freq;
  s_prev = now;
  unsigned draws = 0, verts = 0;
  GLES3FFPTakeDrawStats(draws, verts);
  double ffpMs = 0, glMs = 0;
  GLES3FFPTakeTimes(ffpMs, glMs);
  unsigned cDraws = 0, cVerts = 0, iDraws = 0;
  GLES3FFPTakeArrayStats(cDraws, cVerts, iDraws);
  double upMs = 0, upKb = 0;
  GLES3FFPTakeUploadStats(upMs, upKb);
  const unsigned progs = GLES3FFPTakeProgramsBuilt();
  static double s_prevCpu = 0; timespec cts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cts);
  const double cpuNow = cts.tv_sec * 1000.0 + cts.tv_nsec / 1e6, cpuMs = s_prevCpu > 0 ? cpuNow - s_prevCpu : 0; s_prevCpu = cpuNow;
  double ph[PORT_PHASE_COUNT];
  for (int i = 0; i < PORT_PHASE_COUNT; ++i) { ph[i] = g_PortPhaseMs[i]; g_PortPhaseMs[i] = 0; }
  if (PortBenchRelative() ? (g_PortGameplayStartMs == 0 || (PortNowMs() - g_PortGameplayStartMs) / 1000.0 < s_warmup) : t < s_warmup) return;
  fprintf(s_f, "%u,%.3f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%u,%.2f,%.2f,%u,%u,%u,%.2f,%.0f,%u,%.2f\n", ++s_frame, ms, t,
          ph[PORT_PHASE_SYS], ph[PORT_PHASE_GAME], ph[PORT_PHASE_RENDER], ph[PORT_PHASE_END], ph[PORT_PHASE_SWAP], draws, verts, ffpMs, glMs, cDraws, cVerts, iDraws, upMs, upKb, progs, cpuMs);
  if ((s_frame % 120) == 0) fflush(s_f);
}

// The engine needs destination alpha (terrain layer blending, sprite
// generation via readback), so the window surface is created with alpha.
// Scan-out must stay opaque whatever the display engine does with alpha:
// force alpha to 1 just before presenting.
static bool s_bGLES3SurfaceAlpha = false;
static void GLES3_OpaqueAlphaForPresent()
{
  if (!s_bGLES3SurfaceAlpha)
    return;
  typedef void (APIENTRY *PColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
  typedef void (APIENTRY *PClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
  typedef void (APIENTRY *PClear)(GLbitfield);
  typedef void (APIENTRY *PGetBooleanv)(GLenum, GLboolean*);
  typedef void (APIENTRY *PGetFloatv)(GLenum, GLfloat*);
  typedef GLboolean (APIENTRY *PIsEnabled)(GLenum);
  typedef void (APIENTRY *PCap)(GLenum);
  static PColorMask colorMask = (PColorMask)SDL_GL_GetProcAddress("glColorMask");
  static PClearColor clearColor = (PClearColor)SDL_GL_GetProcAddress("glClearColor");
  static PClear clear = (PClear)SDL_GL_GetProcAddress("glClear");
  static PGetBooleanv getBooleanv = (PGetBooleanv)SDL_GL_GetProcAddress("glGetBooleanv");
  static PGetFloatv getFloatv = (PGetFloatv)SDL_GL_GetProcAddress("glGetFloatv");
  static PIsEnabled isEnabled = (PIsEnabled)SDL_GL_GetProcAddress("glIsEnabled");
  static PCap enable = (PCap)SDL_GL_GetProcAddress("glEnable"), disable = (PCap)SDL_GL_GetProcAddress("glDisable");
  GLboolean mask[4]; GLfloat cc[4];
  getBooleanv(GL_COLOR_WRITEMASK, mask); getFloatv(GL_COLOR_CLEAR_VALUE, cc);
  const GLboolean scissor = isEnabled(GL_SCISSOR_TEST);
  if (scissor) disable(GL_SCISSOR_TEST);
  colorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
  clearColor(0, 0, 0, 1);
  clear(GL_COLOR_BUFFER_BIT);
  colorMask(mask[0], mask[1], mask[2], mask[3]);
  clearColor(cc[0], cc[1], cc[2], cc[3]);
  if (scissor) enable(GL_SCISSOR_TEST);
}

// Resolution independence: the game renders at r_Width x r_Height into an
// offscreen framebuffer, which is scaled to the real screen at present time
// (FARCRY_SCALE=fit keeps the aspect ratio with black bars, stretch fills the
// screen). When the render size equals the screen size the game draws to the
// window directly and nothing extra happens.
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
struct SGLES3Scene { GLuint fbo, color, depth; int w, h, dw, dh; bool stretch; };
static SGLES3Scene s_scene = { 0, 0, 0, 0, 0, 0, 0, false };
typedef void (APIENTRY *PFNGenObjs)(GLsizei, GLuint*);
typedef void (APIENTRY *PFNBindObj)(GLenum, GLuint);
static PFNBindObj s_bindFramebuffer = NULL;
static void GLES3_SceneSetup(int w, int h, int dw, int dh)
{
  const char* force = getenv("FARCRY_GLES3_FORCE_FBO");
  if ((w == dw && h == dh) && !force)
    return;
  typedef void (APIENTRY *PRbStorage)(GLenum, GLenum, GLsizei, GLsizei);
  typedef void (APIENTRY *PFbRb)(GLenum, GLenum, GLenum, GLuint);
  typedef GLenum (APIENTRY *PCheck)(GLenum);
  PFNGenObjs genFb = (PFNGenObjs)SDL_GL_GetProcAddress("glGenFramebuffers");
  PFNGenObjs genRb = (PFNGenObjs)SDL_GL_GetProcAddress("glGenRenderbuffers");
  PFNBindObj bindRb = (PFNBindObj)SDL_GL_GetProcAddress("glBindRenderbuffer");
  PRbStorage rbStorage = (PRbStorage)SDL_GL_GetProcAddress("glRenderbufferStorage");
  PFbRb fbRb = (PFbRb)SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
  PCheck check = (PCheck)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
  s_bindFramebuffer = (PFNBindObj)SDL_GL_GetProcAddress("glBindFramebuffer");
  if (!genFb || !genRb || !bindRb || !rbStorage || !fbRb || !check || !s_bindFramebuffer)
    return;
  genFb(1, &s_scene.fbo); genRb(1, &s_scene.color); genRb(1, &s_scene.depth);
  bindRb(GL_RENDERBUFFER, s_scene.color); rbStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
  bindRb(GL_RENDERBUFFER, s_scene.depth); rbStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
  bindRb(GL_RENDERBUFFER, 0);
  s_bindFramebuffer(GL_FRAMEBUFFER, s_scene.fbo);
  fbRb(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_scene.color);
  fbRb(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_scene.depth);
  if (check(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
  {
    iLog->Log("Warning: offscreen framebuffer %dx%d incomplete; drawing to the window", w, h);
    s_bindFramebuffer(GL_FRAMEBUFFER, 0); s_scene.fbo = 0;
    return;
  }
  // The context's default viewport and scissor box are the window's size.
  // Menus, videos and other 2D screens never set a viewport (they expect the
  // default to match the game's size), so with a smaller offscreen target
  // they were drawn window-sized and only their lower-left corner showed.
  {
    typedef void (APIENTRY *PRect)(GLint, GLint, GLsizei, GLsizei);
    if (PRect vp = (PRect)SDL_GL_GetProcAddress("glViewport")) vp(0, 0, w, h);
    if (PRect sc = (PRect)SDL_GL_GetProcAddress("glScissor")) sc(0, 0, w, h);
  }
  const char* sm = getenv("FARCRY_SCALE");
  s_scene.w = w; s_scene.h = h; s_scene.dw = dw; s_scene.dh = dh;
  s_sceneReadW = w; s_sceneReadH = h;
  s_scene.stretch = sm && !strcmp(sm, "stretch");
  iLog->Log("GLES3 render %dx%d scaled to screen %dx%d (%s)", w, h, dw, dh, s_scene.stretch ? "stretch" : "fit");
}
static void GLES3_ScenePresent()
{
  if (!s_scene.fbo)
    return;
  typedef void (APIENTRY *PBlit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
  typedef void (APIENTRY *PColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
  typedef void (APIENTRY *PClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
  typedef void (APIENTRY *PClear)(GLbitfield);
  typedef void (APIENTRY *PGetBooleanv)(GLenum, GLboolean*);
  typedef void (APIENTRY *PGetFloatv)(GLenum, GLfloat*);
  typedef GLboolean (APIENTRY *PIsEnabled)(GLenum);
  typedef void (APIENTRY *PCap)(GLenum);
  static PBlit blit = (PBlit)SDL_GL_GetProcAddress("glBlitFramebuffer");
  static PColorMask colorMask = (PColorMask)SDL_GL_GetProcAddress("glColorMask");
  static PClearColor clearColor = (PClearColor)SDL_GL_GetProcAddress("glClearColor");
  static PClear clear = (PClear)SDL_GL_GetProcAddress("glClear");
  static PGetBooleanv getBooleanv = (PGetBooleanv)SDL_GL_GetProcAddress("glGetBooleanv");
  static PGetFloatv getFloatv = (PGetFloatv)SDL_GL_GetProcAddress("glGetFloatv");
  static PIsEnabled isEnabled = (PIsEnabled)SDL_GL_GetProcAddress("glIsEnabled");
  static PCap enable = (PCap)SDL_GL_GetProcAddress("glEnable"), disable = (PCap)SDL_GL_GetProcAddress("glDisable");
  int x0 = 0, y0 = 0, x1 = s_scene.dw, y1 = s_scene.dh;
  if (!s_scene.stretch)
  {
    // Largest rectangle with the render aspect ratio, centred.
    if ((long)s_scene.dw * s_scene.h > (long)s_scene.dh * s_scene.w)
    { const int vw = (int)((long)s_scene.dh * s_scene.w / s_scene.h); x0 = (s_scene.dw - vw) / 2; x1 = x0 + vw; }
    else
    { const int vh = (int)((long)s_scene.dw * s_scene.h / s_scene.w); y0 = (s_scene.dh - vh) / 2; y1 = y0 + vh; }
  }
  GLboolean mask[4]; GLfloat cc[4];
  getBooleanv(GL_COLOR_WRITEMASK, mask); getFloatv(GL_COLOR_CLEAR_VALUE, cc);
  const GLboolean scissor = isEnabled(GL_SCISSOR_TEST);
  if (scissor) disable(GL_SCISSOR_TEST);
  colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  s_bindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  s_bindFramebuffer(GL_READ_FRAMEBUFFER, s_scene.fbo);
  if (x0 > 0 || y0 > 0) { clearColor(0, 0, 0, 1); clear(GL_COLOR_BUFFER_BIT); } // bars
  blit(0, 0, s_scene.w, s_scene.h, x0, y0, x1, y1, GL_COLOR_BUFFER_BIT,
       (x1 - x0 == s_scene.w && y1 - y0 == s_scene.h) ? GL_NEAREST : GL_LINEAR);
  colorMask(mask[0], mask[1], mask[2], mask[3]);
  clearColor(cc[0], cc[1], cc[2], cc[3]);
  if (scissor) enable(GL_SCISSOR_TEST);
  s_bindFramebuffer(GL_READ_FRAMEBUFFER, 0); // draw FB 0 stays bound for the opaque-alpha pass and the swap
}
static void GLES3_SceneRebind()
{
  if (s_scene.fbo) s_bindFramebuffer(GL_FRAMEBUFFER, s_scene.fbo);
}

// Everything after the frame's replay that touches GL: captures, the scaled
// scene blit and the swap. Runs on the render thread when it owns GL.
static void GLES3_Present(int demoFrame, int demoLoop)
{
  GLES3_CaptureIfDue(demoFrame, demoLoop);
  GLES3FFPFrameBoundary();
  GLES3_ScenePresent();
  GLES3_OpaqueAlphaForPresent();
  { // diagnostic: one capture of the final (scaled) screen
    static const char* s_shot = getenv("FARCRY_SCREEN_SHOT");
    static int s_shotFrame = getenv("FARCRY_SCREEN_SHOT_FRAME") ? atoi(getenv("FARCRY_SCREEN_SHOT_FRAME")) : 150;
    if (s_shot && --s_shotFrame == 0)
    {
      int w = 0, h = 0; SDL_GL_GetDrawableSize(g_GLES3Window, &w, &h);
      std::vector<unsigned char> px((size_t)w * h * 4);
      GLES3_RealReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      if (FILE* f = fopen(s_shot, "wb"))
      {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; --y) for (int x = 0; x < w; ++x) fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
        fclose(f);
      }
    }
  }
  // Diagnostic: FARCRY_GPU_TIMING=1 waits for the GPU before the swap and
  // logs the median wait every 100 frames (how long the GPU takes to finish
  // a frame after the CPU has submitted it; separates GPU cost from the
  // display's page-flip wait in swap_ms).
  {
    static const bool s_gpuTiming = getenv("FARCRY_GPU_TIMING") != NULL;
    if (s_gpuTiming)
    {
      static std::vector<double> s_waits;
      const Uint64 t0 = SDL_GetPerformanceCounter();
      glFinish();
      s_waits.push_back((SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
      if (s_waits.size() >= 100)
      {
        std::sort(s_waits.begin(), s_waits.end());
        fprintf(stderr, "PORT gpu_finish_ms median=%.2f p90=%.2f max=%.2f\n", s_waits[50], s_waits[90], s_waits[99]);
        s_waits.clear();
      }
    }
  }
  SDL_GL_SwapWindow(g_GLES3Window);
  GLES3_SceneRebind();
}

void GLES3_SwapBuffers()
{
  if (!g_GLES3Window) return;
  if (GLR::Active())
  {
    if (getenv("FARCRY_GLR_SELFTEST")) GLR::SelfTest();
    GLR::EndFrame();   // native backend: play back the recorded frame (or hand it to the render thread)
  }
  {
    // Threaded: the present is queued after the frame; this waits only for
    // the previous frame (the time shows as swap_ms).
    SPortPhaseScope portScope(PORT_PHASE_SWAP);
    // The demo frame of the frame being presented (the game moves on meanwhile).
    const int demoFrame = g_PortDemoFrame, demoLoop = g_PortDemoLoop;
    if (GLR::Active()) GLR::PostFrameEnd([demoFrame, demoLoop] { GLES3_Present(demoFrame, demoLoop); });
    else GLES3_Present(demoFrame, demoLoop);
  }
  // From the first presented frame on, a render thread owns GL (native renderer).
  if (GLR::Active() && !GLR::Threaded()) GLR::StartThread(g_GLES3Window, g_GLES3Context);
  if (GLR::Active()) GLR::BeginFrame();
  GLAudit_Frame();
  GLES3_LogFrameTime();
}

// WGL/GDI entries of the GL function table, implemented on SDL.
// The render thread owns the context once it runs: making it current here would take it away.
static BOOL  __stdcall GLES3_wglMakeCurrent(HDC, HGLRC) { if (GLR::Threaded()) return TRUE; return g_GLES3Context && SDL_GL_MakeCurrent(g_GLES3Window, g_GLES3Context) == 0; }
static BOOL  __stdcall GLES3_wglSwapBuffers(HDC) { GLES3_SwapBuffers(); return TRUE; }
static HGLRC __stdcall GLES3_wglCreateContext(HDC) { return (HGLRC)g_GLES3Context; }
static BOOL  __stdcall GLES3_wglDeleteContext(HGLRC) { return TRUE; }
static BOOL  __stdcall GLES3_wglShareLists(HGLRC, HGLRC) { return TRUE; }
static HGLRC __stdcall GLES3_wglGetCurrentContext(VOID) { return GLR::Threaded() ? (HGLRC)g_GLES3Context : (HGLRC)SDL_GL_GetCurrentContext(); }
static PROC  __stdcall GLES3_wglGetProcAddress(LPCSTR) { return NULL; } // no WGL extensions on GLES
static BOOL  __stdcall GLES3_wglSwapIntervalEXT(int i) { BOOL ok = FALSE; GLR::Sync([&] { ok = SDL_GL_SetSwapInterval(i) == 0; }); return ok; }

static void* GLES3PlatformProc(const char* name)
{
  if (!strcmp(name, "wglMakeCurrent")) return (void*)&GLES3_wglMakeCurrent;
  if (!strcmp(name, "wglSwapBuffers")) return (void*)&GLES3_wglSwapBuffers;
  if (!strcmp(name, "wglCreateContext")) return (void*)&GLES3_wglCreateContext;
  if (!strcmp(name, "wglDeleteContext")) return (void*)&GLES3_wglDeleteContext;
  if (!strcmp(name, "wglShareLists")) return (void*)&GLES3_wglShareLists;
  if (!strcmp(name, "wglGetCurrentContext")) return (void*)&GLES3_wglGetCurrentContext;
  if (!strcmp(name, "wglGetProcAddress")) return (void*)&GLES3_wglGetProcAddress;
  if (!strcmp(name, "wglSwapIntervalEXT")) return (void*)&GLES3_wglSwapIntervalEXT;
  return NULL;
}

static bool GLES3IsCoreFunc(const char* name)
{
  for (int i = 0; g_GLES3CoreFuncs[i]; ++i)
    if (!strcmp(g_GLES3CoreFuncs[i], name))
      return true;
  return false;
}

// Desktop extensions whose functionality is core in OpenGL ES 3.0. Their
// suffixed entry points (glActiveTextureARB, glBindBufferARB, ...) resolve to
// the core functions; entries with no ES3 equivalent go to counted stubs.
static bool GLES3IsCoreExtension(const char* ext)
{
  static const char* const s_core[] = {
    "GL_ARB_multitexture", "GL_ARB_texture_compression", "GL_ARB_vertex_buffer_object",
    "GL_ARB_texture_cube_map", "GL_EXT_texture_cube_map", "GL_EXT_draw_range_elements",
    "GL_EXT_texture3D", "GL_EXT_stencil_wrap", NULL };
  static const bool s_noVBO = getenv("FARCRY_GLES3_NO_VBO") != NULL; // diagnostic: draw from system memory
  if (s_noVBO && !strcmp(ext, "GL_ARB_vertex_buffer_object"))
    return false;
  for (int i = 0; s_core[i]; ++i)
    if (!strcmp(s_core[i], ext))
      return true;
  // Bring-up switch: report the ARB program extensions so the engine selects
  // its shader techniques and requests Cg programs (captured for the
  // Cg -> GLSL ES translator). Their entry points are counted stubs.
  // In native mode the programs are real (GLRCg translation, GLRCgPrograms).
  static int s_shaderTier = GLR::ProgramsSupported() ? 1 : (getenv("FARCRY_GLES3_SHADER_TIER") ? atoi(getenv("FARCRY_GLES3_SHADER_TIER")) : 0);
  if (s_shaderTier && (!strcmp(ext, "GL_ARB_vertex_program") || !strcmp(ext, "GL_ARB_fragment_program")))
    return true;
  return false;
}

// glFooARB / glFooEXT -> glFoo when glFoo is GLES3 core.
static const char* GLES3CoreAlias(const char* name)
{
  static char buf[128];
  size_t n = strlen(name);
  if (n < 5 || n >= sizeof(buf)) return NULL;
  if (strcmp(name + n - 3, "ARB") && strcmp(name + n - 3, "EXT")) return NULL;
  memcpy(buf, name, n - 3); buf[n - 3] = 0;
  return GLES3IsCoreFunc(buf) ? buf : NULL;
}

// Bring-up diagnostics: desktop-only GL entry points are bound to named stubs
// that count their calls, so a real game run reports which desktop paths are
// exercised and must be converted to GLES3 code. They draw nothing.
struct SGLES3Missing { const char* name; unsigned long calls; };
static std::vector<SGLES3Missing*>& GLES3MissingList() { static std::vector<SGLES3Missing*> v; return v; }
static void GLES3NoteMissing(SGLES3Missing& m)
{
  if (m.calls++ == 0)
  {
    GLES3MissingList().push_back(&m);
    if (iLog) iLog->Log("GLES3: desktop-only GL call reached: %s", m.name);
  }
}
#define GL_EXT(name)
#define GL_PROC(ext,ret,func,parms) \
  static SGLES3Missing g_GLES3Miss_##func = { #func, 0 }; \
  static ret __stdcall GLES3Stub_##func parms { GLES3NoteMissing(g_GLES3Miss_##func); return (ret)0; }
#include "GLFuncs.h"
#undef GL_EXT
#undef GL_PROC

static void* GLES3MissingStub(const char* name)
{
#define GL_EXT(n)
#define GL_PROC(ext,ret,func,parms) if (!strcmp(name, #func)) return (void*)&GLES3Stub_##func;
#include "GLFuncs.h"
#undef GL_EXT
#undef GL_PROC
  return NULL;
}

void GLES3_ReportMissingCalls()
{
  std::vector<SGLES3Missing*> v = GLES3MissingList();
  if (v.empty() || !iLog) return;
  std::sort(v.begin(), v.end(), [](const SGLES3Missing* a, const SGLES3Missing* b) { return a->calls > b->calls; });
  iLog->Log("GLES3: %d distinct desktop-only GL entry points were called:", (int)v.size());
  for (size_t i = 0; i < v.size(); ++i)
    iLog->Log("GLES3:   %10lu  %s", v[i]->calls, v[i]->name);
  if (const char* path = getenv("FARCRY_GLES3_MISSING_LOG"))
    if (FILE* f = fopen(path, "w"))
    {
      for (size_t i = 0; i < v.size(); ++i) fprintf(f, "%lu\t%s\n", v[i]->calls, v[i]->name);
      fclose(f);
    }
}
#endif // GLES3_RENDERER


///////////////////////////////////////////////////////////////////
// Gamma Functions
///////////////////////////////////////////////////////////////////

void CGLRenderer::CheckGammaSupport(void)
{
  m_Features &= ~RFT_HWGAMMA;

  if (CV_r_nohwgamma)
    return;

  m_Features |= RFT_HWGAMMA;
}

void CGLRenderer::SetDeviceGamma(ushort *r, ushort *g, ushort *b)
{
  ushort gamma[3][256];
  int i;

  if (!(GetFeatures() & RFT_HWGAMMA))
    return;

  if (CV_r_nohwgamma)
    return;

  if (!m_CurrContext->m_hDC)
    return;

  for (i=0; i<256; i++)
  {
    gamma[0][i] = r[i];
    gamma[1][i] = g[i];
    gamma[2][i] = b[i];
  }
  if (SUPPORTS_WGL_3DFX_gamma_control)
    wglSetDeviceGammaRamp3DFX(m_CurrContext->m_hDC, gamma);
  else
    SetDeviceGammaRamp(m_CurrContext->m_hDC, gamma);
}

void CGLRenderer::RestoreDeviceGamma(void)
{
  if (!(GetFeatures() & RFT_HWGAMMA))
    return;

  if (CV_r_nohwgamma)
    return;

  HDC dc;

  struct
  {
    ushort red[256];
    ushort green[256];
    ushort blue[256];
  } Ramp;

  for(int i=0; i<256; i++)
  {
    Ramp.red[i] = Ramp.green[i] = Ramp.blue[i] = i << 8;
  }

  dc = GetDC(GetDesktopWindow());
  if (SUPPORTS_WGL_3DFX_gamma_control)
    wglSetDeviceGammaRamp3DFX(dc, &Ramp);
  else
    SetDeviceGammaRamp(dc, &Ramp);
  ReleaseDC(GetDesktopWindow(), dc);
}


void CGLRenderer::SetGamma(float fGamma, float fBrightness, float fContrast)
{
  int i;

  fGamma = CLAMP(fGamma, 0.5f, 3.0f);
  if (m_fLastGamma == fGamma && m_fLastBrightness == fBrightness && m_fLastContrast == fContrast)
    return;

  //int n;
  for ( i=0; i<256; i++ )
  {
    m_GammaTable[i] = CLAMP((int)((fContrast+0.5f)*cry_powf((float)i/255.f,1.0f/fGamma)*65535.f + (fBrightness-0.5f)*32768.f - fContrast*32768.f + 16384.f), 0, 65535);
    /*if ( fGamma == 1.0f )
      n = i;
    else
    {
      float f = (float)i;
      n = (int)(pow(f/255.0, 1.0/fGamma) * 255.0 + 0.5);
    }
    if ( n < 0 )
      n = 0;
    if ( n > 255 )
      n = 255;
    m_GammaTable[i] = n | (n<<8); */
  }

  m_fLastGamma = fGamma;
  m_fLastBrightness = fBrightness;
  m_fLastContrast = fContrast;

  SetDeviceGamma(m_GammaTable, m_GammaTable, m_GammaTable);
}

bool CGLRenderer::SetGammaDelta(const float fGamma)
{
  m_fDeltaGamma = fGamma;
  SetGamma(CV_r_gamma + fGamma, CV_r_brightness, CV_r_contrast);
  return true;
}

//================================================================================
// Library funcs

void CGLRenderer::FreeLibrary()
{
#ifndef GLES3_RENDERER
  if (m_hLibHandle)
  {
    ::FreeLibrary((HMODULE)(intptr_t)(int)m_hLibHandle);
    m_hLibHandle = NULL;
  }
  if (m_hLibHandleGDI)
  {
    ::FreeLibrary((HMODULE)(intptr_t)(int)m_hLibHandleGDI);
    m_hLibHandleGDI = NULL;
  }
#endif

  // Free OpenGL function.
  #define GL_EXT(name) SUPPORTS##name=0;
  #define GL_PROC(ext,ret,func,parms) func = NULL;
  #include "GLFuncs.h"
  #undef GL_EXT
  #undef GL_PROC
}

bool CGLRenderer::LoadLibrary()
{
#ifdef GLES3_RENDERER
  // The GLES driver is loaded by SDL for the platform's video backend.
  strcpy(m_LibName, "SDL2 OpenGL ES 3");
  m_hLibHandle = NULL;
  m_hLibHandleGDI = NULL;
  return g_GLES3Context != NULL;
#endif
  strcpy(m_LibName, "OpenGL32.dll");

  m_hLibHandle = ::LoadLibrary(m_LibName);
  if (!m_hLibHandle)
  {
    iLog->Log("Warning: Couldn't load library <%s>\n", m_LibName);
    return false;
  }
  m_hLibHandleGDI = ::LoadLibrary("GDI32.dll");
  assert(m_hLibHandleGDI);

  return true;
}

typedef const char * (WINAPI * PFNWGLGETEXTENSIONSSTRINGARBPROC) (HDC hdc);
typedef const char * (WINAPI * PFNWGLGETEXTENSIONSSTRINGEXTPROC) ();
static PFNWGLGETEXTENSIONSSTRINGARBPROC wglGetExtensionsStringARB;
static PFNWGLGETEXTENSIONSSTRINGEXTPROC wglGetExtensionsStringEXT;

#define GET_GL_PROC(functype,funcname) \
  static functype funcname = (functype)pwglGetProcAddress(#funcname);\
  assert(funcname);\

bool CGLRenderer::FindExt( const char* Name )
{
#ifdef GLES3_RENDERER
  if (GLES3IsCoreExtension(Name))
    return true;
#endif
  char *str = (char*)glGetString(GL_EXTENSIONS);
  if (str && strstr(str, Name))
    return true;
#ifdef GLES3_RENDERER
  return false;
#endif

  GET_GL_PROC(PFNWGLGETEXTENSIONSSTRINGARBPROC,wglGetExtensionsStringARB);
  if(wglGetExtensionsStringARB)
  {
    const char * wglExt = wglGetExtensionsStringARB(m_RContexts[0]->m_hDC);
    if (wglExt)
      return (strstr(wglExt, Name) != NULL);
  }

  return false;
}

void CGLRenderer::FindProc( void*& ProcAddress, char* Name, char* SupportName, byte& Supports, bool AllowExt )
{
#ifdef GLES3_RENDERER
  {
    const char* tableName = Name;           // stub key keeps the 'p' prefix
    const char* fn = (Name[0] == 'p') ? Name + 1 : Name;
    const bool coreGroup = !strcmp(SupportName, "_GL");
    // Fixed-function core first: it wraps both desktop-only entry points and
    // core ones whose desktop semantics differ (glEnable, draws, uploads).
    if (!ProcAddress) ProcAddress = GLES3FFPOverride(fn);
    if (!ProcAddress) ProcAddress = GLES3PlatformProc(fn);
    // Core GLES3 entry points, or extension entries whose extension the
    // driver advertises. Never trust a bare lookup: EGL may hand back
    // dispatch stubs for any name.
    const bool coreExt = !coreGroup && GLES3IsCoreExtension(SupportName + 1);
    if (!ProcAddress && (GLES3IsCoreFunc(fn) || (!coreGroup && !coreExt && Supports)))
      ProcAddress = SDL_GL_GetProcAddress(fn);
    if (!ProcAddress && (coreGroup || coreExt))
      if (const char* alias = GLES3CoreAlias(fn))
        ProcAddress = SDL_GL_GetProcAddress(alias);
    if (!ProcAddress)
    {
      if (coreGroup || coreExt)
        ProcAddress = GLES3MissingStub(tableName); // no ES3 equivalent: counted stub
      else
      {
        Supports = 0;                              // missing extension: path disabled
        // Some call sites skip the SUPPORTS check; give them a counted stub
        // instead of a null pointer. A few entries are used as availability
        // tests (if (glFooNV) ...) and must stay null.
        static const char* const s_nullChecked[] = { "glVertexAttribPointerNV", "glSetFenceNV", "glVertexArrayRangeNV", NULL };
        bool keepNull = false;
        for (int k = 0; s_nullChecked[k]; ++k) if (!strcmp(s_nullChecked[k], fn)) keepNull = true;
        if (!keepNull)
          ProcAddress = GLES3MissingStub(tableName);
      }
    }
    return;
  }
#endif
  if (Name[0] == 'p')
    Name = &Name[1];
  if( !ProcAddress )
    ProcAddress = GetProcAddress( (HMODULE)(intptr_t)(int)m_hLibHandle, Name );
  if( !ProcAddress )
    ProcAddress = GetProcAddress( (HMODULE)(intptr_t)(int)m_hLibHandleGDI, Name );
  if( !ProcAddress && Supports && AllowExt )
    ProcAddress = pwglGetProcAddress( Name );
  if( !ProcAddress )
  {
    if( Supports )
      iLog->Log("Warning:   Missing function '%s' for '%s' support\n", Name, SupportName );
    Supports = 0;
  }
}

void CGLRenderer::FindProcs( bool AllowExt )
{
  #define GL_EXT(name) if( AllowExt ) SUPPORTS##name = FindExt( TEXT(#name)+1 );
  #define GL_PROC(ext,ret,func,parms) FindProc( *(void**)&func, #func, #ext, SUPPORTS##ext, AllowExt );
  #include "GLFuncs.h"
  #undef GL_EXT
  #undef GL_PROC
#ifdef GLES3_RENDERER
  // Stage 0 of the native backend: FARCRY_GL_AUDIT counts every engine GL
  // call per function and call site (GLAudit.h). Wrapped once.
  static bool s_audited = false;
  if (!s_audited && GLAudit_Enabled())
  {
    s_audited = true;
    #define GL_EXT(name)
    #define GL_PROC(ext,ret,func,parms) GLAUDIT_WRAP(func)
    #include "GLFuncs.h"
    #undef GL_EXT
    #undef GL_PROC
  }
#endif
}

bool CGLRenderer::CheckOGLExtensions(void)
{
  CTexMan::m_bRGBA = true;

  iLog->Log("\n...Check OpenGL extensions\n");

  FindProcs( true );

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_ARB_texture_compression)
    iLog->Log("  ...GL_ARB_texture_compression not found\n");
  else
  if (CV_gl_arb_texture_compression && CV_r_supportcompressedtextures)
  {
    m_Features |= RFT_COMPRESSTEXTURE;
    iLog->Log("  ...using GL_ARB_texture_compression\n");
  }
  else
  {
    SUPPORTS_GL_ARB_texture_compression = 0;
    iLog->Log("  ...ignoring GL_ARB_texture_compression\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_WGL_EXT_swap_control)
    iLog->Log("  ...WGL_EXT_swap_control not found\n");
  else
  if (CV_gl_ext_swapcontrol)
    iLog->Log("  ...using WGL_EXT_swap_control\n");
  else
  {
    SUPPORTS_WGL_EXT_swap_control = 0;
    wglSwapIntervalEXT = NULL;
    iLog->Log("  ...ignoring WGL_EXT_swap_control\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_WGL_ARB_render_texture)
    iLog->Log("  ...WGL_ARB_render_texture not found\n");
  else
  if (CV_gl_arb_render_texture)
    iLog->Log("  ...using WGL_ARB_render_texture\n");
  else
  {
    SUPPORTS_WGL_ARB_render_texture = 0;
    iLog->Log("  ...ignoring WGL_ARB_render_texture\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_WGL_3DFX_gamma_control)
    iLog->Log("  ...WGL_3DFX_gamma_control not found\n");
  else
  if (CV_gl_3dfx_gamma_control)
    iLog->Log("  ...using WGL_3DFX_gamma_control\n");
  else
  {
    SUPPORTS_WGL_3DFX_gamma_control = 0;
    wglGetDeviceGammaRamp3DFX = NULL;
    wglSetDeviceGammaRamp3DFX = NULL;
    iLog->Log("  ...ignoring WGL_3DFX_gamma_control\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_ARB_multisample)
    iLog->Log("  ...GL_ARB_multisample not found\n");
  else
  if (CV_gl_arb_multisample)
    iLog->Log("  ...using GL_ARB_multisample\n");
  else
  {
    SUPPORTS_GL_ARB_multisample = 0;
    iLog->Log("  ...ignoring GL_ARB_multisample\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_WGL_ARB_pbuffer)
    iLog->Log("  ...WGL_ARB_pbuffer not found\n");
  else
  if (CV_gl_arb_pbuffer)
    iLog->Log("  ...using WGL_ARB_pbuffer\n");
  else
  {
    SUPPORTS_WGL_ARB_pbuffer = 0;
    iLog->Log("  ...ignoring WGL_ARB_pbuffer\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_WGL_ARB_pixel_format)
    iLog->Log("  ...WGL_ARB_pixel_format not found\n");
  else
  if (CV_gl_arb_pixel_format)
    iLog->Log("  ...using WGL_ARB_pixel_format\n");
  else
  {
    SUPPORTS_WGL_ARB_pixel_format = 0;
    iLog->Log("  ...ignoring WGL_ARB_pixel_format\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_WGL_ARB_buffer_region)
    iLog->Log("  ...WGL_ARB_buffer_region not found\n");
  else
  if (CV_gl_arb_buffer_region)
    iLog->Log("  ...using WGL_ARB_buffer_region\n");
  else
  {
    SUPPORTS_WGL_ARB_buffer_region = 0;
    iLog->Log("  ...ignoring WGL_ARB_buffer_region\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_ARB_texture_env_combine)
    iLog->Log("  ...GL_ARB_texture_env_combine not found\n");
  else
  if (CV_gl_arb_texture_env_combine)
    iLog->Log("  ...using GL_ARB_texture_env_combine\n");
  else
  {
    SUPPORTS_GL_ARB_texture_env_combine = 0;
    iLog->Log("  ...ignoring GL_ARB_texture_env_combine\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_SGIX_depth_texture)
  {
    iLog->Log("  ...GL_SGIX_depth_texture not found\n");
  }
  else
  if (CV_gl_sgix_depth_texture)
  {
#ifdef WIN64
    //m_Features |= RFT_DEPTHMAPS;
#else
    m_Features |= RFT_DEPTHMAPS;
#endif
    iLog->Log("  ...using GL_SGIX_depth_texture\n");
  }
  else
  {
    SUPPORTS_GL_SGIX_depth_texture = 0;
    iLog->Log("  ...ignoring GL_SGIX_depth_texture\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_SGIX_shadow)
    iLog->Log("  ...GL_SGIX_shadow not found\n");
  else
  if (CV_gl_sgix_shadow)
    iLog->Log("  ...using GL_SGIX_shadow\n");
  else
  {
    SUPPORTS_GL_SGIX_shadow = 0;
    iLog->Log("  ...ignoring GL_SGIX_shadow\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_SGIS_generate_mipmap)
    iLog->Log("  ...GL_SGIS_generate_mipmap not found\n");
  else
  if (CV_gl_sgis_generate_mipmap)
    iLog->Log("  ...using GL_SGIS_generate_mipmap\n");
  else
  {
    SUPPORTS_GL_SGIS_generate_mipmap = 0;
    iLog->Log("  ...ignoring GL_SGIS_generate_mipmap\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_SGIS_texture_lod)
    iLog->Log("  ...GL_SGIS_texture_lod not found\n");
  else
  if (CV_gl_sgis_texture_lod)
    iLog->Log("  ...using GL_SGIS_texture_lod\n");
  else
  {
    SUPPORTS_GL_SGIS_texture_lod = 0;
    iLog->Log("  ...ignoring GL_SGIS_texture_lod\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_ARB_multitexture)
    iLog->Log("  ...GL_ARB_multitexture not found\n");
  else
  if (CV_gl_arb_multitexture)
  {
    iLog->Log("  ...using GL_ARB_multitexture\n");
    m_Features |= RFT_MULTITEXTURE;
  }
  else
  {
    SUPPORTS_GL_ARB_multitexture = 0;
    iLog->Log("  ...ignoring GL_ARB_multitexture\n");
  }

  if (!(GetFeatures() & RFT_MULTITEXTURE))
  {
    glActiveTextureARB = NULL;
    glClientActiveTextureARB = NULL;
    glMultiTexCoord2fARB = NULL;
    glMultiTexCoord2fvARB = NULL;

    m_MaxActiveTexturesARBFixed = 0;
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_texgen_reflection)
    iLog->Log("  ...GL_NV_texgen_reflection not found\n");
  else
  if (CV_gl_nv_texgen_reflection)
  {
    iLog->Log("  ...using GL_NV_texgen_reflection\n");
    m_Features |= RFT_TEXGEN_REFLECTION;
  }
  else
  {
    SUPPORTS_GL_NV_texgen_reflection = 0;
    iLog->Log("  ...ignoring GL_NV_texgen_reflection\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_HP_occlusion_test)
    iLog->Log("  ...GL_HP_occlusion_test not found\n");
  else
  if (CV_gl_hp_occlusion_test)
  {
    iLog->Log("  ...using GL_HP_occlusion_test\n");
    m_Features |= RFT_OCCLUSIONTEST;
  }
  else
  {
    SUPPORTS_GL_HP_occlusion_test = 0;
    iLog->Log("  ...ignoring GL_HP_occlusion_test\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_occlusion_query)
    iLog->Log("  ...GL_NV_occlusion_query not found\n");
  else
  if (CV_gl_hp_occlusion_test)
  {
    iLog->Log("  ...using GL_NV_occlusion_query\n");
    m_Features |= RFT_OCCLUSIONTEST;
  }
  else
  {
    SUPPORTS_GL_NV_occlusion_query = 0;
    iLog->Log("  ...ignoring GL_NV_occlusion_query\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_multisample_filter_hint)
    iLog->Log("  ...GL_NV_multisample_filter_hint not found\n");
  else
  if (CV_gl_nv_multisample_filter_hint)
    iLog->Log("  ...using GL_NV_multisample_filter_hint\n");
  else
  {
    SUPPORTS_GL_NV_multisample_filter_hint = 0;
    iLog->Log("  ...ignoring GL_NV_multisample_filter_hint\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_fog_distance)
    iLog->Log("  ...GL_NV_fog_distance not found\n");
  else
  if (CV_gl_nv_fog_distance)
  {
    iLog->Log("  ...using GL_NV_fog_distance\n");
  }
  else
  {
    SUPPORTS_GL_NV_fog_distance = 0;
    iLog->Log("  ...ignoring GL_NV_fog_distance\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_texgen_emboss)
    iLog->Log("  ...GL_NV_texgen_emboss not found\n");
  else
  if (CV_gl_nv_texgen_emboss)
  {
    iLog->Log("  ...using GL_NV_texgen_emboss\n");
    m_Features |= RFT_TEXGEN_EMBOSS;
  }
  else
  {
    SUPPORTS_GL_NV_texgen_emboss = 0;
    iLog->Log("  ...ignoring GL_NV_texgen_emboss\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_separate_specular_color)
    iLog->Log("  ...GL_EXT_separate_specular_color not found\n");
  else
  if (CV_gl_ext_separate_specular_color)
  {
    iLog->Log("  ...using GL_EXT_separate_specular_color\n");
    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL_EXT, GL_SEPARATE_SPECULAR_COLOR_EXT);
  }
  else
  {
    SUPPORTS_GL_EXT_separate_specular_color = 0;
    iLog->Log("  ...ignoring GL_EXT_separate_specular_color\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_texture_env_combine)
    iLog->Log("  ...GL_EXT_texture_env_combine not found\n");
  else
  if (CV_gl_ext_texture_env_combine)
    iLog->Log("  ...using GL_EXT_texture_env_combine\n");
  else
  {
    SUPPORTS_GL_EXT_texture_env_combine = 0;
    iLog->Log("  ...ignoring GL_EXT_texture_env_combine\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_multi_draw_arrays)
    iLog->Log("  ...GL_EXT_multi_draw_arrays not found\n");
  else
  if (CV_gl_ext_multi_draw_arrays)
  {
    iLog->Log("  ...using GL_EXT_multi_draw_arrays\n");
  }
  else
  {
    SUPPORTS_GL_EXT_multi_draw_arrays = 0;
    iLog->Log("  ...ignoring GL_EXT_multi_draw_arrays\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_texture_env_combine4)
    iLog->Log("  ...GL_NV_texture_env_combine4 not found\n");
  else
  if (CV_gl_nv_texture_env_combine4)
  {
    iLog->Log("  ...using GL_NV_texture_env_combine4\n");
    if (GetFeatures() & RFT_MULTITEXTURE)
      m_Features |= RFT_DETAILTEXTURE;
  }
  else
  {
    SUPPORTS_GL_NV_texture_env_combine4 = 0;
    iLog->Log("  ...ignoring GL_NV_texture_env_combine4\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_point_sprite)
    iLog->Log("  ...GL_NV_point_sprite not found\n");
  else
  if (CV_gl_nv_point_sprite)
  {
    iLog->Log("  ...using GL_NV_point_sprite\n");
  }
  else
  {
    SUPPORTS_GL_NV_point_sprite = 0;
    iLog->Log("  ...ignoring GL_NV_point_sprite\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_vertex_array_range)
    iLog->Log("  ...GL_NV_vertex_array_range not found\n");
  else
  if (CV_gl_nv_vertex_array_range)
    iLog->Log("  ...using GL_NV_vertex_array_range\n");
  else
  {
    SUPPORTS_GL_NV_vertex_array_range = 0;
    iLog->Log("  ...ignoring GL_NV_vertex_array_range\n");
    wglAllocateMemoryNV = NULL;
    glVertexArrayRangeNV = NULL;
    wglFreeMemoryNV = NULL;
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_fence)
    iLog->Log("  ...GL_NV_fence not found\n");
  else
  if (CV_gl_nv_fence)
    iLog->Log("  ...using GL_NV_fence\n");
  else
  {
    SUPPORTS_GL_NV_fence = 0;
    iLog->Log("  ...ignoring GL_NV_fence\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_compiled_vertex_array)
    iLog->Log("  ...GL_EXT_compiled_vertex_array not found.\n");
  else
  if ( CV_gl_ext_compiled_vertex_array)
    iLog->Log("  ...using GL_EXT_compiled_vertex_array.\n");
  else
  {
    SUPPORTS_GL_EXT_compiled_vertex_array = 0;
    glLockArraysEXT = NULL;
    glUnlockArraysEXT = NULL;
    iLog->Log("  ...ignoring GL_EXT_compiled_vertex_array.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_texture_env_add)
    iLog->Log("  ...GL_EXT_texture_env_add not found.\n");
  else
  if ( CV_gl_ext_texture_env_add)
    iLog->Log("  ...using GL_EXT_texture_env_add.\n");
  else
  {
    SUPPORTS_GL_EXT_texture_env_add = 0;
    iLog->Log("  ...ignoring GL_EXT_texture_env_add.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_texture_filter_anisotropic)
    iLog->Log("  ...GL_EXT_texture_filter_anisotropic not found.\n");
  else
  if ( CV_gl_ext_texture_filter_anisotropic)
  {
    iLog->Log("  ...using GL_EXT_texture_filter_anisotropic.\n");
    glGetIntegerv( GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &m_MaxAnisotropicLevel );
    m_Features |= RFT_ALLOWANISOTROPIC;
    if (!CV_r_texture_anisotropic_level)
      CV_r_texture_anisotropic_level = 1;
  }
  else
  {
    SUPPORTS_GL_EXT_texture_filter_anisotropic = 0;
    iLog->Log("  ...ignoring GL_EXT_texture_filter_anisotropic.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_bgra)
    iLog->Log("  ...GL_EXT_bgra not found.\n");
  else
  if ( CV_gl_ext_bgra)
    iLog->Log("  ...using GL_EXT_bgra.\n");
  else
  {
    SUPPORTS_GL_EXT_bgra = 0;
    iLog->Log("  ...ignoring GL_EXT_bgra.\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_depth_bounds_test)
    iLog->Log("  ...GL_EXT_depth_bounds_test not found.\n");
  else
  if (CV_gl_ext_depth_bounds_test)
    iLog->Log("  ...using GL_EXT_depth_bounds_test.\n");
  else
  {
    SUPPORTS_GL_EXT_depth_bounds_test = 0;
    iLog->Log("  ...ignoring GL_EXT_depth_bounds_test.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_secondary_color)
    iLog->Log("  ...GL_EXT_secondary_color not found.\n");
  else
  if (CV_gl_ext_secondary_color)
  {
    iLog->Log("  ...using GL_EXT_secondary_color.\n");
    m_Features |= RFT_ALLOWSECONDCOLOR;
  }
  else
  {
    SUPPORTS_GL_EXT_secondary_color = 0;
    iLog->Log("  ...ignoring GL_EXT_secondary_color.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_paletted_texture)
    iLog->Log("  ...GL_EXT_paletted_texture not found.\n");
  else
  if (CV_gl_ext_paletted_texture && CV_r_supportpalettedtextures)
  {
    iLog->Log("  ...using GL_EXT_paletted_texture.\n");
    m_Features |= RFT_PALTEXTURE;
  }
  else
  {
    SUPPORTS_GL_EXT_paletted_texture = 0;
    iLog->Log("  ...ignoring GL_EXT_paletted_texture.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_stencil_two_side)
    iLog->Log("  ...GL_EXT_stencil_two_side not found.\n");
  else
  if (CV_gl_ext_stencil_two_side)
    iLog->Log("  ...using GL_EXT_stencil_two_side.\n");
  else
  {
    SUPPORTS_GL_EXT_stencil_two_side = 0;
    iLog->Log("  ...ignoring GL_EXT_stencil_two_side.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_stencil_wrap)
    iLog->Log("  ...GL_EXT_stencil_wrap not found.\n");
  else
  if (CV_gl_ext_stencil_wrap)
    iLog->Log("  ...using GL_EXT_stencil_wrap.\n");
  else
  {
    SUPPORTS_GL_EXT_stencil_wrap = 0;
    iLog->Log("  ...ignoring GL_EXT_stencil_wrap.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_register_combiners)
    iLog->Log("  ...GL_NV_register_combiners not found.\n");
  else
  if (CV_gl_nv_register_combiners)
  {
    iLog->Log("  ...using GL_NV_register_combiners.\n");
    m_Features &= ~RFT_HW_MASK;
    m_Features |= RFT_HW_GF2;
    m_Features |= RFT_FOGVP;
  }
  else
  {
    SUPPORTS_GL_NV_register_combiners = 0;
    iLog->Log("  ...ignoring GL_NV_register_combiners.\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_register_combiners2)
    iLog->Log("  ...GL_NV_register_combiners2 not found.\n");
  else
  if (CV_gl_nv_register_combiners2)
  {
    iLog->Log("  ...using GL_NV_register_combiners2.\n");
    m_Features |= RFT_HW_RC;
    m_Features |= RFT_FOGVP;
    glEnable(GL_PER_STAGE_CONSTANTS_NV);
  }
  else
  {
    SUPPORTS_GL_NV_register_combiners2 = 0;
    iLog->Log("  ...ignoring GL_NV_register_combiners2.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_vertex_program)
    iLog->Log("  ...GL_NV_vertex_program not found.\n");
  else
  if (CV_gl_nv_vertex_program)
  {
    iLog->Log("  ...using GL_NV_vertex_program.\n");
    m_Features |= RFT_HW_VS;
    m_Features |= RFT_FOGVP;
  }
  else
  {
    SUPPORTS_GL_NV_vertex_program = 0;
    iLog->Log("  ...ignoring GL_NV_vertex_program3.\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_ARB_vertex_program)
    iLog->Log("  ...GL_ARB_vertex_program not found.\n");
  else
  if (CV_gl_arb_vertex_program)
  {
    iLog->Log("  ...using GL_ARB_vertex_program.\n");
    m_Features |= RFT_HW_VS;
    m_Features |= RFT_FOGVP;
  }
  else
  {
    SUPPORTS_GL_ARB_vertex_program = 0;
    iLog->Log("  ...ignoring GL_ARB_vertex_program.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_texture_shader)
    iLog->Log("  ...GL_NV_texture_shader not found.\n");
  else
  if (CV_gl_nv_texture_shader)
  {
    iLog->Log("  ...using GL_NV_texture_shader.\n");
    m_Features |= RFT_HW_TS;
    m_Features &= ~RFT_HW_MASK;
    m_Features |= RFT_HW_GF3;
    m_Features |= RFT_FOGVP;
  }
  else
  {
    SUPPORTS_GL_NV_texture_shader = 0;
    iLog->Log("  ...ignoring GL_NV_texture_shader.\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_texture_shader2)
    iLog->Log("  ...GL_NV_texture_shader2 not found.\n");
  else
  if (CV_gl_nv_texture_shader2)
  {
    iLog->Log("  ...using GL_NV_texture_shader2.\n");
    m_Features |= RFT_HW_TS;
    m_Features |= RFT_FOGVP;
  }
  else
  {
    SUPPORTS_GL_NV_texture_shader2 = 0;
    iLog->Log("  ...ignoring GL_NV_texture_shader2.\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_texture_shader3)
    iLog->Log("  ...GL_NV_texture_shader3 not found.\n");
  else
  if (CV_gl_nv_texture_shader3)
  {
    iLog->Log("  ...using GL_NV_texture_shader3.\n");
    m_Features |= RFT_HW_TS | RFT_HW_ENVBUMPPROJECTED;
  }
  else
  {
    SUPPORTS_GL_NV_texture_shader3 = 0;
    iLog->Log("  ...ignoring GL_NV_texture_shader3.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_texture_rectangle)
    iLog->Log("  ...GL_NV_texture_rectangle not found.\n");
  else
  if (CV_gl_nv_texture_rectangle)
  {
    iLog->Log("  ...using GL_NV_texture_rectangle.\n");
    m_Features |= RFT_ALLOWRECTTEX;
  }
  else
  {
    SUPPORTS_GL_NV_texture_rectangle = 0;
    iLog->Log("  ...ignoring GL_NV_texture_rectangle.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_texture_rectangle)
    iLog->Log("  ...GL_EXT_texture_rectangle not found.\n");
  else
  if (CV_gl_ext_texture_rectangle && !SUPPORTS_GL_NV_texture_rectangle)
  {
    iLog->Log("  ...using GL_EXT_texture_rectangle.\n");
    m_Features |= RFT_ALLOWRECTTEX;
  }
  else
  {
    SUPPORTS_GL_EXT_texture_rectangle = 0;
    iLog->Log("  ...ignoring GL_EXT_texture_rectangle.\n");
  }

/////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_EXT_texture_cube_map)
    iLog->Log("  ...GL_EXT_texture_cube_map not found.\n");
  else
  if (CV_gl_ext_texture_cube_map)
  {
    iLog->Log("  ...using GL_EXT_texture_cube_map.\n");
  }
  else
  {
    SUPPORTS_GL_EXT_texture_cube_map = 0;
    iLog->Log("  ...ignoring GL_EXT_texture_cube_map.\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_ATI_separate_stencil)
    iLog->Log("  ...GL_ATI_separate_stencil not found.\n");
  else
  if (CV_gl_ati_separate_stencil)
  {
    iLog->Log("  ...using GL_ATI_separate_stencil.\n");
  }
  else
  {
    SUPPORTS_GL_ATI_separate_stencil = 0;
    iLog->Log("  ...ignoring GL_ATI_separate_stencil.\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_ATI_fragment_shader)
    iLog->Log("  ...GL_ATI_fragment_shader not found.\n");
  else
  if (CV_gl_ati_fragment_shader)
  {
    iLog->Log("  ...using GL_ATI_fragment_shader.\n");
    m_Features &= ~RFT_HW_MASK;
    m_Features |= RFT_HW_RADEON;
  }
  else
  {
    SUPPORTS_GL_ATI_fragment_shader = 0;
    iLog->Log("  ...ignoring GL_ATI_fragment_shader.\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_ARB_vertex_buffer_object)
    iLog->Log("  ...GL_ARB_vertex_buffer_object not found.\n");
  else
  if (CV_gl_arb_vertex_buffer_object || !SUPPORTS_GL_NV_fence)
  {
    SUPPORTS_GL_ATI_fragment_shader = 0;
    SUPPORTS_GL_NV_fence = 0;
    SUPPORTS_GL_NV_vertex_array_range = 0;
    iLog->Log("  ...using GL_ARB_vertex_buffer_object.\n");
  }
  else
  {
    SUPPORTS_GL_ARB_vertex_buffer_object = 0;
    iLog->Log("  ...ignoring GL_ARB_vertex_buffer_object.\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_NV_fragment_program)
    iLog->Log("  ...GL_NV_fragment_program not found.\n");
  else
  if (CV_gl_nv_fragment_program)
  {
    iLog->Log("  ...using GL_NV_fragment_program.\n");
    SUPPORTS_GL_ATI_fragment_shader = 0;
    SUPPORTS_GL_ARB_fragment_program = 0;
    m_Features |= RFT_HW_TS | RFT_HW_PS20;
    m_Features &= ~RFT_HW_MASK;
    m_Features |= RFT_HW_GFFX;
    m_Features |= RFT_HW_ENVBUMPPROJECTED;
    //m_MaxActiveTexturesARB = Max(m_MaxActiveTexturesARB, 6);
  }
  else
  {
    SUPPORTS_GL_NV_fragment_program = 0;
    iLog->Log("  ...ignoring GL_NV_fragment_program.\n");
  }


  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_ARB_fragment_program)
    iLog->Log("  ...GL_ARB_fragment_program not found.\n");
  else
  if (CV_gl_arb_fragment_program && !SUPPORTS_GL_NV_fragment_program)
  {
    iLog->Log("  ...using GL_ARB_fragment_program.\n");
    SUPPORTS_GL_ATI_fragment_shader = 0;
    m_Features |= RFT_HW_TS | RFT_HW_PS20;
    if (((m_Features & RFT_HW_MASK) != RFT_HW_RADEON))
    {
      m_Features &= ~RFT_HW_MASK;
      m_Features |= RFT_HW_GFFX;
    }
    m_Features |= RFT_HW_ENVBUMPPROJECTED;
    m_Features |= RFT_FOGVP;
    //m_MaxActiveTexturesARB = Max(m_MaxActiveTexturesARB, 6);
  }
  else
  {
    SUPPORTS_GL_ARB_fragment_program = 0;
    iLog->Log("  ...ignoring GL_ARB_fragment_program.\n");
  }


  if (!SUPPORTS_GL_NV_vertex_program3)
    iLog->Log("  ...GL_NV_vertex_program3 not found.\n");
  else
  if (CV_gl_nv_vertex_program3)
  {
    iLog->Log("  ...using GL_NV_vertex_program3.\n");
    m_Features |= RFT_HW_VS;
    m_Features |= RFT_FOGVP;
    m_Features &= ~RFT_HW_MASK;
    m_Features |= RFT_HW_NV4X;
    if (SUPPORTS_GL_ARB_vertex_program)
      SUPPORTS_GL_NV_vertex_program = 0;
  }
  else
  {
    SUPPORTS_GL_NV_vertex_program3 = 0;
    iLog->Log("  ...ignoring GL_NV_vertex_program3.\n");
  }

  /////////////////////////////////////////////////////////////////////////////////////

  if (!SUPPORTS_GL_ARB_texture_env_combine)
    iLog->Log("  ...GL_ARB_texture_env_combine not found.\n");
  else
  if (CV_gl_arb_fragment_program)
  {
    iLog->Log("  ...using GL_ARB_texture_env_combine.\n");
  }
  else
  {
    SUPPORTS_GL_ARB_texture_env_combine = 0;
    iLog->Log("  ...ignoring GL_ARB_texture_env_combine.\n");
  }

  m_Features |= RFT_BUMP;
  if (SUPPORTS_GL_ARB_fragment_program || (m_Features & RFT_DEPTHMAPS))
    m_Features |= RFT_SHADOWMAP_SELFSHADOW;
  if (CV_r_shadowtype == 1)
    m_Features &= ~(RFT_SHADOWMAP_SELFSHADOW | RFT_DEPTHMAPS);

  iLog->Log("\n");


  if (SUPPORTS_GL_ARB_fragment_program)
  {
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS_ARB, &m_MaxActiveTexturesARB_VP);
    glGetIntegerv(GL_MAX_TEXTURES_UNITS_ARB, &m_MaxActiveTexturesARBFixed);
  }
  else
  {
    glGetIntegerv(GL_MAX_TEXTURES_UNITS_ARB, &m_MaxActiveTexturesARBFixed);
    m_MaxActiveTexturesARB_VP = m_MaxActiveTexturesARBFixed;
  }
  m_MaxActiveTexturesARB_VP = min(m_MaxActiveTexturesARB_VP, MAX_TMU);
  m_MaxActiveTexturesARBFixed = min(m_MaxActiveTexturesARBFixed, MAX_TMU);

  return true;
}

void CGLRenderer::MakeCurrent()
{
  int i;

  if (pwglGetCurrentContext() == m_RContexts[0]->m_hRC)
    return;
  if (pwglMakeCurrent(m_RContexts[0]->m_hDC, m_RContexts[0]->m_hRC) != TRUE)
	{
		if (iLog)
			iLog->LogToFile( "Warning: MakeCurrent of OpenGL context failed!" );
	}
  for (i=0; i<96; i++)
  {
    CCGVProgram_GL::m_CurParams[i][0] = -999999;
    CCGVProgram_GL::m_CurParams[i][1] = -999999;
    CCGVProgram_GL::m_CurParams[i][2] = -999999;
    CCGVProgram_GL::m_CurParams[i][3] = -999999;
  }
  CVProgram::m_LastVP = 0;
  CPShader::m_CurRC = NULL;
}

void CGLRenderer::ShareResources( IRenderer *renderer )
{
  // Ignore share with myself!
  if (renderer == this)
    return;

  // Assume that renderer is of same type.
  CGLRenderer *glRend = (CGLRenderer*)renderer;

  if (pwglShareLists( m_CurrContext->m_hRC, glRend->m_CurrContext->m_hRC ) == FALSE)
  {
    if (iLog)
      iLog->Log("Warning: Sharing of OpenGL resources failed!" );
  }
}


//////////////////////////////////////////////////////////////////////
bool CGLRenderer::SetupPixelFormat(unsigned char colorbits,unsigned char zbits,unsigned char sbits,SRendContext *rc)
{
#ifdef WIN32
  iLog->LogToFile("SetupPixelFormat ...");

  PIXELFORMATDESCRIPTOR pfd;
  int pixelFormat;

  memset(&pfd, 0, sizeof(pfd));

  pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER;
  pfd.dwLayerMask = PFD_MAIN_PLANE;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = colorbits;
  pfd.cAlphaBits = CV_gl_alpha_bits;
  pfd.cDepthBits = zbits;
  pfd.cAccumBits = 0;
  pfd.cStencilBits = sbits;

  BOOL status = FALSE;

  if (m_RContexts.Num() && rc != m_RContexts[0])
    pixelFormat = m_RContexts[0]->m_PixFormat;
  else
    pixelFormat = ChoosePixelFormat(m_CurrContext->m_hDC, &pfd);
  m_CurrContext->m_PixFormat = pixelFormat;

  if (!pixelFormat)
  {
    iLog->LogToFile("Cannot ChoosePixelFormat \n");
    return (false);
  }

  iLog->LogToFile("Selected cColorBits = %d, cDepthBits = %d, Stencilbits=%d\n", pfd.cColorBits, pfd.cDepthBits,pfd.cStencilBits);

  iLog->LogToFile("GL PIXEL FORMAT:\n");

  memset(&pfd, 0, sizeof(pfd));
  if (DescribePixelFormat(m_CurrContext->m_hDC, pixelFormat, sizeof(PIXELFORMATDESCRIPTOR), &pfd))
  {
    if (pfd.dwFlags & PFD_DRAW_TO_WINDOW)   iLog->LogToFile("PFD_DRAW_TO_WINDOW\n");
    if (pfd.dwFlags & PFD_DRAW_TO_BITMAP)   iLog->LogToFile("PFD_DRAW_TO_BITMAP\n");

    if (pfd.dwFlags & PFD_SUPPORT_GDI)      iLog->LogToFile("PFD_SUPPORT_GDI\n");

    if (pfd.dwFlags & PFD_SUPPORT_OPENGL)   iLog->LogToFile("PFD_SUPPORT_OPENGL\n");
    if (pfd.dwFlags & PFD_GENERIC_ACCELERATED)  iLog->LogToFile("PFD_GENERIC_ACCELERATED\n");

    if (pfd.dwFlags & PFD_NEED_PALETTE)     iLog->LogToFile("PFD_NEED_PALETTE\n");
    if (pfd.dwFlags & PFD_NEED_SYSTEM_PALETTE)  iLog->LogToFile("PFD_NEED_SYSTEM_PALETTE\n");
    if (pfd.dwFlags & PFD_DOUBLEBUFFER)     iLog->LogToFile("PFD_DOUBLEBUFFER\n");
    if (pfd.dwFlags & PFD_STEREO)       iLog->LogToFile("PFD_STEREO\n");
    if (pfd.dwFlags & PFD_SWAP_LAYER_BUFFERS) iLog->LogToFile("PFD_SWAP_LAYER_BUFFERS\n");
    iLog->LogToFile("Pixel format %d:\n",pixelFormat);
    iLog->LogToFile("Pixel Type: %d\n",pfd.iPixelType);
    iLog->LogToFile("Bits: Color=%d R=%d G=%d B=%d A=%d\n", pfd.cColorBits, pfd.cRedBits, pfd.cGreenBits, pfd.cBlueBits, pfd.cAlphaBits);
    iLog->LogToFile("Bits: Accum=%d Depth=%d Stencil=%d\n", pfd.cAccumBits, pfd.cDepthBits, pfd.cStencilBits);

    iLog->LogToFile("Using cColorBits = %d, cDepthBits = %d, Stencilbits=%d\n", pfd.cColorBits, pfd.cDepthBits,pfd.cStencilBits);

    if (pfd.dwFlags & PFD_GENERIC_FORMAT)
    {
      iLog->LogToFile("PFD_GENERIC_FORMAT\n");
      return (false);
    }

    m_abpp = pfd.cAlphaBits;
    m_sbpp = pfd.cStencilBits;
    m_cbpp = pfd.cColorBits;
    m_zbpp = pfd.cDepthBits;
  }
  else
  {
    iLog->LogToFile("Warning: Cannot DescribePixelFormat \n");
    return (false);
  }

  // set the format to closest match
  SetPixelFormat(m_CurrContext->m_hDC, pixelFormat, &pfd);

  return (true);

#else

  return (false);

#endif
}

//! Return all supported by video card video AA formats
int CGLRenderer::EnumAAFormats(TArray<SAAFormat>& Formats, bool bReset)
{
  return 0;
}

int CGLRenderer::EnumDisplayFormats(TArray<SDispFormat>& Formats, bool bReset)
{
  SDispFormat frm;
  Formats.Free();

  if (bReset)
    return 0;

  int i, j;
  for (i=0; i<m_numvidmodes; i++)
  {
    DEVMODE *m = &m_vidmodes[i];
    if (m->dmPelsWidth < 640 || m->dmPelsHeight < 480 || m->dmBitsPerPel < 15)
      continue;
    for (j=0; j<Formats.Num(); j++)
    {
      SDispFormat *f = &Formats[j];
      if (f->m_Width == m->dmPelsWidth && f->m_Height == m->dmPelsHeight && f->m_BPP == m->dmBitsPerPel)
        break;
    }
    if (j == Formats.Num())
    {
      frm.m_Width = m->dmPelsWidth;
      frm.m_Height = m->dmPelsHeight;
      frm.m_BPP = m->dmBitsPerPel;
      if (frm.m_BPP == 24)
        frm.m_BPP = 32;
      Formats.AddElem(frm);
    }
  }
  return Formats.Num();
}

HWND CGLRenderer::SetMode(int x,int y,int width,int height,unsigned int cbpp, int zbpp, int sbits, bool fullscreen,HINSTANCE hinst, HWND Glhwnd)
{
#ifdef GLES3_RENDERER
  {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
    {
      iLog->Log("Error: SDL video init failed: %s", SDL_GetError());
      return NULL;
    }
    SDL_DisplayMode dm;
    m_deskwidth = width; m_deskheight = height; m_deskbpp = 32;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) { m_deskwidth = dm.w; m_deskheight = dm.h; }
    iLog->Log("Desktop settings: %d x %d (SDL video driver '%s')", m_deskwidth, m_deskheight, SDL_GetCurrentVideoDriver());

    const int depthTries[2] = { zbpp >= 24 ? 24 : 16, 16 };
    for (int attempt = 0; attempt < 8 && !g_GLES3Context; ++attempt)
    {
      const bool fs = fullscreen && (attempt & 3) < 2;
      const int alphaBits = (attempt < 4 && getenv("FARCRY_GLES_ALPHA")) ? 8 : 0; // opt-in destination alpha (costs a clear per frame)
      SDL_GL_ResetAttributes();
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
      SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
      SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
      SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, alphaBits);
      SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depthTries[attempt & 1]);
      SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, sbits > 0 ? 8 : 0);
      // Fullscreen uses the screen's own mode (no mode switch); the game's
      // resolution is rendered offscreen and scaled (GLES3_SceneSetup).
      // FARCRY_WINDOW_SIZE=WxH sizes a desktop window independently (testing).
      int winW = width, winH = height;
      if (const char* ws = getenv("FARCRY_WINDOW_SIZE")) sscanf(ws, "%dx%d", &winW, &winH);
      Uint32 flags = SDL_WINDOW_OPENGL | (fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
      if (!g_GLES3Window)
        g_GLES3Window = SDL_CreateWindow("Far Cry", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH, flags);
      if (!g_GLES3Window)
        continue;
      g_GLES3Context = SDL_GL_CreateContext(g_GLES3Window);
      if (!g_GLES3Context)
      {
        iLog->Log("Warning: GLES3 context (depth %d, %s) failed: %s", depthTries[attempt & 1], fs ? "fullscreen" : "windowed", SDL_GetError());
        SDL_DestroyWindow(g_GLES3Window);
        g_GLES3Window = NULL;
      }
    }
    if (!g_GLES3Context)
    {
      iLog->Log("Error: could not create an OpenGL ES 3.0 context: %s", SDL_GetError());
      return NULL;
    }
    SDL_GL_MakeCurrent(g_GLES3Window, g_GLES3Context);
    {
      int a = 0, d = 0; SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a); SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &d);
      s_bGLES3SurfaceAlpha = a > 0;
      iLog->Log("GLES3 surface: alpha %d bits, depth %d bits%s", a, d, a > 0 ? "" : " (no destination alpha: terrain layers/sprites degrade)");
    }
    GLES3FFPInit(&GLES3LogBridge);
    if (GLR::Active() && !GLR::Init()) iLog->Log("GLR: native backend could not start");
    GLTex::SetTexNameLookup(&GLES3TexNameOf);
    GLES3FFPSetAuditHook(&GLES3AuditTextureNames);
    int dw = width, dh = height;
    SDL_GL_GetDrawableSize(g_GLES3Window, &dw, &dh);
    iLog->Log("GLES3 window: requested %dx%d, drawable %dx%d, fullscreen=%d", width, height, dw, dh, (int)fullscreen);
    GLR::Sync([&] { GLES3_SceneSetup(width, height, dw, dh); });
    m_width = width;
    m_height = height;
    m_FullScreen = fullscreen;
    m_VX = m_VY = 0;
    m_VWidth = width;
    m_VHeight = height;
    return (HWND)g_GLES3Window;
  }
#else
  ///////////////////////////////////Get Desktop Settings

  HWND temp = GetDesktopWindow();
  RECT trect;

  GetWindowRect(temp, &trect);

  m_deskwidth =trect.right-trect.left;
  m_deskheight=trect.bottom-trect.top;

  HDC hDC = GetDC (NULL);
  m_deskbpp = GetDeviceCaps( hDC, BITSPIXEL );
//  m_deskfreq= GetDeviceCaps( hDC, VREFRESH );
  ReleaseDC (NULL,hDC);


  iLog->Log("Desktop settings: %d x %d x %d",m_deskwidth,m_deskheight,m_deskbpp/*,m_deskfreq*/);
  iLog->Log("Available video modes:\n");


  ///////////////////////////////////Find available video modes

  int nummode;
  for (nummode=0;;nummode++)
  {
    DEVMODE Tmp;
    ZeroMemory(&Tmp,sizeof(Tmp));
    Tmp.dmSize = sizeof(Tmp);
    if (!EnumDisplaySettings(NULL,nummode,&Tmp))
      break;
  } //nummode

  if (nummode>0)
  {
    if (m_vidmodes)
      delete [] m_vidmodes;
    m_vidmodes = new DEVMODE [m_numvidmodes=nummode];
  }

  for (nummode=0;;nummode++)
  {
    DEVMODE Tmp;
    ZeroMemory(&Tmp,sizeof(Tmp));
    Tmp.dmSize = sizeof(Tmp);

    if (!EnumDisplaySettings(NULL,nummode,&Tmp))
      break;

    m_vidmodes[nummode]=Tmp;
  } //nummode

  if (m_numvidmodes==0)
  {
    iLog->Log("Warning: Cannot find at least one video mode available\n");
    return (NULL);
  }

  ///////////////////////////////////Find the best video mode
  if (width < 640)
    width = 640;
  if (height < 480)
    height = 480;
  if (cbpp < 16)
    cbpp = 16;

  int best_mode=0;

  for (int mode = 0; mode < m_numvidmodes; mode++)
  {
    if (((int)(m_vidmodes[mode].dmPelsWidth)  >= (int)(m_vidmodes[best_mode].dmPelsWidth)) &&
      ((int)(m_vidmodes[mode].dmPelsWidth)  <= width) &&
      ((int)(m_vidmodes[mode].dmPelsHeight) >= (int)(m_vidmodes[best_mode].dmPelsHeight)) &&
      ((int)(m_vidmodes[mode].dmPelsHeight) <= height) &&
      ((int)(m_vidmodes[mode].dmBitsPerPel) >= (int)(m_vidmodes[best_mode].dmBitsPerPel)) &&
      ((int)(m_vidmodes[mode].dmBitsPerPel) <= cbpp))
      best_mode = mode;
  }

///////////////////////////////////Create window

  if (width==m_deskwidth && height==m_deskheight)
    fullscreen=true;

	char szWinTitle[80];
	sprintf(szWinTitle,"- Far Cry - %s (%s)",__DATE__, __TIME__);

  if (fullscreen)
  {
    if (!ChangeDisplay(m_vidmodes[best_mode].dmPelsWidth,m_vidmodes[best_mode].dmPelsHeight,m_vidmodes[best_mode].dmBitsPerPel))
      return (false);
    Glhwnd = CreateWindowEx(WS_EX_APPWINDOW,
                      "CryENGINE", szWinTitle,
                      WS_POPUP |  WS_CLIPSIBLINGS,
                      0, 0, width, height,
                      NULL, NULL, hinst, NULL);

    m_width=m_vidmodes[best_mode].dmPelsWidth;
    m_height=m_vidmodes[best_mode].dmPelsHeight;

    SetCursor(NULL);
    ShowCursor(FALSE);
  }
  else
  {
    if(!Glhwnd)
		{
			WNDCLASS wc;

			if (!GetClassInfo(hinst, "CryENGINE", &wc))
			{

				wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
				wc.lpfnWndProc   = DefWindowProc;
				wc.cbClsExtra    = 4;
				wc.cbWndExtra    = 4;
				wc.hInstance     = hinst;
				wc.hIcon         = NULL;
				wc.hCursor       = NULL;
				wc.hbrBackground = (HBRUSH)(BLACK_BRUSH);
				wc.lpszMenuName  = NULL;
				wc.lpszClassName = "CryENGINE";

				RegisterClass(&wc);
			}

      Glhwnd = CreateWindowEx(WS_EX_APPWINDOW,
                  "CryENGINE", szWinTitle,
                  WS_BORDER | WS_DLGFRAME | WS_CLIPSIBLINGS | WS_SYSMENU,
                  (GetSystemMetrics(SM_CXFULLSCREEN)-width)/2,
                  (GetSystemMetrics(SM_CYFULLSCREEN)-height)/2,
                  GetSystemMetrics(SM_CXDLGFRAME)*2 + width,
                  GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CXDLGFRAME)*2 + height,
                  NULL, NULL, hinst, NULL);
		}

    DWORD err=GetLastError();

    m_width = width;
    m_height = height;
  }
  m_VX = m_VY = 0;
  m_VWidth = m_width;
  m_VHeight = m_height;

  if (!Glhwnd)
  {
    iLog->Log("Error: CreateWindowEx\n");
    return (false);
  }

  m_FullScreen = fullscreen;

  if (!m_FullScreen)
    SetWindowPos(Glhwnd, HWND_NOTOPMOST, (GetSystemMetrics(SM_CXFULLSCREEN)-width)/2, (GetSystemMetrics(SM_CYFULLSCREEN)-height)/2, GetSystemMetrics(SM_CXDLGFRAME)*2 + width, GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CXDLGFRAME)*2 + height, SWP_SHOWWINDOW);

  //SAFE_DELETE_ARRAY(m_vidmodes);

  return Glhwnd;
#endif
}

//////////////////////////////////////////////////////////////////////

typedef BOOL (WINAPI * PFNWGLGETPIXELFORMATATTRIBIVARBPROC) (HDC hdc, int iPixelFormat, int iLayerPlane, UINT nAttributes, const int *piAttributes, int *piValues);
typedef BOOL (WINAPI * PFNWGLGETPIXELFORMATATTRIBFVARBPROC) (HDC hdc, int iPixelFormat, int iLayerPlane, UINT nAttributes, const int *piAttributes, FLOAT *pfValues);
typedef BOOL (WINAPI * PFNWGLCHOOSEPIXELFORMATARBPROC) (HDC hdc, const int *piAttribIList, const FLOAT *pfAttribFList, UINT nMaxFormats, int *piFormats, UINT *nNumFormats);
typedef PROC (WINAPI * PFNWGLGETPROCADDRESSPROC) (LPCSTR str);

// Determine if an OpenGL WGL extension is supported.
//
// NOTE:  This routine uses wglGetProcAddress so this routine REQUIRES
// that the calling thread is bound to a hardware-accelerated OpenGL
// rendering context.
int CGLRenderer::_wglExtensionSupported(const char *extension)
{
  // Lazy initialization - We don't know if we've initialized this yet, so do it here.
  wglGetExtensionsStringEXT = (PFNWGLGETEXTENSIONSSTRINGEXTPROC) pwglGetProcAddress("wglGetExtensionsStringEXT");
  wglGetExtensionsStringARB = (PFNWGLGETEXTENSIONSSTRINGARBPROC) pwglGetProcAddress("wglGetExtensionsStringARB");

  if (wglGetExtensionsStringARB || wglGetExtensionsStringEXT)
  {
    static const GLubyte *extensions = NULL;
    const GLubyte *start;
    GLubyte *where, *terminator;

    // Extension names should not have spaces.
    where = (GLubyte *) strchr(extension, ' ');
    if (where || *extension == '\0')
      return 0;

    if (!extensions)
    {
      HDC hdc = GetDC(0);
      if (wglGetExtensionsStringARB)
        extensions = (const GLubyte *) wglGetExtensionsStringARB(hdc);
      else
        extensions = (const GLubyte *) wglGetExtensionsStringEXT();
      ::ReleaseDC(0,hdc);
    }

    // It takes a bit of care to be fool-proof about parsing the
    // OpenGL extensions string.  Don't be fooled by sub-strings,
    // etc.
    start = extensions;
    for (;;)
    {
      where = (GLubyte *) strstr((const char *) start, extension);
      if (!where)
        break;
      terminator = where + strlen(extension);
      if (where == start || *(where - 1) == ' ')
      {
        if (*terminator == ' ' || *terminator == '\0')
          return 1;
      }

      start = terminator;
    }
  }

  return 0;
}

//======================================================================

int nTexSize=0;
int nFrameTexSize=0;
static int curBind=-1;
int BindSizes[TX_LASTBIND];
int BindFrame[TX_LASTBIND];
// Texture-memory accounting is indexed by GL texture name; ignore names
// outside the engine's own range instead of writing past the tables.
static bool BindInRange(int name)
{
  if (name >= 0 && name < TX_LASTBIND)
    return true;
  static int s_logged = 0;
  if (s_logged++ < 5 && iLog)
    iLog->Log("Warning: texture name %d outside accounting range [0,%d)", name, TX_LASTBIND);
  return false;
}
int TargetTex[TX_LASTBIND];
static int bAutoMips = 0;

static _inline void sUpdateTexSize(int target, int width, int height, int depth, int internalFormat, int nLevel)
{
  if (target > GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_EXT)
    return;

  int size;
  if (!nLevel && bAutoMips)
  {
    int wdt = width;
    int hgt = height;
    int dpt = depth;
    size = 0;
    while (wdt || hgt || dpt)
    {
      if (!wdt)
        wdt = 1;
      if (!hgt)
        hgt = 1;
      if (!dpt)
        dpt = 1;
      size += CGLTexMan::TexSize(wdt, hgt, dpt, internalFormat);
      wdt >>= 1;
      hgt >>= 1;
      dpt >>= 1;
    }
  }
  else
    size = CGLTexMan::TexSize(width, height, depth, internalFormat);

  assert (curBind >= 0 && curBind < TX_LASTBIND);
  if (!BindInRange(curBind))
    return;
  if (target == GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT)
    size *= 6;

  if (!nLevel)
  {
    nTexSize -= BindSizes[curBind];
    BindSizes[curBind] = size;
  }
  else
  if (nLevel < 0)
  {
    int wdt = width;
    int hgt = height;
    int dpt = depth;
    int mips = 1-nLevel;
    size = 0;
    for (int i=0; i<mips; i++)
    {
      if (!wdt)
        wdt = 1;
      if (!hgt)
        hgt = 1;
      if (!dpt)
        dpt = 1;
      size += CGLTexMan::TexSize(wdt, hgt, dpt, internalFormat);
      wdt >>= 1;
      hgt >>= 1;
      dpt >>= 1;
    }
    BindSizes[curBind] = size;
  }
  else
    BindSizes[curBind] += size;
  nTexSize += size;
}

static int nPrevBindARB[2] = {-1,-1};
void (__stdcall *nnglBindBufferARB) (GLenum target, GLuint param);
void __stdcall nglBindBufferARB (GLenum target, GLuint param)
{
  if (nPrevBindARB[target-GL_ARRAY_BUFFER_ARB] != param)
  {
    nPrevBindARB[target-GL_ARRAY_BUFFER_ARB] = param;
    nnglBindBufferARB(target, param);
  }
}
void (__stdcall *nnglGenBuffersARB) (GLsizei n, GLuint *buffers);
void __stdcall nglGenBuffersARB (GLsizei n, GLuint *buffers)
{
  nnglGenBuffersARB(n, buffers);
  nPrevBindARB[0] = -1;
  nPrevBindARB[1] = -1;
}
void (__stdcall *nnglDeleteBuffersARB) (GLsizei n, const GLuint *buffers);
void __stdcall nglDeleteBuffersARB (GLsizei n, const GLuint *buffers)
{
  nnglDeleteBuffersARB(n, buffers);
  nPrevBindARB[0] = -1;
  nPrevBindARB[1] = -1;
}

void (__stdcall *nnglTexParameteri) (GLenum target, GLenum pname, GLint param);
void __stdcall nglTexParameteri (GLenum target, GLenum pname, GLint param)
{
  if (pname == GL_GENERATE_MIPMAP_SGIS)
    bAutoMips = param;
  nnglTexParameteri(target, pname, param);
}

void (__stdcall *nnglTexImage2D) (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
void __stdcall nglTexImage2D (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
  sUpdateTexSize(target, width, height, 1, internalformat, level);
  nnglTexImage2D (target, level, internalformat, width, height, border, format, type, pixels);
}

void (__stdcall *nnglBindTexture) (GLenum Parm0, GLuint Parm1);
void __stdcall nglBindTexture (GLenum Parm0, GLuint Parm1)
{
  /*float fSize = 0;
  for (int i=0; i<TX_LASTBIND; i++)
  {
    fSize += BindSizes[i];
  }
  fSize /= 1024.0f;
  fSize /= 1024.0f;*/
	assert (Parm1>=0 && Parm1 <TX_LASTBIND);

  curBind = Parm1;
  nnglBindTexture (Parm0, Parm1);
  if (BindInRange((int)Parm1))
    TargetTex[Parm1] = Parm0;
  if (BindInRange((int)Parm1) && BindFrame[Parm1] != gcpOGL->GetFrameID())
  {
    BindFrame[Parm1] = gcpOGL->GetFrameID();
    nFrameTexSize += BindSizes[Parm1];
  }
  if (!Parm1)
    bAutoMips = 0;
}

void (__stdcall *nnglCompressedTexImage2DARB) (GLenum Parm0, GLint Parm1, GLenum Parm2, GLsizei Parm3, GLsizei Parm4, GLint Parm5, GLsizei Parm6, const GLvoid *Parm7);
void __stdcall nglCompressedTexImage2DARB (GLenum Parm0, GLint Parm1, GLenum Parm2, GLsizei Parm3, GLsizei Parm4, GLint Parm5, GLsizei Parm6, const GLvoid *Parm7)
{
  sUpdateTexSize(Parm0, Parm3, Parm4, 1, Parm2, Parm1);
  nnglCompressedTexImage2DARB (Parm0, Parm1, Parm2, Parm3, Parm4, Parm5, Parm6, Parm7);
}

void (__stdcall *nnglCopyTexImage2D) (GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
void __stdcall nglCopyTexImage2D (GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border)
{
  sUpdateTexSize(target, width, height, 1, internalFormat, level);
  nnglCopyTexImage2D (target, level, internalFormat, x, y, width, height, border);
}

void (__stdcall *nnglDeleteTextures) (GLsizei n, const GLuint *textures);
void __stdcall nglDeleteTextures (GLsizei n, const GLuint *textures)
{
  for (int i=0; i<n; i++)
  {
    if (!BindInRange((int)textures[i]))
      continue;
    nTexSize -= BindSizes[textures[i]];
    BindSizes[textures[i]] = 0;
  }
  nnglDeleteTextures (n, textures);
}


#ifndef PS2

WIN_HWND CGLRenderer::Init(int x,int y,int width,int height,unsigned int cbpp, int zbpp, int sbits, bool fullscreen,WIN_HINSTANCE hinst, WIN_HWND Glhwnd, WIN_HDC Glhdc, WIN_HGLRC hGLrc, bool bReInit)
{
#ifdef USE_3DC
    CompressTextureATI = 0;
    DeleteDataATI = 0;
#endif

  m_hInst = (HINSTANCE)hinst;
  if (bReInit)
  {
    CLeafBuffer *pLB = CLeafBuffer::m_Root.m_Prev;
    while(pLB != &CLeafBuffer::m_Root)
    {
      CLeafBuffer *Next = pLB->m_Prev;
      pLB->Unload();
      pLB = Next;
    }
    ShutDownVAR();
    FreeResources(FRR_TEXTURES | FRR_REINITHW);
    ShutDown(true);
  }

  if (!Glhdc)
    Glhwnd = SetMode(x, y, width, height, cbpp, zbpp, sbits, fullscreen, (HINSTANCE)hinst, (HWND)Glhwnd);
  else
  {
    m_width = width;
    m_height = height;
    m_FullScreen = fullscreen;
    m_VX = m_VY = 0;
    m_VWidth = m_width;
    m_VHeight = m_height;
  }
  if ( !LoadLibrary() )
  {
    iLog->Log("Error: Could not open OpenGL library\n");
exr:
    FreeLibrary();
    ShutDown();
    return NULL;
  }
  if (!m_RContexts.Num())
  {
    SRendContext *rc = new SRendContext;
    m_RContexts.AddElem(rc);
  }
  SRendContext *rc = m_RContexts[0];
  if (Glhdc)
  {
    rc->m_hDC = (HDC)Glhdc;
  }
  else
  {
#ifdef GLES3_RENDERER
    rc->m_hDC = (HDC)Glhwnd; // no GDI device context; SDL owns the surface
#else
    rc->m_hDC = GetDC((HWND)Glhwnd);
#endif
  }
  rc->m_Glhwnd = (HWND)Glhwnd;
  m_CurrContext = rc;

  // Find functions.
  SUPPORTS_GL = 1;
  FindProcs( false );
  if( !SUPPORTS_GL )
  {
    iLog->Log("Error: Library <%s> isn't OpenGL library\n", m_LibName);
    goto exr;
  }
  CreateRContext(rc, Glhdc, hGLrc, cbpp, zbpp, sbits, true);

  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_MaxTextureSize);
  if (CV_gl_maxtexsize)
  {
    if (CV_gl_maxtexsize < 128)
      CV_gl_maxtexsize = 128;
    else
    if (CV_gl_maxtexsize > 2048)
      CV_gl_maxtexsize = 2048;
    m_MaxTextureSize = min(CV_gl_maxtexsize, m_MaxTextureSize);
  }

  iLog->Log("****** OpenGL Driver Stats ******\n");
  iLog->Log("Driver: %s\n", m_LibName);
  m_VendorName = glGetString(GL_VENDOR);
  iLog->Log("GL_VENDOR: %s\n", m_VendorName);
  m_RendererName = glGetString(GL_RENDERER);
  iLog->Log("GL_RENDERER: %s\n", m_RendererName);
  m_VersionName = glGetString(GL_VERSION);
  iLog->Log("GL_VERSION: %s\n", m_VersionName);
  m_ExtensionsName = glGetString(GL_EXTENSIONS);
  iLog->LogToFile("GL_EXTENSIONS:\n");
  // Modern extension strings can exceed a fixed buffer; copy what fits.
  char ext[8192];
  char *token;
  strncpy(ext, m_ExtensionsName ? (const char *)m_ExtensionsName : "", sizeof(ext) - 1);
  ext[sizeof(ext) - 1] = 0;
  token = strtok(ext, " ");
  while (token)
  {
    iLog->LogToFile("  %s\n", token);
    token = strtok(NULL, " ");
  }
  iLog->LogToFile("\n");

#ifndef GLES3_RENDERER
  GET_GL_PROC(PFNWGLGETEXTENSIONSSTRINGARBPROC,wglGetExtensionsStringARB);
  if(wglGetExtensionsStringARB)
  {
    const char * wglExt = wglGetExtensionsStringARB(m_CurrContext->m_hDC);
    if (wglExt)
    {
      iLog->LogToFile("WGL_EXTENSIONS:\n");

      strcpy(ext, (char *)wglExt);
      token = strtok(ext, " ");
      while (token)
      {
        iLog->LogToFile("  %s\n", token);
        token = strtok(NULL, " ");
      }
      iLog->LogToFile("\n");
    }
  }

#endif
  CheckOGLExtensions();
  CheckGammaSupport();
  // Port: the GPU class. The engine uses it to pick shader-script branches
  // (NV1X = fixed function, NV3X = ARB vertex/fragment programs) and, in a few
  // places, NVIDIA hardware workarounds. It is chosen here from what this
  // renderer runs, not from the extension strings the GL layer emulates:
  //   classic renderer:                  GF2  (fixed-function branches)
  //   native renderer with programs:     GFFX (ARB program branches, VS/PS 1.1)
  // FARCRY_GLES3_GPU=gf2|gffx forces one (diagnostic).
  {
    const char* gpu = getenv("FARCRY_GLES3_GPU");
    const bool fx = gpu ? !stricmp(gpu, "gffx") : GLR::ProgramsSupported();
    m_Features &= ~RFT_HW_MASK;
    m_Features |= fx ? RFT_HW_GFFX : RFT_HW_GF2;
  }
  fprintf(stderr, "PORT GPU class: %s\n", (m_Features & RFT_HW_MASK) == RFT_HW_GFFX ? "GeForce FX (ARB programs)" : "GeForce2 (fixed function)");
  ChangeLog();

  if (m_Features & RFT_HW_PS20)
  {
    if (CV_r_shadowtype == 0)
      m_Features |= RFT_SHADOWMAP_SELFSHADOW;
  }
  ICVar *var;
  int nGPU = m_Features & RFT_HW_MASK;

  var = iConsole->GetCVar("r_NoPS30");
  if (var)
    var->Set(1);
  m_NoPS30 = 1;
  var = iConsole->GetCVar("r_HDRRenderingForce");
  if (var)
    var->Set(0);
  var = iConsole->GetCVar("r_HDRRendering");
  if (var)
    var->Set(0);

  // Disable per-pixel lighting if pixel-shaders aren't supported
  if (!(m_Features & (RFT_HW_PS20 | RFT_HW_TS | RFT_HW_RC)))
  {
    if (CV_r_Quality_BumpMapping)
    {
      var = iConsole->GetCVar("r_Quality_BumpMapping");
      if (var)
        var->Set(0);
    }
    if (CV_r_checkSunVis >= 2)
    {
      var = iConsole->GetCVar("r_checkSunVis");
      if (var)
        var->Set(1);
    }
  }

  // Allow pixel shaders 2.0 lighting on Radeon only cards with PS.2.0 support
  if (CV_r_Quality_BumpMapping == 3)
  {
    if (!(m_Features & RFT_HW_PS20) || (nGPU == RFT_HW_GFFX && !CV_gl_nv30_ps20))
    {
      var = iConsole->GetCVar("r_Quality_BumpMapping");
      if (var)
        var->Set(2);
    }
  }

  // Shaders remapping for non-PS20 hardware
  if (!(m_Features & RFT_HW_PS20) || (nGPU == RFT_HW_GFFX && !CV_gl_nv30_ps20))
  {
    var = iConsole->GetCVar("r_NoPS20");
    if (var)
      var->Set(1);
  }

  // Disable trees per-pixel lighting from medium and low spec settings
  if (CV_r_Quality_BumpMapping < 2)
  {
    var = iConsole->GetCVar("r_Vegetation_PerpixelLight");
    if (var)
      var->Set(0);
  }

  // Allow offset bump-mapping and parametric shaders system for very high spec settings only
  if (CV_r_Quality_BumpMapping < 3)
  {
    var = iConsole->GetCVar("r_UseHWShaders");
    if (var && var->GetIVal() == 2)
      var->Set(1);
    if (CV_r_offsetbump)
    {
      var = iConsole->GetCVar("r_OffsetBump");
      if (var)
        var->Set(0);
    }
  }

  iLog->Log(" ****** OGL CryRenderer Stats ******\n");
  iLog->Log(" Mode: %d x %d (%s)\n", m_width, m_height, fullscreen ? "FullScreen" : "Windowed");
  iLog->Log(" Gamma: %s\n", (m_Features & RFT_HWGAMMA) ? "Hardware" : "Software");
  if (CV_r_fsaa && SUPPORTS_GL_ARB_multisample && (m_Features & RFT_SUPPORTFSAA))
    iLog->Log(" Full scene AA: Enabled (%d samples)\n", CV_r_fsaa_samples);
  else
    iLog->Log(" Full scene AA: Disabled\n");
  iLog->Log(" Stencil type: %s\n", (SUPPORTS_GL_ATI_separate_stencil || SUPPORTS_GL_EXT_stencil_two_side) ? "Two sided" : "Single sided");
  iLog->Log(" Max Texture size: %d\n", m_MaxTextureSize);
  iLog->Log(" Max Active Textures ARB: %d\n", m_MaxActiveTexturesARBFixed);
  iLog->Log(" Max Active Textures ARB (For ARB_fragment_program): %d\n", m_MaxActiveTexturesARB_VP);
  iLog->Log(" Projective EMBM: %s\n", (m_Features & RFT_HW_ENVBUMPPROJECTED) ? "enabled" : "disabled");
  iLog->Log(" Multitexture: %s\n", (m_Features & RFT_MULTITEXTURE) ? "enabled" : "disabled");
  iLog->Log(" Compressed textures: %s\n", (m_Features & RFT_COMPRESSTEXTURE) ? "enabled" : "disabled");
  iLog->Log(" Vertex buffers usage: %s\n", SUPPORTS_GL_ARB_vertex_buffer_object ? "VBO" : SUPPORTS_GL_NV_vertex_array_range ? "VAR" : "System memory");
  iLog->Log(" Anisotropic texture filtering: %s (Maximum level: %d)\n", (m_Features & RFT_ALLOWANISOTROPIC) ? "enabled" : "disabled", m_MaxAnisotropicLevel);
  if (m_Features & RFT_BUMP)
    iLog->Log(" Bump mapping: enabled (%s)\n", (m_Features & RFT_BUMP) ? "DOT3" : "EMBM");
  else
    iLog->Log(" Bump mapping: disabled\n");
  iLog->Log(" Detail microtextures: %s\n", (m_Features & RFT_DETAILTEXTURE) ? "enabled" : "disabled");
  iLog->Log(" Paletted textures: %s\n", (m_Features & RFT_PALTEXTURE) ? "enabled" : "disabled");

  if (nGPU == RFT_HW_NV4X)
    iLog->Log(" Use Hardware Shaders for NV4x GPUs\n");
  else
  if (nGPU == RFT_HW_GFFX)
    iLog->Log(" Use Hardware Shaders for NV3x GPUs\n");
  else
  if (nGPU == RFT_HW_GF2)
    iLog->Log(" Use Hardware Shaders for NV1x GPU\n");
  else
  if (nGPU == RFT_HW_GF3)
    iLog->Log(" Use Hardware Shaders for NV2x GPU\n");
  else
  if (nGPU == RFT_HW_RADEON)
    iLog->Log(" Use Hardware Shaders for ATI R300 GPU\n");
  else
    iLog->Log(" Hardware Shaders are not supported\n");

  char *str;
  if (nGPU == RFT_HW_GF2)
    str = "Not using pixel shaders";
  else
  if (CV_r_nops20)
    str = "Replace PS.2.0 to PS.1.1";
  else
  if (CV_r_Quality_BumpMapping == 3)
  {
    if (m_Features & RFT_HW_PS30)
      str = "PS.3.0, PS.2.0 and PS.1.1";
    else
      str = "PS.2.0 and PS.1.1";
  }
  else
    str ="PS1.1 only";
  iLog->Log(" Pixel shaders usage: %s\n", str);

  if (nGPU == RFT_HW_GF2)
    str = "Not using vertex shaders";
  else
  if (CV_r_nops20)
    str = "Replace VS.2.0 to VS.1.1";
  else
  if (CV_r_Quality_BumpMapping == 3)
  {
    if (m_Features & RFT_HW_PS30)
      str = "VS.3.0, VS.2.0 and VS.1.1";
    else
      str = "VS.2.0 and VS.1.1";
  }
  else
    str ="VS1.1 only";
  iLog->Log(" Vertex shaders usage: %s\n", str);

  iLog->Log(" Shadow maps type: %s\n", (m_Features & RFT_DEPTHMAPS) ? "Depth maps" : (m_Features & RFT_SHADOWMAP_SELFSHADOW) ? "Mixed Depth/2D maps" : "2D shadow maps");

  m_numtmus = m_MaxActiveTexturesARB_VP;

  glLoadIdentity();
  glRotatef(60, 0, 1, 0);
  Matrix44 m;
  glGetFloatv(GL_MODELVIEW_MATRIX, m.GetData());

  int parms[4];

  // User clip planes (water reflections cut at the water level). Port: the
  // native backend clips plane 0 in its programs (GLR_ClipPlane). The classic
  // layer reports some and ignores them (unchanged). Never 0: the engine's
  // fallback needs NV texture shaders, which nothing here has.
  if (GLR::Active()) m_MaxClipPlanes = 1;
  else glGetIntegerv(GL_MAX_CLIP_PLANES, &m_MaxClipPlanes);
  iLog->Log(" OGL Max Clip Planes=%d", m_MaxClipPlanes);
  glGetIntegerv(GL_MAX_LIGHTS, &m_MaxLightSources);
  iLog->Log(" OGL Max Lights=%d", m_MaxLightSources);
  glGetIntegerv(GL_MAX_TEXTURE_SIZE,parms);
  iLog->Log(" OGL Max Texture size=%dx%d",parms[0],parms[0]);
  glGetIntegerv(GL_MAX_VIEWPORT_DIMS,parms);
  iLog->Log(" OGL Max Viewport dims=%dx%d",parms[0],parms[1]);
  int nDepth;
  glGetIntegerv(GL_MAX_MODELVIEW_STACK_DEPTH, &nDepth);
  iLog->Log(" OGL Max ModelView Matrix stack depth=%d", nDepth);
  glGetIntegerv(GL_MAX_PROJECTION_STACK_DEPTH, &nDepth);
  iLog->Log(" OGL Max Projection Matrix stack depth=%d", nDepth);

  CheckError("CGLRenderer::Init");

  {
    static bool bSet = false;
    if (!bSet)
    {
      bSet = true;
      nnglTexImage2D = glTexImage2D;
      glTexImage2D = nglTexImage2D;

      nnglBindTexture = glBindTexture;
      glBindTexture = nglBindTexture;

      nnglCompressedTexImage2DARB = glCompressedTexImage2DARB;
      glCompressedTexImage2DARB = nglCompressedTexImage2DARB;

      nnglCopyTexImage2D = glCopyTexImage2D;
      glCopyTexImage2D = nglCopyTexImage2D;

      nnglDeleteTextures = glDeleteTextures;
      glDeleteTextures = nglDeleteTextures;

      nnglTexParameteri = glTexParameteri;
      glTexParameteri = nglTexParameteri;

      nnglBindBufferARB = glBindBufferARB;
      glBindBufferARB = nglBindBufferARB;

      nnglGenBuffersARB = glGenBuffersARB;
      glGenBuffersARB = nglGenBuffersARB;

      nnglDeleteBuffersARB = glDeleteBuffersARB;
      glDeleteBuffersARB = nglDeleteBuffersARB;
    }
  }

  GLSetDefaultState();
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  SetPolygonMode(R_SOLID_MODE);

#ifndef GLES3_RENDERER
  ::ShowWindow(m_CurrContext->m_Glhwnd,1);
  ::UpdateWindow(m_CurrContext->m_Glhwnd);
  ::SetForegroundWindow(m_CurrContext->m_Glhwnd);
  ::SetFocus(m_CurrContext->m_Glhwnd);
#endif

  SetGamma(CV_r_gamma+m_fDeltaGamma, CV_r_brightness, CV_r_contrast);

  m_width = width;
  m_height = height;

  InitVAR();

  if (glVertexArrayRangeNV && IsVarPresent())
    m_RP.m_NumFences = MAX_DYNVBS;

  if (bReInit)
  {
    iLog->Log("Reload textures\n");
    RefreshResources(0);
  }

  iLog->Log("Init Shaders\n");

  gRenDev->m_cEF.mfInit();
  EF_PipelineInit();

  return (m_CurrContext->m_Glhwnd);
}

bool CGLRenderer::CreateRContext(SRendContext *rc, WIN_HDC Glhdc, WIN_HGLRC hGLrc, int cbpp, int zbpp, int sbits, bool bAllowFSAA)
{
#ifdef GLES3_RENDERER
  // The window and ES context were created together in SetMode.
  m_CurrContext = rc;
  m_Features &= ~RFT_SUPPORTFSAA;
  CV_r_fsaa = 0;
  rc->m_bFSAAWasSet = false;
  rc->m_PixFormat = 0;
  rc->m_hRC = (HGLRC)g_GLES3Context;
  return g_GLES3Context != NULL && SDL_GL_MakeCurrent(g_GLES3Window, g_GLES3Context) == 0;
#else
  BOOL statusFSAA = false;
  int pixelFormat;

  m_CurrContext = rc;

  int nFSAA = bAllowFSAA ? CV_r_fsaa : 0;
  m_Features &= ~RFT_SUPPORTFSAA;

  if (bAllowFSAA && m_RContexts.Num() && rc == m_RContexts[0])
  {
    WNDCLASS wc;
    wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = ::DefWindowProc;
    wc.cbClsExtra    = 4;
    wc.cbWndExtra    = 4;
    wc.hInstance     = m_hInst;
    wc.hIcon         = 0;
    wc.hCursor       = NULL;
    wc.hbrBackground = (HBRUSH)(BLACK_BRUSH);
    wc.lpszMenuName  = NULL;
    wc.lpszClassName = "FAKE_WINDOW";

    RegisterClass(&wc);

    HWND hWnd = ::CreateWindow( "FAKE_WINDOW",		// window class name
      "Fake",		            // window caption
      WS_OVERLAPPEDWINDOW,	// window style
      CW_USEDEFAULT,		// initial x pos
      CW_USEDEFAULT,		// initial y pos
      CW_USEDEFAULT,		// initial x size
      CW_USEDEFAULT,		// initial x size
      NULL,			// parent window handle
      NULL,			// window menu handle
      m_hInst,			// program instance handle
      NULL );			// creation params

    if (hWnd)
    {
      PIXELFORMATDESCRIPTOR pfd;
      memset(&pfd, 0, sizeof(pfd));

      pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
      pfd.nVersion = 1;
      pfd.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER;
      pfd.dwLayerMask = PFD_MAIN_PLANE;
      pfd.iPixelType = PFD_TYPE_RGBA;
      pfd.cColorBits = cbpp;
      pfd.cAlphaBits = CV_gl_alpha_bits;
      pfd.cDepthBits = zbpp;
      pfd.cAccumBits = 0;
      pfd.cStencilBits = sbits;

      BOOL status = FALSE;

      HDC hDC = GetDC(hWnd);

      pixelFormat = ChoosePixelFormat(m_CurrContext->m_hDC, &pfd);

      if (SetPixelFormat(hDC, pixelFormat, &pfd) != FALSE)
      {
        // Create a rendering context
        HGLRC hGLRC = pwglCreateContext(hDC);

        // Make the rendering context current
        pwglMakeCurrent(hDC, hGLRC);

        // Search for best matching pixel format
        pwglGetProcAddress = (PFNWGLGETPROCADDRESSPROC)GetProcAddress( (HINSTANCE)m_hLibHandle, "wglGetProcAddress" );
        if (_wglExtensionSupported("WGL_ARB_multisample"))
        {
          wglGetPixelFormatAttribivARB = (PFNWGLGETPIXELFORMATATTRIBIVARBPROC)pwglGetProcAddress("wglGetPixelFormatAttribivARB");
          wglGetPixelFormatAttribfvARB = (PFNWGLGETPIXELFORMATATTRIBFVARBPROC)pwglGetProcAddress("wglGetPixelFormatAttribfvARB");
          wglChoosePixelFormatARB = (PFNWGLCHOOSEPIXELFORMATARBPROC)pwglGetProcAddress("wglChoosePixelFormatARB");

          if (wglChoosePixelFormatARB)
            m_Features |= RFT_SUPPORTFSAA;
        }
        // make no rendering context current
        wglMakeCurrent(NULL, NULL);

        // Destroy the rendering context...
        wglDeleteContext(hGLRC);
      }

      ReleaseDC(hWnd, hDC);
      DestroyWindow(hWnd);
      UnregisterClass("FAKE_WINDOW", m_hInst);
    }
  }

  if (nFSAA && (m_Features & RFT_SUPPORTFSAA))
  {
    // Search for best matching pixel format
    iLog->Log("Tryng to set FSAA pixel format...");
    int   iAttributes[30];
    float fAttributes[] = {0, 0};
    uint  numFormats;

    // Choose a Pixel Format Descriptor (PFD) with multisampling support.
    iAttributes[0] = WGL_DOUBLE_BUFFER_ARB;
    iAttributes[1] = TRUE;
    iAttributes[2] = WGL_COLOR_BITS_ARB;
    iAttributes[3] = cbpp;
    iAttributes[4] = WGL_DEPTH_BITS_ARB;
    iAttributes[5] = zbpp;
    iAttributes[6] = WGL_ALPHA_BITS_ARB;
    iAttributes[7] = CV_gl_alpha_bits;
    iAttributes[8] = WGL_STENCIL_BITS_ARB;
    iAttributes[9] = sbits;
    iAttributes[10] = WGL_SAMPLE_BUFFERS_ARB;
    iAttributes[11] = TRUE;
    iAttributes[12] = WGL_SAMPLES_ARB;
    iAttributes[13] = CV_r_fsaa_samples;
    iAttributes[14] = 0;
    iAttributes[15] = 0;

    // First attempt...
    statusFSAA = wglChoosePixelFormatARB(m_CurrContext->m_hDC, iAttributes, fAttributes, 1, &pixelFormat, &numFormats);
    // Failure happens not only when the function fails, but also when no matching pixel format has been found
    if (statusFSAA == FALSE || numFormats == 0)
    {
      iAttributes[6] = 0;
      iAttributes[7] = 0;
      statusFSAA = wglChoosePixelFormatARB(m_CurrContext->m_hDC, iAttributes, fAttributes, 1, &pixelFormat, &numFormats);
    }
    if (statusFSAA)
    {
      rc->m_bFSAAWasSet = true;
      iLog->Log("Ok");
    }
    else
      iLog->Log("False");
  }
  if (m_RContexts.Num() && rc != m_RContexts[0])
    statusFSAA = m_RContexts[0]->m_bFSAAWasSet;

  if (!statusFSAA)
    CV_r_fsaa = 0;

  if (statusFSAA)
  {
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(pfd));
    DescribePixelFormat(m_CurrContext->m_hDC, pixelFormat, sizeof(PIXELFORMATDESCRIPTOR), &pfd);
    SetPixelFormat(m_CurrContext->m_hDC, pixelFormat, &pfd);
    rc->m_PixFormat = pixelFormat;

    m_CurrContext->m_hRC = pwglCreateContext(m_CurrContext->m_hDC);
    m_CurrContext->m_bFSAAWasSet = true;

    pwglMakeCurrent(m_CurrContext->m_hDC, m_CurrContext->m_hRC);

    glEnable(GL_MULTISAMPLE_ARB);
  }
  else
  {
    m_CurrContext->m_bFSAAWasSet = false;
    if (!Glhdc && !SetupPixelFormat(cbpp,zbpp,sbits,rc))
    {
      iLog->Log("Warning: SetupPixelFormat failed\n");
		  //try to change alpha bits
		  if (CV_gl_alpha_bits>0)
		  {
			  iLog->Log("Trying to change alpha bits");
			  CV_gl_alpha_bits = 0;
			  //and call pixel format again
			  if (!SetupPixelFormat(cbpp,zbpp,sbits,rc))
				  return (false);
		  }
    }

    if (!hGLrc)
      rc->m_hRC = pwglCreateContext(rc->m_hDC);
    else
    {
      rc->m_hRC = (HGLRC)hGLrc;
      m_bEditor = true;
    }
  }
  if (!rc->m_hRC)
  {
    iLog->Log("Warning: wglCreateContext failed\n");
    return (false);
  }

  pwglMakeCurrent(rc->m_hDC, rc->m_hRC);
  if (m_RContexts[0] != rc)
  {
    pwglShareLists(m_RContexts[0]->m_hRC, rc->m_hRC);
    CV_r_vsync = 1;
  }

  return true;
#endif
}

bool CGLRenderer::SetCurrentContext(WIN_HWND hWnd)
{
  int i;

  for (i=0; i<m_RContexts.Num(); i++)
  {
    if (m_RContexts[i]->m_Glhwnd == hWnd)
      break;
  }
  if (i == m_RContexts.Num())
    return false;

  if (m_CurrContext == m_RContexts[i])
    return true;

  m_CurrContext = m_RContexts[i];
  if (pwglGetCurrentContext() != m_RContexts[i]->m_hRC)
  {
    pwglMakeCurrent(m_RContexts[i]->m_hDC, m_RContexts[i]->m_hRC);

    for (i=0; i<96; i++)
    {
      CCGVProgram_GL::m_CurParams[i][0] = -999999;
      CCGVProgram_GL::m_CurParams[i][1] = -999999;
      CCGVProgram_GL::m_CurParams[i][2] = -999999;
      CCGVProgram_GL::m_CurParams[i][3] = -999999;
    }
    CVProgram::m_LastVP = NULL;
    CPShader::m_CurRC = NULL;
  }

  return true;
}

bool CGLRenderer::CreateContext(WIN_HWND hWnd, bool bAllowFSAA)
{
  int i;

  for (i=0; i<m_RContexts.Num(); i++)
  {
    if (m_RContexts[i]->m_Glhwnd == hWnd)
      break;
  }
  if (i != m_RContexts.Num())
    return false;
  SRendContext *rc = new SRendContext;
  m_RContexts.AddElem(rc);
  rc->m_Glhwnd = (HWND)hWnd;
  rc->m_hDC = GetDC((HWND)hWnd);
  rc->m_hRC = NULL;
  int cbpp = m_cbpp;
  int zbpp = m_zbpp;
  int sbpp = m_sbpp;
  bool bRes = CreateRContext(rc, NULL, NULL, cbpp, zbpp, sbpp, bAllowFSAA);
  if (bRes)
  {
    for (i=0; i<96; i++)
    {
      CCGVProgram_GL::m_CurParams[i][0] = -999999;
      CCGVProgram_GL::m_CurParams[i][1] = -999999;
      CCGVProgram_GL::m_CurParams[i][2] = -999999;
      CCGVProgram_GL::m_CurParams[i][3] = -999999;
    }
    CVProgram::m_LastVP = NULL;
    CPShader::m_CurRC = NULL;
  }
  return bRes;
}

bool CGLRenderer::DeleteContext(WIN_HWND hWnd)
{
  int i;

  for (i=0; i<m_RContexts.Num(); i++)
  {
    if (m_RContexts[i]->m_Glhwnd == hWnd)
      break;
  }
  if (i == m_RContexts.Num())
    return false;
  SRendContext *rc = m_RContexts[i];
  pwglMakeCurrent(NULL, NULL);

  if (rc->m_hRC)
  {
    pwglDeleteContext(rc->m_hRC);
    rc->m_hRC = NULL;
  }

  if (rc->m_hDC)
  {
    ReleaseDC(rc->m_Glhwnd, rc->m_hDC);
    rc->m_hDC = NULL;
  }
  delete rc;
  m_RContexts.Remove(i, 1);

  return true;
}


#else //PS2

bool CGLRenderer::Init(int x,int y,int width,int height,unsigned int cbpp, int zbpp, int sbits, bool fullscreen)
{
  return false;
}

#endif  //endif

void CGLRenderer::RefreshResources(int nFlags)
{
  if (nFlags & FRO_TEXTURES)
    m_TexMan->ReloadAll(nFlags);
  if (nFlags & (FRO_SHADERS | FRO_SHADERTEXTURES))
    gRenDev->m_cEF.mfReloadAllShaders(nFlags);
}


extern float gFOpenTime;
extern int nRejectFOpen;
extern int nAcceptFOpen;

void CGLRenderer::ShutDown(bool bReInit)
{
  int i;

  //  CRendElement::mfLogAllREs();

  if (CV_r_printmemoryleaks)
  {
    FILE *fp = fxopen("LeafBufferLeaks.txt", "w");
    if (fp)
    {
      CLeafBuffer *pLB = CLeafBuffer::m_RootGlobal.m_NextGlobal;
      while (pLB != &CLeafBuffer::m_RootGlobal)
      {
        CLeafBuffer *Next = pLB->m_NextGlobal;
        float fSize = pLB->Size(0)/1024.0f/1024.0f;
        fprintf(fp, "*** LB %s: %0.3fMb\n", pLB->m_sSource, fSize);
        iLog->Log("WARNING: LB Leak %s: %0.3fMb", pLB->m_sSource, fSize);
        DeleteLeafBuffer(pLB);
        pLB = Next;
      }

      CRendElement *pRE = CRendElement::m_RootGlobal.m_NextGlobal;
      while (pRE != &CRendElement::m_RootGlobal)
      {
        CRendElement *Next = pRE->m_NextGlobal;
        float fSize = pRE->Size()/1024.0f/1024.0f;
        fprintf(fp, "*** LB %s: %0.3fMb\n", pRE->mfTypeString(), fSize);
        iLog->Log("WARNING: RE Leak %s: %0.3fMb", pRE->mfTypeString(), fSize);
        SAFE_RELEASE(pRE);
        pRE = Next;
      }

      fclose(fp);
    }
  }

  FreeResources(FRR_ALL);
  EF_PipelineShutdown();
  GenerateVBLog("VBLeaks.txt");
  ShutDownVAR();
  CName::mfExitSubsystem();

  if (m_CGContext)
  {
#ifndef WIN64
		// TODO: AMD64 port: find 64-bit CG
    cgDestroyContext(m_CGContext);
#endif
    m_CGContext = NULL;
  }


  iLog->LogToFile("Tex. FOpen time: %f sec\n", gFOpenTime);
  iLog->LogToFile("Tex. Accept FOpen: %d\n", nAcceptFOpen);
  iLog->LogToFile("Tex. Reject FOpen: %d\n", nRejectFOpen);

  iLog->LogToFile("GlRenderer Shutdown\n");

  RestoreDeviceGamma();

  GLR::StopThread();   // GL back on this thread for the teardown
  glFinish();

  HWND hWnd = m_RContexts[0]->m_Glhwnd;

  for (i=0; i<m_RContexts.Num(); i++)
  {
    DeleteContext(m_RContexts[i]->m_Glhwnd);
  }
#ifdef GLES3_RENDERER
  GLES3_ReportMissingCalls();
  if (!bReInit)
  {
    GLES3FFPShutdown();
    if (g_GLES3Context) { SDL_GL_MakeCurrent(g_GLES3Window, NULL); SDL_GL_DeleteContext(g_GLES3Context); g_GLES3Context = NULL; }
    if (g_GLES3Window) { SDL_DestroyWindow(g_GLES3Window); g_GLES3Window = NULL; }
  }
#else
  if (hWnd && !bReInit)
  {
    DestroyWindow(hWnd);
  }
#endif

  //ChangeDisplay(m_deskwidth,m_deskheight,m_deskbpp);
  ChangeDisplay(0,0,0);

  FreeLibrary();

  if (m_vidmodes)
  {
    delete [] m_vidmodes;
    m_vidmodes=NULL;
  }
}


//=======================================================================

ILog     *iLog;
IConsole *iConsole;
ITimer   *iTimer;
ISystem  *iSystem;
//CVars    *cVars;
int *pTest_int;
//CryCharManager *pCharMan;
IPhysicalWorld *pIPhysicalWorld;

__attribute__((weak)) ISystem *GetISystem()
{
	return iSystem;
}

extern "C" DLL_EXPORT IRenderer* PackageRenderConstructor(int argc, char* argv[], SCryRenderInterface *sp);
DLL_EXPORT IRenderer* PackageRenderConstructor(int argc, char* argv[], SCryRenderInterface *sp)
{
  gbRgb = false;

  iConsole  = sp->ipConsole;
  iLog      = sp->ipLog;
  iTimer    = sp->ipTimer;
  iSystem   = sp->ipSystem;
//  cVars     = sp->ipVars;
  pTest_int = sp->ipTest_int;
	pIPhysicalWorld = sp->pIPhysicalWorld;
//  pCharMan = sp->ipCharMan;

#ifdef DEBUGALLOC
#undef new
#endif
  IRenderer *rd = (IRenderer *) (new CGLRenderer());
#ifdef DEBUGALLOC
#define new DEBUG_CLIENTBLOCK
#endif

  g_SecondsPerCycle = iSystem->GetSecondsPerCycle();
  g_CpuFlags = iSystem->GetCPUFlags();

  srand( GetTickCount() );

  return rd;
}

void *gGet_D3DDevice()
{
  return NULL;
}
void *gGet_glReadPixels()
{
  return glReadPixels;
}

