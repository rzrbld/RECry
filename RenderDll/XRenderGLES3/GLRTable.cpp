/*=============================================================================
  GLRTable.cpp : the engine's GL function table on the native backend
  (see GLRTable.h).
=============================================================================*/
#include "GLRTable.h"
#include "GLR.h"
#include <GLES3/gl3.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_set>

namespace
{
// Entry points the old fixed-function layer provided (resolution keeps the
// engine's extension support flags as they were).
const char* const kKnown[] = { "glActiveTexture","glActiveTextureARB","glAlphaFunc","glBegin","glBindBuffer","glBindBufferARB","glBindTexture","glBufferData","glBufferDataARB","glBufferSubData","glBufferSubDataARB","glClearDepth","glClientActiveTexture","glClientActiveTextureARB","glClipPlane","glColor3f","glColor3fv","glColor3ub","glColor3ubv","glColor4f","glColor4fv","glColor4ub","glColor4ubv","glColorMaterial","glColorPointer","glCompressedTexImage2D","glCompressedTexImage2DARB","glCompressedTexSubImage2D","glCompressedTexSubImage2DARB","glCopyTexSubImage2D","glDeleteBuffers","glDeleteBuffersARB","glDeleteTextures","glDepthFunc","glDepthRange","glDisable","glDisableClientState","glDisableVertexAttribArray","glDisableVertexAttribArrayARB","glDrawArrays","glDrawBuffer","glDrawElements","glDrawRangeElements","glDrawRangeElementsEXT","glEnable","glEnableClientState","glEnableVertexAttribArray","glEnableVertexAttribArrayARB","glEnd","glFogf","glFogfv","glFogi","glFogiv","glFrustum","glGenTextures","glGetDoublev","glGetFloatv","glGetIntegerv","glHint","glIsEnabled","glLightf","glLightfv","glLightModelfv","glLightModeli","glLoadIdentity","glLoadMatrixd","glLoadMatrixf","glMapBuffer","glMapBufferARB","glMaterialf","glMaterialfv","glMatrixMode","glMultiTexCoord2fARB","glMultiTexCoord2fvARB","glMultiTexCoord3fARB","glMultiTexCoord3fvARB","glMultiTexCoord4fARB","glMultiTexCoord4fvARB","glMultMatrixd","glMultMatrixf","glNormal3f","glNormal3fv","glNormalPointer","glOrtho","glPointSize","glPolygonMode","glPopMatrix","glPushMatrix","glReadPixels","glRotated","glRotatef","glScaled","glScalef","glSecondaryColorPointerEXT","glShadeModel","glTexCoord1f","glTexCoord2f","glTexCoord2fv","glTexCoord3f","glTexCoord3fv","glTexCoord4f","glTexCoord4fv","glTexCoordPointer","glTexEnvf","glTexEnvfv","glTexEnvi","glTexEnviv","glTexGenf","glTexGenfv","glTexGeni","glTexImage2D","glTexParameterf","glTexParameterfv","glTexParameteri","glTexParameteriv","glTexSubImage2D","glTranslated","glTranslatef","glUnmapBuffer","glUnmapBufferARB","glUseProgram","glVertex2f","glVertex2fv","glVertex2i","glVertex3f","glVertex3fv","glVertex4f","glVertex4fv","glVertexAttribPointer","glVertexAttribPointerARB","glVertexPointer", nullptr };
char s_placeholder;

enum { kMaxIds = 2048 };
const char* s_names[kMaxIds];
unsigned s_stubCalls[kMaxIds];

void Report()
{
  bool any = false;
  for (int i = 0; i < kMaxIds; ++i)
    if (s_stubCalls[i]) { if (!any) fprintf(stderr, "GLRTable: engine GL calls that reached a no-op stub:\n"); any = true; fprintf(stderr, "GLRTable:   %s x%u\n", s_names[i] ? s_names[i] : "?", s_stubCalls[i]); }
  if (!any) fprintf(stderr, "GLRTable: no engine GL call reached a stub\n");
}

// Driver query, on the render thread when it owns the context.
template <typename F> void Driver(F f)
{
  if (GLR::Threaded() && !GLR::OnRenderThread()) GLR::Sync(f); else f();
}
void NativeGetIntegerv(GLenum pname, GLint* v)
{
  switch (pname)
  {
    // Desktop fixed-function limits the engine reads at start-up (the values
    // the fixed-function layer reported; ES has none of these).
    case 0x84E2: /* GL_MAX_TEXTURE_UNITS */ case 0x8871: /* GL_MAX_TEXTURE_COORDS */ *v = 4; return;
    case 0x0D32: /* GL_MAX_CLIP_PLANES */ *v = 1; return;   // the backend clips plane 0
    case 0x0D31: /* GL_MAX_LIGHTS */ *v = 8; return;
    case 0x0D36: /* GL_MAX_MODELVIEW_STACK_DEPTH */ *v = 32; return;
    case 0x0D38: /* GL_MAX_PROJECTION_STACK_DEPTH */ case 0x0D39: /* GL_MAX_TEXTURE_STACK_DEPTH */ *v = 4; return;
    case GL_VIEWPORT: { int t[4]; GLR::GetViewport(t); for (int i = 0; i < 4; ++i) v[i] = t[i]; return; }
    case 0x0B60: /* GL_FOG */ *v = GLR::FogEnabled() ? 1 : 0; return;
  }
  static void (*real)(GLenum, GLint*) = (void (*)(GLenum, GLint*))SDL_GL_GetProcAddress("glGetIntegerv");
  Driver([&] { real(pname, v); });
}
void NativeGetFloatv(GLenum pname, GLfloat* v)
{
  switch (pname)
  {
    case 0x0BA6: /* GL_MODELVIEW_MATRIX */ memcpy(v, GLR::ModelView(), 64); return;
    case 0x0BA7: /* GL_PROJECTION_MATRIX */ memcpy(v, GLR::Projection(), 64); return;
    case 0x0BA8: /* GL_TEXTURE_MATRIX */ memcpy(v, GLR::TextureMatrix(GLR::SelectedTexture()), 64); return;
  }
  static void (*real)(GLenum, GLfloat*) = (void (*)(GLenum, GLfloat*))SDL_GL_GetProcAddress("glGetFloatv");
  Driver([&] { real(pname, v); });
}
void NativeGetDoublev(GLenum pname, double* v)
{
  float f[16] = {};
  NativeGetFloatv(pname, f);
  const int n = (pname >= 0x0BA6 && pname <= 0x0BA8) ? 16 : 4;
  for (int i = 0; i < n; ++i) v[i] = f[i];
}
GLboolean NativeIsEnabled(GLenum cap)
{
  if (cap == 0x0B60 /*GL_FOG*/) return GLR::FogEnabled() ? GL_TRUE : GL_FALSE;
  return GL_FALSE;   // desktop capabilities the engine asks about (NV extensions) are absent
}
GLenum NativeGetError() { return GL_NO_ERROR; }   // the backend checks its own calls
} // namespace

