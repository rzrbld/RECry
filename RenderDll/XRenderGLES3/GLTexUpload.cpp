/*=============================================================================
  GLTexUpload.cpp : texture names, parameters and uploads for GLES 3, shared
  by both backends (the translation layer in layer mode, the native backend
  GLR in native mode). Moved here from GLES3FFP.cpp unchanged in behaviour:
  desktop format mapping and swizzles (ALPHA, INTENSITY, BGR/BGRA), RGB
  sub-image repacking, auto-mipmaps, DXT handling (DXT1 cut-out detection,
  asynchronous DXT->ETC2 conversion with the disk cache, CPU decode
  fallback), engine-safe texture names.
  Every operation names its texture explicitly; the backend's bind/done
  callbacks make it current for the upload and keep the backend's own
  binding cache valid.
=============================================================================*/
#include <GLES3/gl3.h>
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "GLES3TexConvert.h"
#include "GLTexUpload.h"
#include <mutex>

namespace GLTex
{
namespace
{
enum
{
  D_QUADS = 0x0007, D_QUAD_STRIP = 0x0008, D_POLYGON = 0x0009,
  D_MODELVIEW = 0x1700, D_PROJECTION = 0x1701, D_TEXTURE = 0x1702,
  D_MATRIX_MODE = 0x0BA0, D_MODELVIEW_MATRIX = 0x0BA6, D_PROJECTION_MATRIX = 0x0BA7, D_TEXTURE_MATRIX = 0x0BA8,
  D_CURRENT_COLOR = 0x0B00,
  D_LIGHTING = 0x0B50, D_COLOR_MATERIAL = 0x0B57, D_FOG = 0x0B60, D_NORMALIZE = 0x0BA1, D_ALPHA_TEST = 0x0BC0,
  D_LIGHT0 = 0x4000, D_CLIP_PLANE0 = 0x3000,
  D_TEXTURE_1D = 0x0DE0, D_TEXTURE_CUBE_MAP = 0x8513, D_TEXTURE_3D = 0x806F, D_TEXTURE_RECTANGLE = 0x84F5,
  D_TEXTURE_GEN_S = 0x0C60, D_TEXTURE_GEN_T = 0x0C61, D_TEXTURE_GEN_R = 0x0C62, D_TEXTURE_GEN_Q = 0x0C63,
  D_VERTEX_ARRAY = 0x8074, D_NORMAL_ARRAY = 0x8075, D_COLOR_ARRAY = 0x8076, D_TEXTURE_COORD_ARRAY = 0x8078,
  D_SECONDARY_COLOR_ARRAY = 0x845E, D_FOG_COORD_ARRAY = 0x8457,
  D_TEXTURE_ENV = 0x2300, D_TEXTURE_ENV_MODE = 0x2200, D_TEXTURE_ENV_COLOR = 0x2201,
  D_MODULATE = 0x2100, D_DECAL = 0x2101, D_ADD = 0x0104, D_BLEND = 0x0BE2, D_REPLACE = 0x1E01, D_COMBINE = 0x8570,
  D_COMBINE_RGB = 0x8571, D_COMBINE_ALPHA = 0x8572, D_RGB_SCALE = 0x8573, D_ALPHA_SCALE = 0x0D1C,
  D_SOURCE0_RGB = 0x8580, D_SOURCE0_ALPHA = 0x8588, D_OPERAND0_RGB = 0x8590, D_OPERAND0_ALPHA = 0x8598,
  D_ADD_SIGNED = 0x8574, D_INTERPOLATE = 0x8575, D_SUBTRACT = 0x84E7, D_DOT3_RGB = 0x86AE, D_DOT3_RGBA = 0x86AF,
  D_DOT3_RGB_EXT = 0x8740, D_DOT3_RGBA_EXT = 0x8741,
  D_CONSTANT = 0x8576, D_PRIMARY_COLOR = 0x8577, D_PREVIOUS = 0x8578, D_TEXTURE0 = 0x84C0,
  D_SRC_COLOR = 0x0300, D_ONE_MINUS_SRC_COLOR = 0x0301, D_SRC_ALPHA = 0x0302, D_ONE_MINUS_SRC_ALPHA = 0x0303,
  D_FOG_MODE = 0x0B65, D_FOG_DENSITY = 0x0B62, D_FOG_START = 0x0B63, D_FOG_END = 0x0B64, D_FOG_COLOR = 0x0B66,
  D_EXP = 0x0800, D_EXP2 = 0x0801, D_LINEAR_FOG = 0x2601,
  D_S = 0x2000, D_T = 0x2001, D_R = 0x2002, D_Q = 0x2003,
  D_TEXTURE_GEN_MODE = 0x2500, D_OBJECT_PLANE = 0x2501, D_EYE_PLANE = 0x2502,
  D_EYE_LINEAR = 0x2400, D_OBJECT_LINEAR = 0x2401, D_SPHERE_MAP = 0x2402, D_NORMAL_MAP = 0x8511, D_REFLECTION_MAP = 0x8512,
  D_AMBIENT = 0x1200, D_DIFFUSE = 0x1201, D_SPECULAR = 0x1202, D_POSITION = 0x1203,
  D_CONSTANT_ATTENUATION = 0x1207, D_LINEAR_ATTENUATION = 0x1208, D_QUADRATIC_ATTENUATION = 0x1209,
  D_EMISSION = 0x1600, D_SHININESS = 0x1601, D_AMBIENT_AND_DIFFUSE = 0x1602, D_LIGHT_MODEL_AMBIENT = 0x0B53,
  D_CLAMP = 0x2900, D_CLAMP_TO_BORDER = 0x812D, D_GENERATE_MIPMAP = 0x8191, D_TEXTURE_MAX_ANISOTROPY = 0x84FE,
  D_BGR = 0x80E0, D_BGRA = 0x80E1, D_LUMINANCE = 0x1909, D_LUMINANCE_ALPHA = 0x190A, D_ALPHA = 0x1906,
  D_INTENSITY = 0x8049, D_INTENSITY8 = 0x804B, D_ALPHA8 = 0x803C, D_LUMINANCE8 = 0x8040, D_LUMINANCE8_ALPHA8 = 0x8045,
  D_RGB4 = 0x804F, D_RGB5 = 0x8050, D_RGB8 = 0x8051, D_RGBA2 = 0x8055, D_RGBA4 = 0x8056, D_RGB5_A1 = 0x8057, D_RGBA8 = 0x8058,
  D_UNSIGNED_INT_8_8_8_8_REV = 0x8367, D_UNSIGNED_INT_8_8_8_8 = 0x8035,
  D_READ_ONLY = 0x88B8, D_WRITE_ONLY = 0x88B9, D_READ_WRITE = 0x88BA,
  D_MAX_TEXTURE_UNITS = 0x84E2, D_MAX_TEXTURE_COORDS = 0x8871, D_MAX_TEXTURE_IMAGE_UNITS = 0x8872,
  D_DOUBLE = 0x140A,
};

Backend s_be;
void Log(const char* fmt, ...)
{
  char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
  if (s_be.log) s_be.log(b); else fprintf(stderr, "%s\n", b);
}
// Diagnostic: FARCRY_GLR_DEBUG=2 reports GL errors of each texture operation.
GLenum (*r_glGetError_dbg)() = nullptr;
bool DebugErrors() { static const bool on = getenv("FARCRY_GLR_DEBUG") && atoi(getenv("FARCRY_GLR_DEBUG")) >= 2; return on; }
const char* (*s_texName)(unsigned) = nullptr;
// What the current operation is (diagnostic report on a GL error).
char s_opDesc[160] = "";
struct OpScope
{
  GLenum t; GLuint x;
  OpScope(GLenum target, GLuint tex) : t(target), x(tex)
  {
    if (DebugErrors() && r_glGetError_dbg) r_glGetError_dbg();   // start clean
    if (s_be.bind) s_be.bind(target, tex);
  }
  ~OpScope()
  {
    if (DebugErrors() && r_glGetError_dbg)
    {
      static int n = 0; const GLenum e = r_glGetError_dbg();
      if (e && n < 40) { ++n; Log("GLTex error 0x%x: %s tex %u (%s) target 0x%x known format 0x%x", e, s_opDesc, x, s_texName ? s_texName(x) : "?", t, (unsigned)Format(x)); }
    }
    if (s_be.done) s_be.done();
  }
};
// Diagnostic: FARCRY_GLTEX_TRACE=1 logs uploads of 256x256 and larger.
bool TraceTex(int w, int h) { static const bool on = getenv("FARCRY_GLTEX_TRACE") != NULL; return on && w * h >= 256 * 256; }
struct UploadScope
{
  uint64_t bytes; Uint64 t0;
  explicit UploadScope(uint64_t b) : bytes(b), t0(SDL_GetPerformanceCounter()) {}
  ~UploadScope() { if (s_be.uploadStats) s_be.uploadStats((SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency(), bytes); }
};

// Driver entry points (direct, never through the engine's table).
#define GLTEX_FUNCS(X) X(glActiveTexture) X(glBindTexture) X(glCompressedTexImage2D) X(glCompressedTexSubImage2D) \
  X(glDeleteTextures) X(glGenerateMipmap) X(glIsTexture) X(glPixelStorei) X(glTexImage2D) X(glTexParameteri) X(glTexParameterf) X(glTexSubImage2D) \
  X(glReadPixels) X(glCopyTexSubImage2D) X(glCopyTexImage2D)
#define GLTEX_DECLARE(n) decltype(&::n) r_##n = nullptr;
GLTEX_FUNCS(GLTEX_DECLARE)
#undef GLTEX_DECLARE

std::unordered_map<GLuint, bool> s_autoMip;
// Last value per (texture, target kind, parameter): see Param().
struct ParamEntry { uint32_t key; GLint v; };
std::unordered_map<GLuint, std::vector<ParamEntry>> s_paramCache;
static GLenum TexTarget(GLenum t) { return t == D_TEXTURE_RECTANGLE ? GL_TEXTURE_2D : t; }
static bool ValidTexParam(GLenum pname)
{
  switch (pname)
  {
    case GL_TEXTURE_MIN_FILTER: case GL_TEXTURE_MAG_FILTER: case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T: case GL_TEXTURE_WRAP_R:
    case GL_TEXTURE_MIN_LOD: case GL_TEXTURE_MAX_LOD: case GL_TEXTURE_BASE_LEVEL: case GL_TEXTURE_MAX_LEVEL:
    case GL_TEXTURE_COMPARE_MODE: case GL_TEXTURE_COMPARE_FUNC: case D_TEXTURE_MAX_ANISOTROPY:
    case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A:
      return true;
  }
  return false;
}
} // namespace

static std::vector<unsigned char> s_convBuf;

// Map a desktop (internalformat, format, type) upload to an ES3 one. Returns
// the pixel pointer to upload (possibly converted into s_convBuf).
static const void* ConvertUpload(GLint& internal, GLenum& format, GLenum& type, GLsizei w, GLsizei h, const void* pixels, bool& alphaOnly, bool& intensity)
{
  alphaOnly = intensity = false;
  switch (internal)
  {
    case 1: case D_LUMINANCE8: internal = D_LUMINANCE; break;
    case 2: case D_LUMINANCE8_ALPHA8: internal = D_LUMINANCE_ALPHA; break;
    case 3: case D_RGB4: case D_RGB5: case D_RGB8: internal = GL_RGB; break;
    case 4: case D_RGBA2: case D_RGBA4: case D_RGB5_A1: case D_RGBA8: internal = GL_RGBA; break;
    case D_ALPHA8: internal = D_ALPHA; break;
    // Uncompressed data with a DXT internal format asks the driver to
    // compress (sprites, FT2_FORCEDXT). Drivers without S3TC reject that and
    // leave the texture incomplete (black), so store it uncompressed.
    case 0x83F0: internal = GL_RGB; break;
    case 0x83F1: case 0x83F2: case 0x83F3: internal = GL_RGBA; break;
    case D_INTENSITY: case D_INTENSITY8: internal = D_LUMINANCE; intensity = true; break;
  }
  if (type == D_UNSIGNED_INT_8_8_8_8_REV) type = GL_UNSIGNED_BYTE;
  // Sized formats passed as the pixel format (fonts: GL_ALPHA8) are not valid
  // ES pixel formats; sub-image uploads then mismatch and fail.
  if (format == D_ALPHA8) format = D_ALPHA;
  if (format == D_LUMINANCE8) format = D_LUMINANCE;
  if (format == D_LUMINANCE8_ALPHA8) format = D_LUMINANCE_ALPHA;
  if (format == D_ALPHA) alphaOnly = true;
  if ((format == D_BGRA || format == D_BGR) && type == GL_UNSIGNED_BYTE)
  {
    const int bpp = format == D_BGRA ? 4 : 3;
    format = format == D_BGRA ? GL_RGBA : GL_RGB;
    if (pixels)
    {
      // Engine uploads are tightly packed; the default unpack alignment of
      // 4 is satisfied for RGBA and handled row-by-row for RGB below.
      const size_t rowIn = ((size_t)w * bpp + 3) & ~(size_t)3;
      s_convBuf.resize(rowIn * h);
      for (GLsizei y = 0; y < h; ++y)
      {
        const unsigned char* s = (const unsigned char*)pixels + rowIn * y;
        unsigned char* d = s_convBuf.data() + rowIn * y;
        for (GLsizei x = 0; x < w; ++x, s += bpp, d += bpp)
        { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; if (bpp == 4) d[3] = s[3]; }
      }
      pixels = s_convBuf.data();
    }
  }
  // ES unsized formats require internalformat == format.
  if (internal == GL_RGB || internal == GL_RGBA || internal == D_LUMINANCE || internal == D_LUMINANCE_ALPHA || internal == D_ALPHA)
    internal = (GLint)format;
  return pixels;
}

static std::unordered_map<GLuint, GLenum> s_texFormat;
void Image2D(GLuint tex, GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void* pixels)
{
  if (DebugErrors()) snprintf(s_opDesc, sizeof(s_opDesc), "Image2D level %d %dx%d internal 0x%x format 0x%x type 0x%x data %d", level, w, h, (unsigned)internal, (unsigned)format, (unsigned)type, pixels != nullptr);
  OpScope op(target, tex);
  if (TraceTex(w, h)) Log("GLTex trace: %s tex %u target 0x%x level %d %dx%d format 0x%x type 0x%x (known format 0x%x)", "Image2D", tex, target, level, w, h, (unsigned)format, (unsigned)type, (unsigned)Format(tex));
  UploadScope upScope((uint64_t)w * h * 4);
  target = TexTarget(target);
  if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z && getenv("FARCRY_GLES3_TRACE_CUBE"))
    fprintf(stderr, "CUBEUP raw tex=%u face=%d level=%d %dx%d int=0x%x fmt=0x%x\n", tex, target - GL_TEXTURE_CUBE_MAP_POSITIVE_X, level, w, h, internal, format);
  bool alphaOnly, intensity;
  pixels = ConvertUpload(internal, format, type, w, h, pixels, alphaOnly, intensity);
  r_glTexImage2D(target, level, internal, w, h, 0, format, type, pixels);
  if (level == 0 && target == GL_TEXTURE_2D) s_texFormat[tex] = format; // for later sub-image uploads
  if (level == 0 && target != GL_TEXTURE_CUBE_MAP)
  {
    GLenum t = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (GLenum)GL_TEXTURE_CUBE_MAP : target;
    // ES samples ALPHA textures as (0,0,0,a); fixed-function MODULATE expects
    // the colour untouched, so swizzle RGB to one. INTENSITY replicates to A.
    if (alphaOnly) { r_glTexParameteri(t, GL_TEXTURE_SWIZZLE_R, GL_ONE); r_glTexParameteri(t, GL_TEXTURE_SWIZZLE_G, GL_ONE); r_glTexParameteri(t, GL_TEXTURE_SWIZZLE_B, GL_ONE); }
    if (intensity) r_glTexParameteri(t, GL_TEXTURE_SWIZZLE_A, GL_RED);
    auto it = s_autoMip.find(tex);
    if (it != s_autoMip.end() && it->second && t == GL_TEXTURE_2D) r_glGenerateMipmap(t);
  }
}

void SubImage2D(GLuint tex, GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels)
{
  if (DebugErrors()) snprintf(s_opDesc, sizeof(s_opDesc), "SubImage2D level %d at %d,%d %dx%d format 0x%x type 0x%x", level, x, y, w, h, (unsigned)format, (unsigned)type);
  OpScope op(target, tex);
  if (TraceTex(w, h)) Log("GLTex trace: %s tex %u target 0x%x level %d %dx%d format 0x%x type 0x%x (known format 0x%x)", "SubImage2D", tex, target, level, w, h, (unsigned)format, (unsigned)type, (unsigned)Format(tex));
  UploadScope upScope((uint64_t)w * h * 4);
  target = TexTarget(target);
  // Fast path: 32-bit BGRA into an RGB texture (movie frames) in one pass.
  if (target == GL_TEXTURE_2D && format == D_BGRA && (type == GL_UNSIGNED_BYTE || type == D_UNSIGNED_INT_8_8_8_8_REV) && pixels)
  {
    auto f = s_texFormat.find(tex);
    if (f != s_texFormat.end() && f->second == GL_RGB)
    {
      static std::vector<unsigned char> s_rgb;
      s_rgb.resize((size_t)w * h * 3);
      const unsigned char* src = (const unsigned char*)pixels;
      unsigned char* d = s_rgb.data();
      for (size_t n = 0, cnt = (size_t)w * h; n < cnt; ++n, src += 4, d += 3) { d[0] = src[2]; d[1] = src[1]; d[2] = src[0]; }
      r_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      r_glTexSubImage2D(target, level, x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, s_rgb.data());
      r_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      return;
    }
  }
  GLint internal = 0; bool a, i;
  pixels = ConvertUpload(internal, format, type, w, h, pixels, a, i);
  // ES requires the sub-image format to match the texture: the engine updates
  // RGB textures with 32-bit data (video panel: created eTF_0888, updated
  // eTF_8888), which ES rejects. Repack RGBA to RGB for RGB textures.
  if (target == GL_TEXTURE_2D && format == GL_RGBA && type == GL_UNSIGNED_BYTE && pixels)
  {
    auto f = s_texFormat.find(tex);
    if (f != s_texFormat.end() && f->second == GL_RGB)
    {
      static std::vector<unsigned char> s_rgb;
      s_rgb.resize((size_t)w * h * 3);
      const unsigned char* src = (const unsigned char*)pixels;
      for (size_t n = 0; n < (size_t)w * h; ++n) { s_rgb[n*3] = src[n*4]; s_rgb[n*3+1] = src[n*4+1]; s_rgb[n*3+2] = src[n*4+2]; }
      r_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      r_glTexSubImage2D(target, level, x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE, s_rgb.data());
      r_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      pixels = NULL;
    }
  }
  // Alpha-only updates into a texture created RGBA (fonts: the engine
  // converts the 8-bit atlas at creation) fail on ES: expand to (1,1,1,a).
  if (pixels && format == D_ALPHA && type == GL_UNSIGNED_BYTE && target == GL_TEXTURE_2D)
  {
    auto f = s_texFormat.find(tex);
    if (f != s_texFormat.end() && f->second == GL_RGBA)
    {
      static std::vector<unsigned char> s_rgba;
      s_rgba.resize((size_t)w * h * 4);
      const unsigned char* src = (const unsigned char*)pixels;
      for (size_t n = 0; n < (size_t)w * h; ++n) { s_rgba[n*4] = s_rgba[n*4+1] = s_rgba[n*4+2] = 255; s_rgba[n*4+3] = src[n]; }
      r_glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s_rgba.data());
      pixels = NULL;
    }
  }
  if (pixels) r_glTexSubImage2D(target, level, x, y, w, h, format, type, pixels);
  auto it = s_autoMip.find(tex);
  if (level == 0 && it != s_autoMip.end() && it->second && target == GL_TEXTURE_2D) r_glGenerateMipmap(target);
}

