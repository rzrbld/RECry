/*=============================================================================
  GLRPlatform.cpp : see GLRPlatform.h.
=============================================================================*/
#include "GLRPlatform.h"
#include "GLR.h"
#include "GLTexUpload.h"
#include "GLES3TexConvert.h"
#include <GLES3/gl3.h>
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
void (*s_log)(const char*) = nullptr;
void Log(const char* fmt, ...)
{
  char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
  if (s_log) s_log(b); else fprintf(stderr, "%s\n", b);
}
Uint32 s_loadEndTicks = 0;
// Uploads of finished conversions run in draw order on the render thread.
void Pump(bool block) { GLR::Record([block] { GLTex::Pump(block); }); }
}

bool GLRPlatformInit(void (*log)(const char*))
{
  s_log = log;
  typedef const GLubyte* (*PGetString)(GLenum);
  PGetString getString = (PGetString)SDL_GL_GetProcAddress("glGetString");
  const char* ext = getString ? (const char*)getString(GL_EXTENSIONS) : nullptr;
  const bool s3tc = ext && (strstr(ext, "GL_EXT_texture_compression_s3tc") || strstr(ext, "GL_EXT_texture_compression_dxt1"));
  const bool decodeDXT = !s3tc || getenv("FARCRY_GLES3_FORCE_DXT_DECODE");
  const bool dxtRGBA8 = getenv("FARCRY_GLES3_DXT_DECODE_RGBA8") != nullptr;
  double uploadBudgetMs = 2.0;
  const char* conv = getenv("FARCRY_GLES3_TEXCONV");
  if (decodeDXT && (!conv || atoi(conv) != 0))
  {
    const char* th = getenv("FARCRY_TEXCONV_THREADS");
    if (const char* b = getenv("FARCRY_TEXCONV_UPLOAD_MS")) uploadBudgetMs = atof(b);
    TexConv_Start(th ? atoi(th) : 1, getenv("FARCRY_TEXCACHE"), s_log);
  }
  GLTex::Backend be;   // GLR::Init installs the backend's binding callbacks
  be.log = [](const char* m) { Log("%s", m); };
  const bool ok = GLTex::Init(be, decodeDXT, dxtRGBA8, uploadBudgetMs);
  Log("GLR: S3TC/DXT %s by the driver%s", s3tc ? "supported" : "NOT supported",
      decodeDXT ? "; DXT textures are converted (ETC2 in background, 16-bit fallback)" : "");
  return ok;
}

void GLRPumpTextures()
{
  // Shortly after a level load the engine still precaches resources; finish
  // their conversions before drawing instead of showing placeholders.
  const bool afterLoad = s_loadEndTicks && SDL_GetTicks() - s_loadEndTicks < 10000;
  if (TexConv_Running()) Pump(afterLoad && TexConv_Pending() > 0);
}

void GLRFinishTextures()
{
  if (TexConv_Running()) Pump(true);
  s_loadEndTicks = SDL_GetTicks();
  if (!s_loadEndTicks) s_loadEndTicks = 1;
}

void GLRPlatformShutdown() { TexConv_Stop(); }
