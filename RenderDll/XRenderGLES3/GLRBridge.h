/*=============================================================================
  GLRBridge.h : engine-side calls into the native backend (GLR.h).

  The engine's renderer code calls these instead of the GL table: state,
  matrices, textures, buffers, arrays, draws and frame reads go straight to
  the native backend (GLR.h), which records them for the render thread.
  Included by GL_Renderer.h after the GL table declarations.
=============================================================================*/
#pragma once
#include "GLR.h"
#include "GLTexUpload.h"

namespace GLRBridgeDetail
{
// Desktop texture targets -> native stage targets.
inline GLR::TexTarget Target(GLenum t)
{
  if (t == GL_TEXTURE_CUBE_MAP_EXT || (t >= GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT && t <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_EXT)) return GLR::TEX_CUBE;
  return GLR::TEX_2D;   // 2D, rectangle (no 1D/3D textures on ES)
}
inline bool IsTextureCap(GLenum c)
{
  return c == GL_TEXTURE_2D || c == GL_TEXTURE_CUBE_MAP_EXT || c == GL_TEXTURE_RECTANGLE_NV || c == GL_TEXTURE_1D || c == GL_TEXTURE_3D_EXT;
}
}

// ---------------------------------------------------------------- texture units
inline void GLR_ActiveTexture(GLenum unit)
{
  GLR::SelectTexture((int)(unit - GL_TEXTURE0_ARB));
}
extern int TargetTex[];   // GLSystem.cpp: each texture's target, recorded at bind (used by video updates)
inline void GLR_BindTexture(GLenum target, GLuint name)
{
    // The engine's bind hook (GLSystem.cpp nglBindTexture) records the
    // texture's target; keep that bookkeeping on the native path too.
    if (name < TX_LASTBIND) TargetTex[name] = (int)target;
    GLTex::NoteBound(name);
    GLR::NoteTextureTarget(name, target);
    GLR::BindTexture(GLRBridgeDetail::Target(target), name);
}
// glEnable/glDisable of a texture target (other capabilities: GL table).
inline void GLR_EnableTexture(GLenum cap, bool on)
{
  GLR::EnableTexture(GLRBridgeDetail::Target(cap), on);
}

// ---------------------------------------------------------------- texture objects
inline void GLR_GenTextures(GLsizei n, GLuint* out)
{
  GLR::TexGenNames(n, out);
}
inline void GLR_DeleteTextures(GLsizei n, const GLuint* tex)
{
  GLR::TexDeleteNames(n, tex);
}
inline unsigned GLR_Bound(GLenum target) { return GLR::BoundTexture(GLRBridgeDetail::Target(target)); }
inline void GLR_TexParameteri(GLenum target, GLenum pname, GLint v)
{
  GLR::TexParam(GLR_Bound(target), target, pname, v);
}
inline void GLR_TexParameterf(GLenum target, GLenum pname, GLfloat v)
{
  GLR::TexParamF(GLR_Bound(target), target, pname, v);
}
inline void GLR_TexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const GLvoid* pixels)
{
  GLR::TexImage2D(GLR_Bound(target), target, level, internal, w, h, border, format, type, pixels);
}
inline void GLR_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid* pixels)
{
  GLR::TexSubImage2D(GLR_Bound(target), target, level, x, y, w, h, format, type, pixels);
}
inline void GLR_CompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border, GLsizei size, const GLvoid* data)
{
  GLR::TexCompressedImage2D(GLR_Bound(target), target, level, internal, w, h, border, size, data);
}
inline void GLR_CompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLsizei size, const GLvoid* data)
{
  GLR::TexCompressedSubImage2D(GLR_Bound(target), target, level, x, y, w, h, format, size, data);
}