///////////////////////////////////////////////////////////////////////////////
// DXT (S3TC) fallback. Many GLES drivers (e.g. Mali) lack S3TC; decode the
// blocks to RGBA8 on the CPU so the textures are correct, at a memory cost.
// Install-time transcoding to ETC2/ASTC is the intended replacement.
///////////////////////////////////////////////////////////////////////////////
enum { D_DXT1_RGB = 0x83F0, D_DXT1_RGBA = 0x83F1, D_DXT3 = 0x83F2, D_DXT5 = 0x83F3 };
static bool s_decodeDXT = false;
static bool IsDXT(GLenum f) { return f >= D_DXT1_RGB && f <= D_DXT5; }

static void Color565(uint16_t c, uint8_t* o) { o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31); o[3] = 255; }

static void DecodeDXTBlock(GLenum fmt, const uint8_t* b, uint8_t out[16][4])
{
  const uint8_t* cb = (fmt == D_DXT3 || fmt == D_DXT5) ? b + 8 : b;
  uint16_t c0 = (uint16_t)(cb[0] | (cb[1] << 8)), c1 = (uint16_t)(cb[2] | (cb[3] << 8));
  uint8_t pal[4][4];
  Color565(c0, pal[0]); Color565(c1, pal[1]);
  const bool dxt1 = (fmt == D_DXT1_RGB || fmt == D_DXT1_RGBA);
  if (!dxt1 || c0 > c1)
    for (int i = 0; i < 3; ++i)
    {
      pal[2][i] = (uint8_t)((2*pal[0][i] + pal[1][i]) / 3);
      pal[3][i] = (uint8_t)((pal[0][i] + 2*pal[1][i]) / 3);
    }
  else
    for (int i = 0; i < 3; ++i)
    {
      pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2);
      pal[3][i] = 0;
    }
  pal[2][3] = 255;
  pal[3][3] = (dxt1 && c0 <= c1 && fmt == D_DXT1_RGBA) ? 0 : 255;
  const uint32_t idx = (uint32_t)(cb[4] | (cb[5] << 8) | (cb[6] << 16) | ((uint32_t)cb[7] << 24));
  for (int p = 0; p < 16; ++p) memcpy(out[p], pal[(idx >> (2*p)) & 3], 4);
  if (fmt == D_DXT3)
    for (int p = 0; p < 16; ++p) { uint8_t a = (b[p/2] >> ((p & 1) * 4)) & 15; out[p][3] = (uint8_t)(a * 17); }
  else if (fmt == D_DXT5)
  {
    uint8_t a[8]; a[0] = b[0]; a[1] = b[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; ++i) a[i+1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
    else { for (int i = 1; i < 5; ++i) a[i+1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
    uint64_t bits = 0; for (int i = 0; i < 6; ++i) bits |= (uint64_t)b[2+i] << (8*i);
    for (int p = 0; p < 16; ++p) out[p][3] = a[(bits >> (3*p)) & 7];
  }
}

static const void* DecodeDXT(GLenum fmt, GLsizei w, GLsizei h, const void* data)
{
  s_convBuf.resize((size_t)w * h * 4);
  const uint8_t* src = (const uint8_t*)data;
  const int bs = (fmt == D_DXT1_RGB || fmt == D_DXT1_RGBA) ? 8 : 16;
  uint8_t blk[16][4];
  for (GLsizei by = 0; by < (h + 3) / 4; ++by)
    for (GLsizei bx = 0; bx < (w + 3) / 4; ++bx, src += bs)
    {
      DecodeDXTBlock(fmt, src, blk);
      for (int py = 0; py < 4; ++py)
        for (int px = 0; px < 4; ++px)
        {
          const GLsizei x = bx*4 + px, y = by*4 + py;
          if (x < w && y < h) memcpy(&s_convBuf[((size_t)y * w + x) * 4], blk[py*4 + px], 4);
        }
    }
  return s_convBuf.data();
}

// Pack the decoded RGBA8 block data to 16 bits per pixel to limit memory on
// 1 GB devices: DXT1 -> RGB565, DXT1 with 1-bit alpha -> RGBA5551,
// DXT3/5 -> RGBA4444. FARCRY_GLES3_DXT_DECODE_RGBA8=1 keeps 32-bit RGBA.
static std::vector<uint16_t> s_pack16;
static bool s_dxtRGBA8 = false;
static const void* PackDecoded(GLenum fmt, GLsizei w, GLsizei h, GLenum& outFormat, GLenum& outType)
{
  if (s_dxtRGBA8) { outFormat = GL_RGBA; outType = GL_UNSIGNED_BYTE; return s_convBuf.data(); }
  const size_t n = (size_t)w * h;
  s_pack16.resize(n);
  const uint8_t* c = s_convBuf.data();
  if (fmt == D_DXT1_RGB)
  {
    outFormat = GL_RGB; outType = GL_UNSIGNED_SHORT_5_6_5;
    for (size_t i = 0; i < n; ++i, c += 4) s_pack16[i] = (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
  }
  else if (fmt == D_DXT1_RGBA)
  {
    outFormat = GL_RGBA; outType = GL_UNSIGNED_SHORT_5_5_5_1;
    for (size_t i = 0; i < n; ++i, c += 4) s_pack16[i] = (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | (c[3] >> 7));
  }
  else
  {
    outFormat = GL_RGBA; outType = GL_UNSIGNED_SHORT_4_4_4_4;
    for (size_t i = 0; i < n; ++i, c += 4) s_pack16[i] = (uint16_t)(((c[0] >> 4) << 12) | ((c[1] >> 4) << 8) | ((c[2] >> 4) << 4) | (c[3] >> 4));
  }
  return s_pack16.data();
}

// Asynchronous conversion state: generation per texture name, so results for
// a texture that was deleted or re-specified meanwhile are dropped.
static std::unordered_map<GLuint, uint32_t> s_texGen;
static std::vector<uint8_t> s_placeholder;
static double s_uploadBudgetMs = 2.0;

// The engine uploads every DXT1 texture as RGB (the PC GL path), but Far Cry's
// DXT1 art uses DXT1's 1-bit alpha for cut-outs (bushes, grass, distant huts
// and ladders; 9 of the 2,598 DXT1 textures),
// which Direct3D always honours. Treating them as RGB made those texels
// opaque: sprites drew as rectangles. A texture whose level 0 has any
// transparent texel (a 3-colour block using index 3) is kept as DXT1 RGBA for
// all its levels; opaque ones stay RGB (same ETC2 size and cache entries).
static bool Dxt1HasAlpha(const void* data, size_t size)
{
  const uint8_t* b = (const uint8_t*)data;
  for (size_t o = 0; o + 8 <= size; o += 8, b += 8)
  {
    const unsigned c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    if (c0 > c1) continue;
    const uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
    for (int i = 0; i < 32; i += 2) if (((idx >> i) & 3) == 3) return true;
  }
  return false;
}
static std::unordered_map<GLuint, bool> s_dxt1Alpha;

void CompressedImage2D(GLuint tex, GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border, GLsizei size, const void* data)
{
  if (DebugErrors()) snprintf(s_opDesc, sizeof(s_opDesc), "CompressedImage2D level %d %dx%d internal 0x%x size %d", level, w, h, (unsigned)internal, (int)size);
  OpScope op(target, tex);
  UploadScope upScope((uint64_t)w * h * 4);
  target = TexTarget(target);
  static const bool s_keepDxt1Rgb = getenv("FARCRY_GLES3_DXT1_OPAQUE") != NULL; // diagnostic: old behaviour
  if (internal == D_DXT1_RGB && target == GL_TEXTURE_2D && !s_keepDxt1Rgb)
  {
    bool& alpha = s_dxt1Alpha[tex];
    if (level == 0) alpha = data && Dxt1HasAlpha(data, (size_t)size);
    if (alpha) internal = D_DXT1_RGBA;
  }
  if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z && getenv("FARCRY_GLES3_TRACE_CUBE"))
    fprintf(stderr, "CUBEUP compressed tex=%u face=%d level=%d %dx%d fmt=0x%x data=%p\n", tex, target - GL_TEXTURE_CUBE_MAP_POSITIVE_X, level, w, h, internal, data);
  if (s_decodeDXT && IsDXT(internal) && data && target == GL_TEXTURE_2D && TexConv_Running())
  {
    const GLuint tex = tex;
    uint32_t& gen = s_texGen[tex];
    if (level == 0) ++gen;
    const GLenum etc2 = TexConv_Etc2Format(internal);
    if (TexConv_TryCached(internal, w, h, data, (size_t)size, s_placeholder))
    {
      r_glCompressedTexImage2D(target, level, etc2, w, h, 0, (GLsizei)s_placeholder.size(), s_placeholder.data());
      return;
    }
    TexConv_Placeholder(internal, w, h, data, s_placeholder);
    r_glCompressedTexImage2D(target, level, etc2, w, h, 0, (GLsizei)s_placeholder.size(), s_placeholder.data());
    TexConv_Enqueue(tex, gen, target, level, internal, w, h, data, (size_t)size);
    return;
  }
  if (s_decodeDXT && IsDXT(internal))
  {
    GLenum format = GL_RGBA, type = GL_UNSIGNED_BYTE;
    const void* px = NULL;
    if (data) { DecodeDXT(internal, w, h, data); px = PackDecoded(internal, w, h, format, type); }
    else if (!s_dxtRGBA8) { format = internal == D_DXT1_RGB ? GL_RGB : GL_RGBA;
                            type = internal == D_DXT1_RGB ? GL_UNSIGNED_SHORT_5_6_5 : (internal == D_DXT1_RGBA ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_SHORT_4_4_4_4); }
    r_glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    r_glTexImage2D(target, level, (GLint)format, w, h, 0, format, type, px);
    r_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return;
  }
  r_glCompressedTexImage2D(target, level, internal, w, h, 0, size, data);
}
void CompressedSubImage2D(GLuint tex, GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLsizei size, const void* data)
{
  if (DebugErrors()) snprintf(s_opDesc, sizeof(s_opDesc), "CompressedSubImage2D level %d %dx%d format 0x%x size %d", level, w, h, (unsigned)fmt, (int)size);
  OpScope op(target, tex);
  UploadScope upScope((uint64_t)w * h * 4);
  target = TexTarget(target);
  if (s_decodeDXT && IsDXT(fmt))
  {
    GLenum format, type;
    DecodeDXT(fmt, w, h, data);
    const void* px = PackDecoded(fmt, w, h, format, type);
    r_glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    r_glTexSubImage2D(target, level, x, y, w, h, format, type, px);
    r_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return;
  }
  r_glCompressedTexSubImage2D(target, level, x, y, w, h, fmt, size, data);
}


// Texture names. The engine's texture manager binds names it chooses itself
// (TX_FIRSTBIND 0x1000 .. TX_LASTBIND 0x4000) without glGenTextures, while
// other code (terrain sectors, sprites, render targets) uses glGenTextures.
// Mali returns generated names inside that range (e.g. 5896), so two textures
// shared one name. Generated names are allocated here, outside the range.
static GLuint s_nextTexName = 1;
static std::vector<GLuint> s_freeTexNames;
static std::unordered_set<GLuint> s_ownTexNames;
static std::mutex s_nameMx;   // names are given out on the game thread, freed on the render thread
void GenNames(GLsizei n, GLuint* out, bool queryGL)
{
  std::lock_guard<std::mutex> lk(s_nameMx);
  for (GLsizei i = 0; i < n; ++i)
  {
    GLuint name = 0;
    while (!name)
    {
      if (!s_freeTexNames.empty()) { name = s_freeTexNames.back(); s_freeTexNames.pop_back(); }
      else
      {
        if (s_nextTexName >= 0x1000 && s_nextTexName < 0x4000) s_nextTexName = 0x4000;
        name = s_nextTexName++;
      }
      if (queryGL && r_glIsTexture(name)) name = 0; // already created by a direct bind (outside the engine range)
    }
    s_ownTexNames.insert(name);
    out[i] = name;
  }
}

void DeleteNames(GLsizei n, const GLuint* tex)
{
  for (GLsizei i = 0; i < n; ++i) s_paramCache.erase(tex[i]);   // a recreated name starts with GL's defaults
  std::lock_guard<std::mutex> lk(s_nameMx);
  for (GLsizei i = 0; i < n; ++i)
  {
    if (tex[i] && s_ownTexNames.erase(tex[i])) s_freeTexNames.push_back(tex[i]);
    auto it = s_texGen.find(tex[i]);
    if (it != s_texGen.end()) ++it->second;
    s_autoMip.erase(tex[i]);
  }
  r_glDeleteTextures(n, tex);
}

// Upload finished conversions. With block=true, wait until every queued level
// is converted and uploaded (used at the end of level loading, so gameplay
// never shows placeholders); otherwise stop after the per-frame budget.
void Pump(bool block)
{
  if (!TexConv_Running()) return;
  const Uint64 t0 = SDL_GetPerformanceCounter();
  const double freq = (double)SDL_GetPerformanceFrequency();
  STexConvResult r;
  int uploaded = 0;
  const size_t pendingAtStart = TexConv_Pending();
  for (;;)
  {
    if (!TexConv_Pop(r))
    {
      if (!block || TexConv_Pending() == 0) break;
      SDL_Delay(1);
      continue;
    }
    auto it = s_texGen.find(r.tex);
    if (it == s_texGen.end() || it->second != r.gen) continue; // deleted or re-specified
    OpScope op(GL_TEXTURE_2D, r.tex);
    r_glCompressedTexSubImage2D(GL_TEXTURE_2D, r.level, 0, 0, r.w, r.h, r.glFormat, (GLsizei)r.data.size(), r.data.data());
    ++uploaded;
    if (!block && (SDL_GetPerformanceCounter() - t0) * 1000.0 / freq >= s_uploadBudgetMs) break;
  }
  const double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / freq;
  if (block)
    Log("GLES3FFP: texture conversions finished before drawing: %d level(s) uploaded, %d pending at start, waited %.0f ms",
           uploaded, (int)pendingAtStart, ms);
}

// Parameters already set to the same value are skipped: the engine re-sets
// filters and wrap modes every frame (fonts, UI), and on the native backend a
// real change to a texture pending draws use plays those draws back first.
void Param(GLuint tex, GLenum target, GLenum pname, GLint v)
{
  target = TexTarget(target);
  if (pname == D_GENERATE_MIPMAP) { s_autoMip[tex] = v != 0; return; }
  if (!ValidTexParam(pname)) return;
  if (v == D_CLAMP || v == D_CLAMP_TO_BORDER) v = GL_CLAMP_TO_EDGE;
  {
    const uint32_t key = ((uint32_t)(target == GL_TEXTURE_CUBE_MAP) << 31) | pname;
    std::vector<ParamEntry>& e = s_paramCache[tex];
    bool found = false;
    for (ParamEntry& p : e) if (p.key == key) { if (p.v == v) return; p.v = v; found = true; break; }
    if (!found) e.push_back(ParamEntry{ key, v });
  }
  OpScope op(target, tex);
  r_glTexParameteri(target, pname, v);
}
void ParamF(GLuint tex, GLenum target, GLenum pname, GLfloat v)
{
  if (pname == D_TEXTURE_MAX_ANISOTROPY || pname == GL_TEXTURE_MIN_LOD || pname == GL_TEXTURE_MAX_LOD)
  {
    if (!ValidTexParam(pname)) return;
    OpScope op(TexTarget(target), tex);
    r_glTexParameterf(TexTarget(target), pname, v);
    return;
  }
  Param(tex, target, pname, (GLint)v);
}

// GLES3 only guarantees RGBA/UNSIGNED_BYTE readback. The engine reads BGRA
// (vegetation sprite generation) and BGR; read RGBA and convert in place.
static std::vector<unsigned char> s_readBuf;
void ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels)
{
  // GLES3 cannot read depth through glReadPixels; the engine's per-light
  // flare/corona tests still try, which only stalls the GPU pipeline.
  static unsigned s_depthReads = 0;
  if (format == 0x1902 /*GL_DEPTH_COMPONENT*/)
  {
    ++s_depthReads;
    if (getenv("FARCRY_GLES3_NO_DEPTH_READ"))
    {
      if (pixels && type == GL_FLOAT) for (GLsizei i = 0; i < w * h; ++i) ((float*)pixels)[i] = 1.0f;
      return;
    }
  }
  if (type == D_UNSIGNED_INT_8_8_8_8_REV) type = GL_UNSIGNED_BYTE;
  const bool bgra = format == D_BGRA, bgr = format == D_BGR, rgb = format == GL_RGB;
  if (type != GL_UNSIGNED_BYTE || (!bgra && !bgr && !rgb))
  {
    r_glReadPixels(x, y, w, h, format, type, pixels);
    return;
  }
  const size_t n = (size_t)w * h;
  if (bgra)
  {
    r_glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    unsigned char* p = (unsigned char*)pixels;
    for (size_t i = 0; i < n; ++i, p += 4) { unsigned char t = p[0]; p[0] = p[2]; p[2] = t; }
    return;
  }
  s_readBuf.resize(n * 4);
  r_glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, s_readBuf.data());
  unsigned char* d = (unsigned char*)pixels;
  const unsigned char* src = s_readBuf.data();
  for (size_t i = 0; i < n; ++i, src += 4, d += 3)
  {
    d[0] = bgr ? src[2] : src[0]; d[1] = src[1]; d[2] = bgr ? src[0] : src[2];
  }
}

