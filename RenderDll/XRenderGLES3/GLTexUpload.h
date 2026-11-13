/*=============================================================================
  GLTexUpload.h : texture names, parameters and uploads shared by both
  backends (see GLTexUpload.cpp). Internal: includes nothing GL-specific,
  uses plain integer types matching GLuint/GLenum/GLint/GLsizei.
=============================================================================*/
#pragma once
#include <stdint.h>

namespace GLTex
{
struct Backend
{
  void (*bind)(unsigned target, unsigned tex) = nullptr;   // make tex current for an upload on target
  void (*done)() = nullptr;                                // upload finished (restore the backend's binding)
  void (*uploadStats)(double ms, uint64_t bytes) = nullptr;
  void (*log)(const char*) = nullptr;
  const char* (*texName)(unsigned tex) = nullptr;           // diagnostics: engine name of a GL texture
};
bool Init(const Backend& be, bool decodeDXT, bool dxtRGBA8, double uploadBudgetMs);
void SetBackend(const Backend& be);
void SetTexNameLookup(const char* (*fn)(unsigned tex));   // kept across backend changes
const char* TexName(unsigned tex);                         // diagnostics ("?" when unknown)

void GenNames(int n, unsigned* out);
void DeleteNames(int n, const unsigned* tex);
void Param(unsigned tex, unsigned target, unsigned pname, int v);
void ParamF(unsigned tex, unsigned target, unsigned pname, float v);
// target: GL_TEXTURE_2D, a cube face, or the desktop rectangle target (mapped to 2D).
void Image2D(unsigned tex, unsigned target, int level, int internal, int w, int h, int border, unsigned format, unsigned type, const void* pixels);
void SubImage2D(unsigned tex, unsigned target, int level, int x, int y, int w, int h, unsigned format, unsigned type, const void* pixels);
void CompressedImage2D(unsigned tex, unsigned target, int level, unsigned internal, int w, int h, int border, int size, const void* data);
void CompressedSubImage2D(unsigned tex, unsigned target, int level, int x, int y, int w, int h, unsigned format, int size, const void* data);
void ReadPixels(int x, int y, int w, int h, unsigned format, unsigned type, void* pixels);   // desktop formats (BGRA, BGR, RGB) converted
void CopyTexSubImage2D(unsigned tex, unsigned target, int level, int xo, int yo, int x, int y, int w, int h);
void CopyTexImage2D(unsigned tex, unsigned target, int level, unsigned internal, int x, int y, int w, int h, int border);
void Pump(bool block);           // upload finished DXT->ETC2 conversions (block: wait for all)
bool IsAutoMip(unsigned tex);
unsigned Format(unsigned tex);   // level-0 pixel format of a 2D texture (0 unknown)
}
