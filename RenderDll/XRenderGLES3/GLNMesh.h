/*=============================================================================
  GLNMesh.h : native mesh draws (stage "mesh"). No GL includes (engine-safe).

  The engine describes a draw (vertex streams of a leaf buffer, the texture
  stages' combiner state it keeps in m_eCurColorOp & co., alpha test, fog,
  matrices); GLNMesh_Draw builds/uses the matching GLSL ES program and draws
  with a VAO. Returns false when the draw needs something not handled
  natively yet; the caller then draws through the translation layer.
=============================================================================*/
#pragma once
#include <stdint.h>

struct GLNMeshStream
{
  unsigned buffer;      // GL buffer object (0 = stream absent)
  unsigned offset;      // byte offset of the first element
  int stride, size;     // bytes between vertices, components
  unsigned type;        // GL component type
  bool normalized;
};

struct GLNMeshStage
{
  unsigned texture;     // GL texture name
  unsigned target;      // GL target (0 = stage disabled)
  uint8_t colorOp, alphaOp, colorArg, alphaArg;   // engine EColorOp / EColorArg codes
  float rgbScale;
  uint8_t constant[4];  // stage constant colour (RGBA bytes)
};

struct GLNLighting
{
  bool on, colorMaterial;
  int count;                                    // enabled lights, in GL order
  float pos[4][4], amb[4][4], diff[4][4], att[4][3];   // eye-space positions
  float matAmb[4], matDiff[4], matEmis[4], sceneAmb[4];
  float normalMat[9];                           // inverse-transpose of the model-view (3x3)
};

struct GLNTexGen
{
  bool cube, texMatOn;
  unsigned texture;      // texture actually bound on the unit
  float envColor[4], scaleRGB, scaleA;   // stage constant colour and scales (glTexEnv)
  uint8_t gen[4];        // per s/t/r/q: 0 none 1 object 2 eye 3 sphere 4 reflection 5 normal
  float obj[16], eye[16], texMat[16];
};

struct GLNMeshDraw
{
  GLNMeshStream pos, color, normal, tex[4];
  int colorSemantic;    // 0 colour, 1 terrain brightness in alpha, 2 vegetation light (x2)
  int numStages;
  GLNMeshStage stage[4];
  int alphaTestMode;    // 0 off, 1 >ref, 2 <ref, 3 >=ref
  float alphaRef;
  int fogMode;          // 0 off, 1 linear, 2 exp, 3 exp2
  float fogStart, fogEnd, fogDensity, fogColor[3];
  unsigned indexBuffer;
  unsigned primitive;   // GL primitive
};

// Draw count indices of type unsigned short starting at byte offset idxOffset.
bool GLNMesh_Draw(const GLNMeshDraw& d, int count, unsigned idxOffset);

// Fallback accounting (why a draw stayed on the translation layer).
void GLNMesh_Fallback(const char* reason);

// Once per frame: periodic native/fallback statistics.
void GLNMesh_FrameStats();

// Current model-view and projection (column-major), from the translation
// layer until matrix setup is native (GLES3FFP.cpp).
void GLES3FFPGetMatrices(float mv[16], float proj[16]);
// True while the layer applies a port override the native path doesn't know
// (the low-spec water alpha), so such draws stay on the layer.
bool GLES3FFPHasEnvOverride();
// Texgen or a texture matrix active on an enabled unit.
bool GLES3FFPUsesTexGen();
// Resolved lighting / texgen state (from the layer until captured natively).
void GLES3FFPGetLighting(GLNLighting* L);
void GLES3FFPGetTexGen(int unit, GLNTexGen* G);
// Streams, index buffer, current colour/texcoords from the layer; false when
// any enabled array is in client memory.
bool GLES3FFPGetArrays(GLNMeshDraw* d, float curColor[4], float curTex[4][4]);
void GLES3FFPGetFog(int* mode, float* start, float* end, float* density, float color[3]);

// Texcoord streams recorded by SArrayPointer_Texture::mfSet (per stage).
extern GLNMeshStream g_GLNTexStream[4];
// Position/colour streams (SArrayPointer_Vertex/_Color::mfSet), the colour
// semantic of the colour stream, and the engine's bound index buffer.
extern GLNMeshStream g_GLNPosStream, g_GLNColorStream, g_GLNNormalStream;
extern int g_GLNColorSemantic;
extern unsigned g_GLNIndexBuffer;