// ---------------------------------------------------------------- matrices
inline void GLR_MatrixMode(GLenum m)
{
  GLR::MatrixModeSet(m == GL_PROJECTION ? GLR::MAT_PROJECTION : (m == GL_TEXTURE ? GLR::MAT_TEXTURE : GLR::MAT_MODELVIEW));
}
inline void GLR_LoadIdentity() { GLR::LoadIdentity(); }
inline void GLR_LoadMatrixf(const GLfloat* m) { GLR::LoadMatrix(m); }
inline void GLR_LoadMatrixd(const GLdouble* m)
{
  float f[16]; for (int i = 0; i < 16; ++i) f[i] = (float)m[i];
  GLR::LoadMatrix(f);
}
inline void GLR_MultMatrixf(const GLfloat* m) { GLR::MultMatrix(m); }
inline void GLR_MultMatrixd(const GLdouble* m)
{
  float f[16]; for (int i = 0; i < 16; ++i) f[i] = (float)m[i];
  GLR::MultMatrix(f);
}
inline void GLR_PushMatrix() { GLR::PushMatrix(); }
inline void GLR_PopMatrix() { GLR::PopMatrix(); }
inline void GLR_Ortho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{ GLR::Ortho(l, r, b, t, n, f); }
inline void GLR_Translatef(GLfloat x, GLfloat y, GLfloat z) { GLR::Translate(x, y, z); }
inline void GLR_Rotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) { GLR::Rotate(a, x, y, z); }
inline void GLR_Scalef(GLfloat x, GLfloat y, GLfloat z) { GLR::Scale(x, y, z); }
// Matrix queries answer from the native stacks; other queries go to the driver.
inline void GLR_GetFloatv(GLenum pname, GLfloat* v)
{
  if (pname == GL_MODELVIEW_MATRIX) { memcpy(v, GLR::ModelView(), 64); return; }
  if (pname == GL_PROJECTION_MATRIX) { memcpy(v, GLR::Projection(), 64); return; }
  if (pname == GL_TEXTURE_MATRIX) { memcpy(v, GLR::TextureMatrix(GLR::SelectedTexture()), 64); return; }
  glGetFloatv(pname, v);   // GLRTable: the driver, on the render thread
}
// Integer queries: native state where the backend owns it (no driver round trip).
inline void GLR_GetIntegerv(GLenum pname, GLint* v)
{
  if (pname == GL_VIEWPORT) { int t[4]; GLR::GetViewport(t); for (int i = 0; i < 4; ++i) v[i] = t[i]; return; }
  if (pname == GL_FOG) { *v = GLR::FogEnabled() ? 1 : 0; return; }
  glGetIntegerv(pname, v);   // GLRTable: native limits or the driver
}
inline void GLR_GetDoublev(GLenum pname, GLdouble* v)
{
  if ((pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX))
  { float f[16]; GLR_GetFloatv(pname, f); for (int i = 0; i < 16; ++i) v[i] = f[i]; return; }
  glGetDoublev(pname, v);
}

// ---------------------------------------------------------------- current colour
inline void GLR_Color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{ { const float c[4] = { r, g, b, a }; GLR::SetColor(c); } }
inline void GLR_Color4fv(const GLfloat* c) { GLR::SetColor(c); }
inline void GLR_Color3f(GLfloat r, GLfloat g, GLfloat b) { GLR_Color4f(r, g, b, 1.0f); }
inline void GLR_Color3fv(const GLfloat* c) { GLR_Color4f(c[0], c[1], c[2], 1.0f); }
inline void GLR_Color4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { GLR_Color4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
inline void GLR_Color4ubv(const GLubyte* c) { GLR_Color4f(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f); }
inline void GLR_Color3ub(GLubyte r, GLubyte g, GLubyte b) { GLR_Color4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f); }
inline void GLR_Color3ubv(const GLubyte* c) { GLR_Color4f(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f); }

