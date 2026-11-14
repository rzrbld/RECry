/*=============================================================================
  GLRBridge.h : engine-side calls into the native backend (GLR.h).

  Renderer code calls these instead of the GL table for texture binding,
  texture units, texture creation/upload/parameters. In layer mode they call
  the GL table exactly as before; in native mode (FARCRY_RENDERER=native)
  they go to the native backend and the shared texture module.
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
  if (GLR::Active()) GLR::SelectTexture((int)(unit - GL_TEXTURE0_ARB));
  else glActiveTextureARB(unit);
}
extern int TargetTex[];   // GLSystem.cpp: each texture's target, recorded at bind (used by video updates)
inline void GLR_BindTexture(GLenum target, GLuint name)
{
  if (GLR::Active())
  {
    // The engine's bind hook (GLSystem.cpp nglBindTexture) records the
    // texture's target; keep that bookkeeping on the native path too.
    if (name < TX_LASTBIND) TargetTex[name] = (int)target;
    GLR::BindTexture(GLRBridgeDetail::Target(target), name);
  }
  else glBindTexture(target, name);
}
// glEnable/glDisable of a texture target (other capabilities: GL table).
inline void GLR_EnableTexture(GLenum cap, bool on)
{
  if (GLR::Active()) { GLR::EnableTexture(GLRBridgeDetail::Target(cap), on); return; }
  if (on) glEnable(cap); else glDisable(cap);
}

// ---------------------------------------------------------------- texture objects
inline void GLR_GenTextures(GLsizei n, GLuint* out)
{
  if (GLR::Active()) GLR::TexGenNames(n, out);
  else glGenTextures(n, out);
}
inline void GLR_DeleteTextures(GLsizei n, const GLuint* tex)
{
  if (GLR::Active()) GLR::TexDeleteNames(n, tex);
  else glDeleteTextures(n, tex);
}
inline unsigned GLR_Bound(GLenum target) { return GLR::BoundTexture(GLRBridgeDetail::Target(target)); }
inline void GLR_TexParameteri(GLenum target, GLenum pname, GLint v)
{
  if (GLR::Active()) GLR::TexParam(GLR_Bound(target), target, pname, v);
  else glTexParameteri(target, pname, v);
}
inline void GLR_TexParameterf(GLenum target, GLenum pname, GLfloat v)
{
  if (GLR::Active()) GLR::TexParamF(GLR_Bound(target), target, pname, v);
  else glTexParameterf(target, pname, v);
}
inline void GLR_TexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const GLvoid* pixels)
{
  if (GLR::Active()) GLR::TexImage2D(GLR_Bound(target), target, level, internal, w, h, border, format, type, pixels);
  else glTexImage2D(target, level, internal, w, h, border, format, type, pixels);
}
inline void GLR_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const GLvoid* pixels)
{
  if (GLR::Active()) GLR::TexSubImage2D(GLR_Bound(target), target, level, x, y, w, h, format, type, pixels);
  else glTexSubImage2D(target, level, x, y, w, h, format, type, pixels);
}
inline void GLR_CompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border, GLsizei size, const GLvoid* data)
{
  if (GLR::Active()) GLR::TexCompressedImage2D(GLR_Bound(target), target, level, internal, w, h, border, size, data);
  else glCompressedTexImage2DARB(target, level, internal, w, h, border, size, data);
}
inline void GLR_CompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLsizei size, const GLvoid* data)
{
  if (GLR::Active()) GLR::TexCompressedSubImage2D(GLR_Bound(target), target, level, x, y, w, h, format, size, data);
  else glCompressedTexSubImage2DARB(target, level, x, y, w, h, format, size, data);
}

// ---------------------------------------------------------------- matrices
inline void GLR_MatrixMode(GLenum m)
{
  if (GLR::Active()) GLR::MatrixModeSet(m == GL_PROJECTION ? GLR::MAT_PROJECTION : (m == GL_TEXTURE ? GLR::MAT_TEXTURE : GLR::MAT_MODELVIEW));
  else glMatrixMode(m);
}
inline void GLR_LoadIdentity() { if (GLR::Active()) GLR::LoadIdentity(); else glLoadIdentity(); }
inline void GLR_LoadMatrixf(const GLfloat* m) { if (GLR::Active()) GLR::LoadMatrix(m); else glLoadMatrixf(m); }
inline void GLR_LoadMatrixd(const GLdouble* m)
{
  if (GLR::Active()) { float f[16]; for (int i = 0; i < 16; ++i) f[i] = (float)m[i]; GLR::LoadMatrix(f); }
  else glLoadMatrixd(m);
}
inline void GLR_MultMatrixf(const GLfloat* m) { if (GLR::Active()) GLR::MultMatrix(m); else glMultMatrixf(m); }
inline void GLR_MultMatrixd(const GLdouble* m)
{
  if (GLR::Active()) { float f[16]; for (int i = 0; i < 16; ++i) f[i] = (float)m[i]; GLR::MultMatrix(f); }
  else glMultMatrixd(m);
}
inline void GLR_PushMatrix() { if (GLR::Active()) GLR::PushMatrix(); else glPushMatrix(); }
inline void GLR_PopMatrix() { if (GLR::Active()) GLR::PopMatrix(); else glPopMatrix(); }
inline void GLR_Ortho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{ if (GLR::Active()) GLR::Ortho(l, r, b, t, n, f); else glOrtho(l, r, b, t, n, f); }
inline void GLR_Translatef(GLfloat x, GLfloat y, GLfloat z) { if (GLR::Active()) GLR::Translate(x, y, z); else glTranslatef(x, y, z); }
inline void GLR_Rotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) { if (GLR::Active()) GLR::Rotate(a, x, y, z); else glRotatef(a, x, y, z); }
inline void GLR_Scalef(GLfloat x, GLfloat y, GLfloat z) { if (GLR::Active()) GLR::Scale(x, y, z); else glScalef(x, y, z); }
// Matrix queries answer from the native stacks; other queries go to the driver.
inline void GLR_GetFloatv(GLenum pname, GLfloat* v)
{
  if (GLR::Active())
  {
    if (pname == GL_MODELVIEW_MATRIX) { memcpy(v, GLR::ModelView(), 64); return; }
    if (pname == GL_PROJECTION_MATRIX) { memcpy(v, GLR::Projection(), 64); return; }
    if (pname == GL_TEXTURE_MATRIX) { memcpy(v, GLR::TextureMatrix(GLR::SelectedTexture()), 64); return; }
  }
  glGetFloatv(pname, v);
}
// Integer queries: native state where the backend owns it (no driver round trip).
inline void GLR_GetIntegerv(GLenum pname, GLint* v)
{
  if (GLR::Active())
  {
    if (pname == GL_VIEWPORT) { int t[4]; GLR::GetViewport(t); for (int i = 0; i < 4; ++i) v[i] = t[i]; return; }
    if (pname == GL_FOG) { *v = GLR::FogEnabled() ? 1 : 0; return; }
  }
  glGetIntegerv(pname, v);
}
inline void GLR_GetDoublev(GLenum pname, GLdouble* v)
{
  if (GLR::Active() && (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX))
  { float f[16]; GLR_GetFloatv(pname, f); for (int i = 0; i < 16; ++i) v[i] = f[i]; return; }
  glGetDoublev(pname, v);
}

// ---------------------------------------------------------------- current colour
inline void GLR_Color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{ if (GLR::Active()) { const float c[4] = { r, g, b, a }; GLR::SetColor(c); } else glColor4f(r, g, b, a); }
inline void GLR_Color4fv(const GLfloat* c) { if (GLR::Active()) GLR::SetColor(c); else glColor4fv(c); }
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
  if (GLR::Active())
  {
    if (GLRBridgeDetail::IsTextureCap(cap)) { GLR::EnableTexture(GLRBridgeDetail::Target(cap), on); return; }
    if (GLR::Enable(cap, on)) return;
  }
  if (on) glEnable(cap); else glDisable(cap);
}
inline void GLR_Enable(GLenum cap) { GLR_SetCap(cap, true); }
inline void GLR_Disable(GLenum cap) { GLR_SetCap(cap, false); }
inline void GLR_BlendFunc(GLenum s, GLenum d) { if (GLR::Active()) GLR::BlendFunc(s, d); else glBlendFunc(s, d); }
inline void GLR_DepthMask(GLboolean on) { if (GLR::Active()) GLR::DepthMask(on != 0); else glDepthMask(on); }
inline void GLR_DepthFunc(GLenum f) { if (GLR::Active()) GLR::DepthFunc(f); else glDepthFunc(f); }
inline void GLR_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{ if (GLR::Active()) GLR::ColorMask(r != 0, g != 0, b != 0, a != 0); else glColorMask(r, g, b, a); }
inline void GLR_AlphaFunc(GLenum f, GLclampf ref) { if (GLR::Active()) GLR::AlphaFunc(f, ref); else glAlphaFunc(f, ref); }
inline void GLR_CullFace(GLenum m) { if (GLR::Active()) GLR::CullFace(m); else glCullFace(m); }
inline void GLR_FrontFace(GLenum m) { if (GLR::Active()) GLR::FrontFace(m); else glFrontFace(m); }
inline void GLR_PolygonOffset(GLfloat f, GLfloat u) { if (GLR::Active()) GLR::PolygonOffset(f, u); else glPolygonOffset(f, u); }
inline void GLR_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) { if (GLR::Active()) GLR::Viewport(x, y, w, h); else glViewport(x, y, w, h); }
inline void GLR_Scissor(GLint x, GLint y, GLsizei w, GLsizei h) { if (GLR::Active()) GLR::Scissor(x, y, w, h); else glScissor(x, y, w, h); }
inline void GLR_DepthRange(GLclampd n, GLclampd f) { if (GLR::Active()) GLR::DepthRange((float)n, (float)f); else glDepthRange(n, f); }
inline void GLR_ClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { if (GLR::Active()) GLR::ClearColor(r, g, b, a); else glClearColor(r, g, b, a); }
inline void GLR_ClearDepth(GLclampd d) { if (GLR::Active()) GLR::ClearDepth((float)d); else glClearDepth(d); }
inline void GLR_ClearStencil(GLint s) { if (GLR::Active()) GLR::ClearStencil(s); else glClearStencil(s); }
inline void GLR_Clear(GLbitfield m) { if (GLR::Active()) GLR::Clear(m); else glClear(m); }

// ---------------------------------------------------------------- fog
inline void GLR_Fogi(GLenum p, GLint v) { if (GLR::Active()) GLR::Fogi(p, v); else glFogi(p, v); }
inline void GLR_Fogf(GLenum p, GLfloat v) { if (GLR::Active()) GLR::Fogf(p, v); else glFogf(p, v); }
inline void GLR_Fogfv(GLenum p, const GLfloat* v) { if (GLR::Active()) GLR::Fogfv(p, v); else glFogfv(p, v); }
inline void GLR_Fogiv(GLenum p, const GLint* v)
{
  if (GLR::Active()) { if (p == GL_FOG_COLOR) { const float c[4] = { v[0] / 2147483647.0f, v[1] / 2147483647.0f, v[2] / 2147483647.0f, v[3] / 2147483647.0f }; GLR::Fogfv(p, c); } else GLR::Fogi(p, v[0]); }
  else glFogiv(p, v);
}

// ---------------------------------------------------------------- immediate mode
inline void GLR_Begin(GLenum prim) { if (GLR::Active()) GLR::Begin(prim); else glBegin(prim); }
inline void GLR_End() { if (GLR::Active()) GLR::End(); else glEnd(); }
inline void GLR_Vertex3f(GLfloat x, GLfloat y, GLfloat z) { if (GLR::Active()) GLR::Vertex(x, y, z); else glVertex3f(x, y, z); }
inline void GLR_Vertex3fv(const GLfloat* v) { if (GLR::Active()) GLR::Vertex(v[0], v[1], v[2]); else glVertex3fv(v); }
inline void GLR_Vertex2f(GLfloat x, GLfloat y) { if (GLR::Active()) GLR::Vertex(x, y, 0); else glVertex2f(x, y); }
inline void GLR_Vertex2fv(const GLfloat* v) { if (GLR::Active()) GLR::Vertex(v[0], v[1], 0); else glVertex2fv(v); }
inline void GLR_Vertex2i(GLint x, GLint y) { if (GLR::Active()) GLR::Vertex((float)x, (float)y, 0); else glVertex2i(x, y); }
inline void GLR_Vertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{ if (GLR::Active()) GLR::Vertex(x / w, y / w, z / w); else glVertex4f(x, y, z, w); }
inline void GLR_TexCoord2f(GLfloat s, GLfloat t) { if (GLR::Active()) GLR::SetTexCoord(0, s, t, 0, 1); else glTexCoord2f(s, t); }
inline void GLR_TexCoord2fv(const GLfloat* v) { if (GLR::Active()) GLR::SetTexCoord(0, v[0], v[1], 0, 1); else glTexCoord2fv(v); }
inline void GLR_TexCoord3f(GLfloat s, GLfloat t, GLfloat r) { if (GLR::Active()) GLR::SetTexCoord(0, s, t, r, 1); else glTexCoord3f(s, t, r); }
inline void GLR_TexCoord3fv(const GLfloat* v) { if (GLR::Active()) GLR::SetTexCoord(0, v[0], v[1], v[2], 1); else glTexCoord3fv(v); }
inline void GLR_TexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { if (GLR::Active()) GLR::SetTexCoord(0, s, t, r, q); else glTexCoord4f(s, t, r, q); }
inline void GLR_TexCoord4fv(const GLfloat* v) { if (GLR::Active()) GLR::SetTexCoord(0, v[0], v[1], v[2], v[3]); else glTexCoord4fv(v); }
inline void GLR_MultiTexCoord2fARB(GLenum u, GLfloat s, GLfloat t) { if (GLR::Active()) GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), s, t, 0, 1); else glMultiTexCoord2fARB(u, s, t); }
inline void GLR_MultiTexCoord2fvARB(GLenum u, const GLfloat* v) { if (GLR::Active()) GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), v[0], v[1], 0, 1); else glMultiTexCoord2fvARB(u, v); }
inline void GLR_MultiTexCoord3fARB(GLenum u, GLfloat s, GLfloat t, GLfloat r) { if (GLR::Active()) GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), s, t, r, 1); else glMultiTexCoord3fARB(u, s, t, r); }
inline void GLR_MultiTexCoord3fvARB(GLenum u, const GLfloat* v) { if (GLR::Active()) GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), v[0], v[1], v[2], 1); else glMultiTexCoord3fvARB(u, v); }
inline void GLR_MultiTexCoord4fARB(GLenum u, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { if (GLR::Active()) GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), s, t, r, q); else glMultiTexCoord4fARB(u, s, t, r, q); }
inline void GLR_MultiTexCoord4fvARB(GLenum u, const GLfloat* v) { if (GLR::Active()) GLR::SetTexCoord((int)(u - GL_TEXTURE0_ARB), v[0], v[1], v[2], v[3]); else glMultiTexCoord4fvARB(u, v); }
inline void GLR_Normal3f(GLfloat x, GLfloat y, GLfloat z) { if (GLR::Active()) GLR::SetNormal(x, y, z); else glNormal3f(x, y, z); }
inline void GLR_Normal3fv(const GLfloat* v) { if (GLR::Active()) GLR::SetNormal(v[0], v[1], v[2]); else glNormal3fv(v); }

// ---------------------------------------------------------------- desktop-only calls
// Fill mode: ES has no line/point polygon mode (wireframe debug only).
inline void GLR_PolygonMode(GLenum face, GLenum mode) { if (GLR::Active()) { (void)face; (void)mode; return; } glPolygonMode(face, mode); }
// The native backend always uses combiners (stage ops come from
// EF_SetColorOp); other texture-environment calls stay on the GL table.
inline void GLR_TexEnvi(GLenum target, GLenum pname, GLint v)
{
  if (GLR::Active() && pname == GL_TEXTURE_ENV_MODE && v == GL_COMBINE_EXT) return;
  glTexEnvi(target, pname, v);
}

// One draw target (the back buffer or the scene FBO).
inline void GLR_DrawBuffer(GLenum b) { if (!GLR::Active()) glDrawBuffer(b); }

// ---------------------------------------------------------------- buffers
inline void GLR_GenBuffersARB(GLsizei n, GLuint* b) { if (GLR::Active()) GLR::GenBuffers(n, b); else glGenBuffersARB(n, b); }
inline void GLR_DeleteBuffersARB(GLsizei n, const GLuint* b) { if (GLR::Active()) GLR::DeleteBuffers(n, b); else glDeleteBuffersARB(n, b); }
inline void GLR_BindBufferARB(GLenum t, GLuint b) { if (GLR::Active()) GLR::BindBuffer(t, b); else glBindBufferARB(t, b); }
inline void GLR_BufferDataARB(GLenum t, GLsizeiptrARB size, const GLvoid* data, GLenum usage)
{ if (GLR::Active()) GLR::BufferData(t, (size_t)size, data, usage); else glBufferDataARB(t, size, data, usage); }
inline void GLR_BufferSubDataARB(GLenum t, GLintptrARB off, GLsizeiptrARB size, const GLvoid* data)
{ if (GLR::Active()) GLR::BufferSubData(t, (size_t)off, (size_t)size, data); else glBufferSubDataARB(t, off, size, data); }
inline GLvoid* GLR_MapBufferARB(GLenum t, GLenum access) { if (GLR::Active()) return GLR::MapBuffer(t); return glMapBufferARB(t, access); }
inline GLboolean GLR_UnmapBufferARB(GLenum t) { if (GLR::Active()) return GLR::UnmapBuffer(t) ? GL_TRUE : GL_FALSE; return glUnmapBufferARB(t); }

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
{ if (GLR::Active()) GLR::ArrayPointer(GLR::ATTR_POS, size, type, false, stride, p); else glVertexPointer(size, type, stride, p); }
inline void GLR_ColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p)
{ if (GLR::Active()) GLR::ArrayPointer(GLR::ATTR_COLOR, size, type, type != GL_FLOAT, stride, p); else glColorPointer(size, type, stride, p); }
inline void GLR_SecondaryColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p)
{ if (GLR::Active()) GLR::ArrayPointer(GLR::ATTR_COLOR1, size, type, type != GL_FLOAT, stride, p); else glSecondaryColorPointerEXT(size, type, stride, p); }
inline void GLR_NormalPointer(GLenum type, GLsizei stride, const GLvoid* p)
{ if (GLR::Active()) GLR::ArrayPointer(GLR::ATTR_NORMAL, 3, type, type != GL_FLOAT, stride, p); else glNormalPointer(type, stride, p); }
inline void GLR_TexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* p)
{ if (GLR::Active()) GLR::ArrayPointer((GLR::Attr)(GLR::ATTR_TEX0 + GLR::ClientActiveTextureUnit()), size, type, false, stride, p); else glTexCoordPointer(size, type, stride, p); }
inline void GLR_DrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* idx)
{ if (GLR::Active()) GLR::DrawElements(mode, count, type, idx); else glDrawElements(mode, count, type, idx); }
inline void GLR_DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid* idx)
{ if (GLR::Active()) GLR::DrawRangeElements(mode, start, end, count, type, idx); else glDrawRangeElementsEXT(mode, start, end, count, type, idx); }
inline void GLR_DrawArrays(GLenum mode, GLint first, GLsizei count)
{ if (GLR::Active()) GLR::DrawArrays(mode, first, count); else glDrawArrays(mode, first, count); }

inline void GLR_ClientActiveTexture(GLenum u) { if (GLR::Active()) GLR::ClientActiveTexture((int)(u - GL_TEXTURE0_ARB)); else glClientActiveTextureARB(u); }
inline void GLR_ClientState(GLenum a, bool on)
{
  if (GLR::Active()) { GLR::Attr at; if (GLRBridgeDetail::ArrayFor(a, at)) GLR::EnableArray(at, on); return; }
  if (on) glEnableClientState(a); else glDisableClientState(a);
}
inline void GLR_EnableClientState(GLenum a) { GLR_ClientState(a, true); }
inline void GLR_DisableClientState(GLenum a) { GLR_ClientState(a, false); }

// ---------------------------------------------------------------- lighting and texgen
inline void GLR_TexGeni(GLenum c, GLenum p, GLint v) { if (GLR::Active()) GLR::TexGeni(c, p, v); else glTexGeni(c, p, v); }
inline void GLR_TexGenf(GLenum c, GLenum p, GLfloat v) { if (GLR::Active()) GLR::TexGeni(c, p, (GLint)v); else glTexGenf(c, p, v); }
inline void GLR_TexGenfv(GLenum c, GLenum p, const GLfloat* v) { if (GLR::Active()) GLR::TexGenfv(c, p, v); else glTexGenfv(c, p, v); }
inline void GLR_TexGeniv(GLenum c, GLenum p, const GLint* v)
{ if (GLR::Active()) { const float f[4] = { (float)v[0], (float)v[1], (float)v[2], (float)v[3] }; if (p == GL_TEXTURE_GEN_MODE) GLR::TexGeni(c, p, v[0]); else GLR::TexGenfv(c, p, f); } else glTexGeniv(c, p, v); }
inline void GLR_Lightfv(GLenum l, GLenum p, const GLfloat* v) { if (GLR::Active()) GLR::Lightfv(l, p, v); else glLightfv(l, p, v); }
inline void GLR_Lightf(GLenum l, GLenum p, GLfloat v) { if (GLR::Active()) GLR::Lightf(l, p, v); else glLightf(l, p, v); }
inline void GLR_Lighti(GLenum l, GLenum p, GLint v) { if (GLR::Active()) GLR::Lightf(l, p, (float)v); else glLighti(l, p, v); }
inline void GLR_Materialfv(GLenum f, GLenum p, const GLfloat* v) { if (GLR::Active()) GLR::Materialfv(f, p, v); else glMaterialfv(f, p, v); }
inline void GLR_Materialf(GLenum f, GLenum p, GLfloat v) { if (GLR::Active()) { const float a[4] = { v, 0, 0, 0 }; GLR::Materialfv(f, p, a); } else glMaterialf(f, p, v); }
inline void GLR_LightModelfv(GLenum p, const GLfloat* v) { if (GLR::Active()) GLR::LightModelfv(p, v); else glLightModelfv(p, v); }
inline void GLR_LightModeli(GLenum p, GLint v) { if (GLR::Active()) { (void)p; (void)v; return; } glLightModeli(p, v); }   // two-sided / local viewer: not used natively
inline void GLR_ColorMaterial(GLenum f, GLenum m) { if (GLR::Active()) GLR::ColorMaterial(f, m); else glColorMaterial(f, m); }
inline void GLR_ShadeModel(GLenum m) { if (!GLR::Active()) glShadeModel(m); }                                                  // smooth only on ES
inline void GLR_Hint(GLenum t, GLenum m) { if (!GLR::Active()) glHint(t, m); }
// User clip planes (water reflections/refractions): the native backend clips in its programs.
inline void GLR_ClipPlane(GLenum plane, const GLdouble* eq)
{
  if (GLR::Active()) { const float f[4] = { (float)eq[0], (float)eq[1], (float)eq[2], (float)eq[3] }; GLR::ClipPlane((int)(plane - GL_CLIP_PLANE0), f); }
  else glClipPlane(plane, eq);
}

// ---------------------------------------------------------------- reads and copies of the frame
// The recorded frame is played back first, so they see everything drawn so far.
inline void GLR_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, GLvoid* p)
{ if (GLR::Active()) GLR::ReadPixels(x, y, w, h, f, t, p); else glReadPixels(x, y, w, h, f, t, p); }
inline void GLR_CopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{ if (GLR::Active()) GLR::CopyTexSubImage2D(GLR_Bound(t), t, l, xo, yo, x, y, w, h); else glCopyTexSubImage2D(t, l, xo, yo, x, y, w, h); }
inline void GLR_CopyTexImage2D(GLenum t, GLint l, GLenum i, GLint x, GLint y, GLsizei w, GLsizei h, GLint b)
{ if (GLR::Active()) GLR::CopyTexImage2D(GLR_Bound(t), t, l, i, x, y, w, h, b); else glCopyTexImage2D(t, l, i, x, y, w, h, b); }
inline void GLR_ReadBuffer(GLenum b) { if (!GLR::Active()) glReadBuffer(b); }   // always the frame being drawn

// ---------------------------------------------------------------- ARB programs (engine Cg programs)
inline void GLR_GenProgramsARB(GLsizei n, GLuint* p) { if (GLR::Active()) GLR::GenPrograms(n, p); else glGenProgramsARB(n, p); }
inline void GLR_DeleteProgramsARB(GLsizei n, const GLuint* p) { if (GLR::Active()) GLR::DeletePrograms(n, p); else glDeleteProgramsARB(n, p); }
inline void GLR_BindProgramARB(GLenum t, GLuint p) { if (GLR::Active()) GLR::BindProgram(t, p); else glBindProgramARB(t, p); }
inline void GLR_ProgramStringARB(GLenum t, GLenum fmt, GLsizei len, const void* s)
{ if (GLR::Active()) GLR::ProgramString(t, (const char*)s, len); else glProgramStringARB(t, fmt, len, s); }
inline void GLR_ProgramEnvParameter4fvARB(GLenum t, GLuint i, const GLfloat* v)
{ if (GLR::Active()) GLR::ProgramEnvParameter(t, i, v); else glProgramEnvParameter4fvARB(t, i, v); }
inline void GLR_ProgramEnvParameter4fARB(GLenum t, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{ const GLfloat v[4] = { x, y, z, w }; GLR_ProgramEnvParameter4fvARB(t, i, v); }
