/*=============================================================================
  GLN2D.h : native 2D drawing (stage "2d"). No GL includes (engine-safe).
  Each call returns false when the stage is off or unavailable; the caller
  then draws through the translation layer as before.
=============================================================================*/
#pragma once

// One quad (Draw2dImage). pos: 4 corners in 2D pixel space (x, y, z), in
// drawing order; st: texcoords; rgba: colour 0..1. alphaTestMode: 0 off,
// 1 a > ref, 2 a < ref, 3 a >= ref. zScale maps z to clip depth (glOrtho:
// -1 / (far range)).
bool GLN2D_DrawQuad(const float pos[4][3], const float st[4][2], const float rgba[4], bool textured,
                    int alphaTestMode, float alphaRef, float width, float height, float zScale);

// Font glyph triangles (FFont via DrawDynVB): 24-byte vertices
// (x, y, z float, rgba bytes, s, t float) in 2D pixel space.
bool GLN2D_DrawFontTriangles(const void* verts, int nVerts, float width, float height);

// Once per frame: periodic log of native 2D draw counts.
void GLN2D_FrameStats();