// ---------------------------------------------------------------- raster state
// Texture targets go to the texture stages, capabilities the native backend
// knows to its raster state; anything else stays on the GL table (counted as
// a trap in native mode, so nothing is dropped silently).
inline void GLR_SetCap(GLenum cap, bool on)
{
  if (GLRBridgeDetail::IsTextureCap(cap)) { GLR::EnableTexture(GLRBridgeDetail::Target(cap), on); return; }
  GLR::Enable(cap, on);   // capabilities ES has no equivalent for are ignored
}
inline void GLR_Enable(GLenum cap) { GLR_SetCap(cap, true); }
inline void GLR_Disable(GLenum cap) { GLR_SetCap(cap, false); }
inline void GLR_BlendFunc(GLenum s, GLenum d) { GLR::BlendFunc(s, d); }
inline void GLR_DepthMask(GLboolean on) { GLR::DepthMask(on != 0); }
inline void GLR_DepthFunc(GLenum f) { GLR::DepthFunc(f); }
inline void GLR_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{ GLR::ColorMask(r != 0, g != 0, b != 0, a != 0); }
inline void GLR_AlphaFunc(GLenum f, GLclampf ref) { GLR::AlphaFunc(f, ref); }
inline void GLR_CullFace(GLenum m) { GLR::CullFace(m); }
inline void GLR_FrontFace(GLenum m) { GLR::FrontFace(m); }
inline void GLR_PolygonOffset(GLfloat f, GLfloat u) { GLR::PolygonOffset(f, u); }
inline void GLR_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) { GLR::Viewport(x, y, w, h); }
inline void GLR_Scissor(GLint x, GLint y, GLsizei w, GLsizei h) { GLR::Scissor(x, y, w, h); }
inline void GLR_DepthRange(GLclampd n, GLclampd f) { GLR::DepthRange((float)n, (float)f); }
inline void GLR_ClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { GLR::ClearColor(r, g, b, a); }
inline void GLR_ClearDepth(GLclampd d) { GLR::ClearDepth((float)d); }
inline void GLR_ClearStencil(GLint s) { GLR::ClearStencil(s); }
inline void GLR_Clear(GLbitfield m) { GLR::Clear(m); }

// ---------------------------------------------------------------- fog
inline void GLR_Fogi(GLenum p, GLint v) { GLR::Fogi(p, v); }
inline void GLR_Fogf(GLenum p, GLfloat v) { GLR::Fogf(p, v); }
inline void GLR_Fogfv(GLenum p, const GLfloat* v) { GLR::Fogfv(p, v); }
inline void GLR_Fogiv(GLenum p, const GLint* v)
{
  if (p == GL_FOG_COLOR) { const float c[4] = { v[0] / 2147483647.0f, v[1] / 2147483647.0f, v[2] / 2147483647.0f, v[3] / 2147483647.0f }; GLR::Fogfv(p, c); }
  else GLR::Fogi(p, v[0]);
}

// ---------------------------------------------------------------- immediate mode
inline void GLR_Begin(GLenum prim) { GLR::Begin(prim); }
inline void GLR_End() { GLR::End(); }
inline void GLR_Vertex3f(GLfloat x, GLfloat y, GLfloat z) { GLR::Vertex(x, y, z); }
inline void GLR_Vertex3fv(const GLfloat* v) { GLR::Vertex(v[0], v[1], v[2]); }
inline void GLR_Vertex2f(GLfloat x, GLfloat y) { GLR::Vertex(x, y, 0); }
inline void GLR_Vertex2fv(const GLfloat* v) { GLR::Vertex(v[0], v[1], 0); }
inline void GLR_Vertex2i(GLint x, GLint y) { GLR::Vertex((float)x, (float)y, 0); }
inline void GLR_Vertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{ GLR::Vertex(x / w, y / w, z / w); }
inline void GLR_TexCoord2f(GLfloat s, GLfloat t) { GLR::SetTexCoord(0, s, t, 0, 1); }
inline void GLR_TexCoord2fv(const GLfloat* v) { GLR::SetTexCoord(0, v[0], v[1], 0, 1); }
inline void GLR_TexCoord3f(GLfloat s, GLfloat t, GLfloat r) { GLR::SetTexCoord(0, s, t, r, 1); }
inline void GLR_TexCoord3fv(const GLfloat* v) { GLR::SetTexCoord(0, v[0], v[1], v[2], 1); }
inline void GLR_TexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { GLR::SetTexCoord(0, s, t, r, q); }
inline void GLR_TexCoord4fv(const GLfloat* v) { GLR::SetTexCoord(0, v[0], v[1], v[2], v[3]); }
inline void GLR_MultiTexCoord2fARB(GLenum u, GLfloat s, GLfloat t) { GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), s, t, 0, 1); }
inline void GLR_MultiTexCoord2fvARB(GLenum u, const GLfloat* v) { GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), v[0], v[1], 0, 1); }
inline void GLR_MultiTexCoord3fARB(GLenum u, GLfloat s, GLfloat t, GLfloat r) { GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), s, t, r, 1); }
inline void GLR_MultiTexCoord3fvARB(GLenum u, const GLfloat* v) { GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), v[0], v[1], v[2], 1); }
inline void GLR_MultiTexCoord4fARB(GLenum u, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), s, t, r, q); }
inline void GLR_MultiTexCoord4fvARB(GLenum u, const GLfloat* v) { GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), v[0], v[1], v[2], v[3]); }
inline void GLR_Normal3f(GLfloat x, GLfloat y, GLfloat z) { GLR::SetNormal(x, y, z); }
inline void GLR_Normal3fv(const GLfloat* v) { GLR::SetNormal(v[0], v[1], v[2]); }