void CopyTexSubImage2D(GLuint tex, GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h)
{
  OpScope op(target, tex);
  UploadScope sc((uint64_t)w * h * 4);
  r_glCopyTexSubImage2D(TexTarget(target), level, xo, yo, x, y, w, h);
}
void CopyTexImage2D(GLuint tex, GLenum target, GLint level, GLenum internal, GLint x, GLint y, GLsizei w, GLsizei h, GLint border)
{
  OpScope op(target, tex);
  UploadScope sc((uint64_t)w * h * 4);
  // ES: unsized internal formats only (the engine passes desktop sized ones)
  GLenum f = internal;
  if (f == D_RGBA8 || f == 4 || f == D_RGBA4) f = GL_RGBA; else if (f == D_RGB8 || f == 3 || f == D_RGB5) f = GL_RGB;
  r_glCopyTexImage2D(TexTarget(target), level, f, x, y, w, h, border);
  if (level == 0 && TexTarget(target) == GL_TEXTURE_2D) s_texFormat[tex] = f;
}

bool Init(const Backend& be, bool decodeDXT, bool dxtRGBA8, double uploadBudgetMs)
{
  s_be = be;
  bool ok = true;
#define GLTEX_LOAD(n) r_##n = (decltype(r_##n))SDL_GL_GetProcAddress(#n); if (!r_##n) { Log("GLTex: missing driver entry point %s", #n); ok = false; }
  GLTEX_FUNCS(GLTEX_LOAD)
#undef GLTEX_LOAD
  s_decodeDXT = decodeDXT; s_dxtRGBA8 = dxtRGBA8; s_uploadBudgetMs = uploadBudgetMs;
  r_glGetError_dbg = (GLenum (*)())SDL_GL_GetProcAddress("glGetError");
  return ok;
}
void SetBackend(const Backend& be) { s_be = be; }
void SetTexNameLookup(const char* (*fn)(unsigned)) { s_texName = fn; }
const char* TexName(unsigned tex) { return s_texName ? s_texName(tex) : "?"; }
bool IsAutoMip(GLuint tex) { auto it = s_autoMip.find(tex); return it != s_autoMip.end() && it->second; }
GLenum Format(GLuint tex) { auto it = s_texFormat.find(tex); return it == s_texFormat.end() ? 0 : it->second; }
} // namespace GLTex