bool GLRTableKnown(const char* name)
{
  static std::unordered_set<std::string> s_set;
  if (s_set.empty()) for (int i = 0; kKnown[i]; ++i) s_set.insert(kKnown[i]);
  return s_set.count(name) != 0;
}
void* GLRTableKnownPlaceholder() { return &s_placeholder; }

void* GLRTableNative(const char* n)
{
  if (!strcmp(n, "glGetIntegerv")) return (void*)&NativeGetIntegerv;
  if (!strcmp(n, "glGetFloatv")) return (void*)&NativeGetFloatv;
  if (!strcmp(n, "glGetDoublev")) return (void*)&NativeGetDoublev;
  if (!strcmp(n, "glIsEnabled")) return (void*)&NativeIsEnabled;
  if (!strcmp(n, "glGetError")) return (void*)&NativeGetError;
  return nullptr;
}
bool GLRTableIsQuery(const char* n) { return !strncmp(n, "glGet", 5) || !strncmp(n, "glIs", 4); }

void GLRTableName(int id, const char* name)
{
  if (id < 0 || id >= kMaxIds) return;
  s_names[id] = name;
  static bool s_reg = false;
  if (!s_reg && getenv("FARCRY_GL_STUBS")) { s_reg = true; atexit(Report); }
}
void GLRTableStubbed(int id) { if (id >= 0 && id < kMaxIds) ++s_stubCalls[id]; }
void GLRTableRunOnRenderThread(void (*fn)(void*), void* arg) { Driver([fn, arg] { fn(arg); }); }