// ---------------------------------------------------------------- desktop-only calls
// Fill mode: ES has no line/point polygon mode (wireframe debug only).
inline void GLR_PolygonMode(GLenum, GLenum) {}   // fill only on ES
// The native backend always uses combiners: texture-environment modes have no effect.
inline void GLR_TexEnvi(GLenum, GLenum, GLint) {}   // stage ops come from EF_SetColorOp

// One draw target (the back buffer or the scene FBO).
inline void GLR_DrawBuffer(GLenum) {}   // always the frame being drawn

// ---------------------------------------------------------------- buffers
inline void GLR_GenBuffersARB(GLsizei n, GLuint* b) { GLR::GenBuffers(n, b); }
inline void GLR_DeleteBuffersARB(GLsizei n, const GLuint* b) { GLR::DeleteBuffers(n, b); }
inline void GLR_BindBufferARB(GLenum t, GLuint b) { GLR::BindBuffer(t, b); }
inline void GLR_BufferDataARB(GLenum t, GLsizeiptrARB size, const GLvoid* data, GLenum usage)
{ GLR::BufferData(t, (size_t)size, data, usage); }
inline void GLR_BufferSubDataARB(GLenum t, GLintptrARB off, GLsizeiptrARB size, const GLvoid* data)
{ GLR::BufferSubData(t, (size_t)off, (size_t)size, data); }
inline GLvoid* GLR_MapBufferARB(GLenum t, GLenum) { return GLR::MapBuffer(t); }
inline GLboolean GLR_UnmapBufferARB(GLenum t) { return GLR::UnmapBuffer(t) ? GL_TRUE : GL_FALSE; }

// ---------------------------------------------------------------- vertex arrays and draws
namespace GLRBridgeDetail
{
inline bool ArrayFor(GLenum a, GLR::Attr& out)
{
  switch (a)
  {
    case GL_VERTEX_ARRAY: out = GLR::ATTR_POS; return true;
    case GL_COLOR_ARRAY: out = GLR::ATTR_COLOR; return true;
    case GL_NORMAL_ARRAY: out = GLR::ATTR_NORMAL; return true;
    case GL_TEXTURE_COORD_ARRAY: out = (GLR::Attr)(GLR::ATTR_TEX0 + GLR::ClientActiveTextureUnit()); return true;
    case GL_SECONDARY_COLOR_ARRAY_EXT: out = GLR::ATTR_COLOR1; return true;   // vertex programs (COLOR1)
  }
  return false;   // fog coordinate: not used
}
}
inline void GLR_VertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p)
{ GLR::ArrayPointer(GLR::ATTR_POS, size, type, false, stride, p); }
inline void GLR_ColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p)
{ GLR::ArrayPointer(GLR::ATTR_COLOR, size, type, type != GL_FLOAT, stride, p); }
inline void GLR_SecondaryColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p)
{ GLR::ArrayPointer(GLR::ATTR_COLOR1, size, type, type != GL_FLOAT, stride, p); }
inline void GLR_NormalPointer(GLenum type, GLsizei stride, const GLvoid* p)
{ GLR::ArrayPointer(GLR::ATTR_NORMAL, 3, type, type != GL_FLOAT, stride, p); }
inline void GLR_TexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p)
{ GLR::ArrayPointer((GLR::Attr)(GLR::ATTR_TEX0 + GLR::ClientActiveTextureUnit()), size, type, false, stride, p); }
inline void GLR_DrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* idx)
{ GLR::DrawElements(mode, count, type, idx); }
inline void GLR_DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid* idx)
{ GLR::DrawRangeElements(mode, start, end, count, type, idx); }
inline void GLR_DrawArrays(GLenum mode, GLint first, GLsizei count)
{ GLR::DrawArrays(mode, first, count); }

