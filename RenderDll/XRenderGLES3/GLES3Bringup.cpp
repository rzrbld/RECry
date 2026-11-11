/*=============================================================================
  GLES3Bringup.cpp : temporary replacements for desktop-only libraries the
  original OpenGL renderer linked against (Cg runtime, GLU, nvparse, NVIDIA
  DXT library). Each section is a bring-up stand-in to be replaced by native
  GLES3 code; none of them draws anything.
=============================================================================*/

#include "RenderPCH.h"
#include "GL_Renderer.h"
#include "CG/cgGL.h"
#include "../Common/Textures/dxtlib.h"
#include <sys/stat.h>
#include <math.h>

///////////////////////////////////////////////////////////////////////////////
// Cg runtime. There is no Cg compiler on the target; shader programs will be
// produced by the GLES3 renderer's own Cg -> GLSL ES translator. Until then,
// compile requests fail cleanly, and when FARCRY_GLES3_CG_DUMP names a
// directory, every requested program (profile, entry point, source) is
// written there as translator test input. Dumps are local game data: never
// package or redistribute them.
///////////////////////////////////////////////////////////////////////////////

#include <set>
#include <string>
static CGerror g_GLES3CgError = CG_NO_ERROR;
// In shader-tier bring-up mode, compiles "succeed" with a placeholder program
// so the engine goes on to request its full program set for capture.
static int g_GLES3CgProgramTag[2];
static std::set<std::string> g_GLES3CgSeen;
static int g_GLES3CgRequests = 0;
static int g_GLES3CgContextTag;

CGcontext cgCreateContext(void) { return (CGcontext)&g_GLES3CgContextTag; }
void cgDestroyContext(CGcontext) {}

CGprogram cgCreateProgram(CGcontext, CGenum, const char *program, CGprofile profile, const char *entry, const char **)
{
  ++g_GLES3CgRequests;
  const bool vertex = ((int)profile == 6150);
  const bool fresh = g_GLES3CgSeen.insert(std::string(entry ? entry : "") + "|" + (vertex ? "v" : "f") + "|" + (program ? program : "")).second;
  if (const char *dir = fresh ? getenv("FARCRY_GLES3_CG_DUMP") : NULL)
  {
    mkdir(dir, 0755);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%05d_p%d_%s.cg", dir, (int)g_GLES3CgSeen.size(), (int)profile, entry ? entry : "main");
    if (FILE *f = fopen(path, "w"))
    {
      fprintf(f, "// profile=%d entry=%s\n%s\n", (int)profile, entry ? entry : "main", program ? program : "");
      fclose(f);
    }
  }
  if (g_GLES3CgRequests <= 3 || (g_GLES3CgRequests % 100) == 0)
    iLog->Log("GLES3: Cg compile request #%d (profile %d, entry %s) - translator not implemented yet",
              g_GLES3CgRequests, (int)profile, entry ? entry : "main");
  static int s_shaderTier = getenv("FARCRY_GLES3_SHADER_TIER") ? atoi(getenv("FARCRY_GLES3_SHADER_TIER")) : 0;
  if (s_shaderTier)
    return (CGprogram)&g_GLES3CgProgramTag[vertex ? 0 : 1];
  g_GLES3CgError = CG_COMPILER_ERROR;
  return NULL;
}

void cgDestroyProgram(CGprogram) {}
const char *cgGetProgramString(CGprogram prog, CGenum)
{
  if (prog == (CGprogram)&g_GLES3CgProgramTag[0]) return "!!ARBvp1.0\nEND\n";
  if (prog == (CGprogram)&g_GLES3CgProgramTag[1]) return "!!ARBfp1.0\nEND\n";
  return "";
}
CGerror cgGetError(void) { CGerror e = g_GLES3CgError; g_GLES3CgError = CG_NO_ERROR; return e; }
const char *cgGetErrorString(CGerror e)
{
  return e == CG_NO_ERROR ? "no error" : "Cg compiler not available in the GLES3 renderer (GLSL translator pending)";
}

// Standard Cg profile numbers for arbvp1 / arbfp1 (the vendored header
// predates them). The GL renderer only passes these back to cgCreateProgram.
CGprofile cgGLGetLatestProfile(CGGLenum type) { return (CGprofile)(type == CG_GL_VERTEX ? 6150 : 7000); }
void cgGLSetOptimalOptions(CGprofile) {}

///////////////////////////////////////////////////////////////////////////////
// GLU. Matrix helpers are real (they feed the renderer's matrix calls);
// quadric drawing is debug-only and does nothing.
///////////////////////////////////////////////////////////////////////////////

void APIENTRY gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar)
{
  double f = 1.0 / tan(fovy * M_PI / 360.0);
  float m[16] = {0};
  m[0] = (float)(f / aspect);
  m[5] = (float)f;
  m[10] = (float)((zFar + zNear) / (zNear - zFar));
  m[11] = -1.0f;
  m[14] = (float)((2.0 * zFar * zNear) / (zNear - zFar));
  glMultMatrixf(m);
}

void APIENTRY gluLookAt(GLdouble ex, GLdouble ey, GLdouble ez, GLdouble cx, GLdouble cy, GLdouble cz,
                        GLdouble ux, GLdouble uy, GLdouble uz)
{
  double f[3] = { cx - ex, cy - ey, cz - ez };
  double fl = sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]); if (fl > 0) { f[0] /= fl; f[1] /= fl; f[2] /= fl; }
  double s[3] = { f[1]*uz - f[2]*uy, f[2]*ux - f[0]*uz, f[0]*uy - f[1]*ux };
  double sl = sqrt(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]); if (sl > 0) { s[0] /= sl; s[1] /= sl; s[2] /= sl; }
  double u[3] = { s[1]*f[2] - s[2]*f[1], s[2]*f[0] - s[0]*f[2], s[0]*f[1] - s[1]*f[0] };
  float m[16] = { (float)s[0], (float)u[0], (float)-f[0], 0,
                  (float)s[1], (float)u[1], (float)-f[1], 0,
                  (float)s[2], (float)u[2], (float)-f[2], 0,
                  0, 0, 0, 1 };
  glMultMatrixf(m);
  float t[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, (float)-ex, (float)-ey, (float)-ez, 1 };
  glMultMatrixf(t);
}

static int g_GLES3QuadricTag;
GLUquadric* APIENTRY gluNewQuadric(void) { return (GLUquadric*)&g_GLES3QuadricTag; }
void APIENTRY gluDeleteQuadric(GLUquadric*) {}
void APIENTRY gluSphere(GLUquadric*, GLdouble, GLint, GLint) {}

///////////////////////////////////////////////////////////////////////////////
// nvparse (NVIDIA register-combiner / texture-shader scripts). Those hardware
// paths are not available on GLES3, so parsing is a no-op.
///////////////////////////////////////////////////////////////////////////////

TArray<SCGBindConst> gParseConsts;
void nvparse(bool, const char *, ...) {}
char * const * const nvparse_get_errors()
{
  static char *s_none[1] = { NULL };
  return s_none;
}

///////////////////////////////////////////////////////////////////////////////
// NVIDIA DXT library: runtime (de)compression is unavailable; callers fall
// back. The GLES3 texture pipeline will provide its own conversion.
///////////////////////////////////////////////////////////////////////////////

HRESULT nvDXTcompress(unsigned char *, unsigned long, unsigned long, DWORD, CompressionOptions *, DWORD, MIPcallback, RECT *)
{
  return (HRESULT)0x80004005L; // E_FAIL
}

unsigned char *nvDXTdecompress(int &, int &, int &, int &, int &, int &, int)
{
  return NULL;
}