inline void GLR_ClientActiveTexture(GLenum u) { GLR::ClientActiveTexture((int)(u - GL_TEXTURE0_ARB)); }
inline void GLR_ClientState(GLenum a, bool on)
{
  GLR::Attr at;
  if (GLRBridgeDetail::ArrayFor(a, at)) GLR::EnableArray(at, on);
}
inline void GLR_EnableClientState(GLenum a) { GLR_ClientState(a, true); }
inline void GLR_DisableClientState(GLenum a) { GLR_ClientState(a, false); }

// ---------------------------------------------------------------- lighting and texgen
inline void GLR_TexGeni(GLenum c, GLenum p, GLint v) { GLR::TexGeni(c, p, v); }
inline void GLR_TexGenf(GLenum c, GLenum p, GLfloat v) { GLR::TexGeni(c, p, (GLint)v); }
inline void GLR_TexGenfv(GLenum c, GLenum p, const GLfloat* v) { GLR::TexGenfv(c, p, v); }
inline void GLR_TexGeniv(GLenum c, GLenum p, const GLint* v)
{ { const float f[4] = { (float)v[0], (float)v[1], (float)v[2], (float)v[3] }; if (p == GL_TEXTURE_GEN_MODE) GLR::TexGeni(c, p, v[0]); else GLR::TexGenfv(c, p, f); } }
inline void GLR_Lightfv(GLenum l, GLenum p, const GLfloat* v) { GLR::Lightfv(l, p, v); }
inline void GLR_Lightf(GLenum l, GLenum p, GLfloat v) { GLR::Lightf(l, p, v); }
inline void GLR_Lighti(GLenum l, GLenum p, GLint v) { GLR::Lightf(l, p, (float)v); }
inline void GLR_Materialfv(GLenum f, GLenum p, const GLfloat* v) { GLR::Materialfv(f, p, v); }
inline void GLR_Materialf(GLenum f, GLenum p, GLfloat v) { { const float a[4] = { v, 0, 0, 0 }; GLR::Materialfv(f, p, a); } }
inline void GLR_LightModelfv(GLenum p, const GLfloat* v) { GLR::LightModelfv(p, v); }
inline void GLR_LightModeli(GLenum, GLint) {}   // two-sided / local viewer: not used natively
inline void GLR_ColorMaterial(GLenum f, GLenum m) { GLR::ColorMaterial(f, m); }
inline void GLR_ShadeModel(GLenum) {}                                                  // smooth only on ES
inline void GLR_Hint(GLenum, GLenum) {}
// User clip planes (water reflections/refractions): the native backend clips in its programs.
inline void GLR_ClipPlane(GLenum plane, const GLdouble* eq)
{
  const float f[4] = { (float)eq[0], (float)eq[1], (float)eq[2], (float)eq[3] };
  GLR::ClipPlane((int)(plane - GL_CLIP_PLANE0), f);
}

// ---------------------------------------------------------------- reads and copies of the frame
// The recorded frame is played back first, so they see everything drawn so far.
inline void GLR_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, GLvoid* p)
{ GLR::ReadPixels(x, y, w, h, f, t, p); }
inline void GLR_CopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{ GLR::CopyTexSubImage2D(GLR_Bound(t), t, l, xo, yo, x, y, w, h); }
inline void GLR_CopyTexImage2D(GLenum t, GLint l, GLenum i, GLint x, GLint y, GLsizei w, GLsizei h, GLint b)
{ GLR::CopyTexImage2D(GLR_Bound(t), t, l, i, x, y, w, h, b); }
inline void GLR_ReadBuffer(GLenum) {}   // always the frame being drawn

// ---------------------------------------------------------------- ARB programs (engine Cg programs)
inline void GLR_GenProgramsARB(GLsizei n, GLuint* p) { GLR::GenPrograms(n, p); }
inline void GLR_DeleteProgramsARB(GLsizei n, const GLuint* p) { GLR::DeletePrograms(n, p); }
inline void GLR_BindProgramARB(GLenum t, GLuint p) { GLR::BindProgram(t, p); }
inline void GLR_ProgramStringARB(GLenum t, GLenum fmt, GLsizei len, const void* s)
{ GLR::ProgramString(t, (const char*)s, len); }
inline void GLR_ProgramEnvParameter4fvARB(GLenum t, GLuint i, const GLfloat* v)
{ GLR::ProgramEnvParameter(t, i, v); }
inline void GLR_ProgramEnvParameter4fARB(GLenum t, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{ const GLfloat v[4] = { x, y, z, w }; GLR_ProgramEnvParameter4fvARB(t, i, v); }
