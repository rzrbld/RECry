/*=============================================================================
  GLR.h : native OpenGL ES 3.0 backend (d3es-style). Public, engine-safe
  interface: no GL headers (engine renderer files name their GL table like
  the real functions). Enum arguments use GL's numeric values.

  Model:
  - FARCRY_RENDERER=native selects this backend for the whole run; the
    engine's GL table is then trapped (GLAudit.h) and the backend owns GL.
  - The renderer's state calls (through GLRBridge.h) set the CURRENT state
    here, CPU only: raster state, texture stages, combiners, matrices, fog,
    current vertex attributes.
  - Draws snapshot the current state into a recorded command; per-frame
    vertex and index data are copied into the frame ring.
  - EndFrame (before the swap) plays the command list back through one GL
    state cache. Flush() plays back what is recorded so far: before anything
    that must see the frame so far (copies, reads, a texture or buffer update
    that pending draws still use).
=============================================================================*/
#pragma once
#include <functional>
#include <stdint.h>
#include <stddef.h>

namespace GLR
{
// ---------------------------------------------------------------- lifetime
bool Active();                       // FARCRY_RENDERER=native (decided once)
bool Init();                         // after the GL context exists
void Shutdown();

// ---------------------------------------------------------------- frame
void BeginFrame();
void EndFrame();                     // play back the frame (before the swap)
void Flush();                        // play back what is recorded so far

// ---------------------------------------------------------------- raster state
// Exact GL semantics of the state calls the engine makes (GL enum values).
bool Enable(unsigned cap, bool on);  // false: capability not handled here
void BlendFunc(unsigned src, unsigned dst);
void DepthMask(bool on);
void DepthFunc(unsigned func);
void ColorMask(bool r, bool g, bool b, bool a);
void AlphaFunc(unsigned func, float ref);
void CullFace(unsigned mode);
void FrontFace(unsigned mode);
void PolygonOffset(float factor, float units);
void Viewport(int x, int y, int w, int h);
void GetViewport(int v[4]);         // current (recorded) viewport
bool FogEnabled();
void Scissor(int x, int y, int w, int h);
void DepthRange(float n, float f);
void ClearColor(float r, float g, float b, float a);
void ClearDepth(float d);
void ClearStencil(int s);
void Clear(unsigned glMask);         // recorded in order
// Engine GS_* bits (CryCommon/IRenderer.h), for code that has them.
void SetState(uint32_t gsBits);

// ---------------------------------------------------------------- textures
// Texture stages, d3es-style (GL_SelectTexture / Bind): a selected stage,
// per stage the name bound per target and which target is enabled. Uploads
// (GLTexUpload) go to the name bound on the selected stage; draws sample the
// enabled target of each stage, up to the first disabled stage.
enum TexTarget : uint8_t { TEX_NONE, TEX_2D, TEX_CUBE };
void SelectTexture(int u);
int SelectedTexture();
void BindTexture(TexTarget t, unsigned name);        // on the selected stage
unsigned BoundTexture(TexTarget t);                  // on the selected stage
void EnableTexture(TexTarget t, bool on);            // on the selected stage
void SetTexture(int u, TexTarget t, unsigned name);  // bind + enable t on stage u (helper)
void TextureDeleted(unsigned name);
void TextureWillChange(unsigned name);               // pending draws using it are played back first
// Stage combiner, engine EColorOp / EColorArg codes (CryCommon/IShader.h).
void SetStageOps(int u, uint8_t colorOp, uint8_t alphaOp, uint8_t colorArg, uint8_t alphaArg);
void SetStageScale(int u, float rgbScale, float alphaScale);
void SetStageColor(int u, const float rgba[4]);

// ---------------------------------------------------------------- transforms
// Matrix stacks as the engine uses them (like the D3D9 renderer's D3DX
// matrix stacks): model-view, projection, and a texture matrix per stage.
// Column-major 4x4; draws use the tops. CPU only.
enum MatrixMode : uint8_t { MAT_MODELVIEW, MAT_PROJECTION, MAT_TEXTURE };
void MatrixModeSet(MatrixMode m);
void LoadIdentity();
void LoadMatrix(const float m[16]);
void MultMatrix(const float m[16]);
void PushMatrix();
void PopMatrix();
void Ortho(double l, double r, double b, double t, double n, double f);
void Translate(float x, float y, float z);
void Rotate(float deg, float x, float y, float z);
void Scale(float x, float y, float z);
const float* ModelView();
const float* Projection();
const float* TextureMatrix(int stage);

// ---------------------------------------------------------------- fog
void Fogi(unsigned pname, int v);             // GL_FOG_MODE
void Fogf(unsigned pname, float v);           // GL_FOG_START / END / DENSITY
void Fogfv(unsigned pname, const float* v);   // GL_FOG_COLOR

// ---------------------------------------------------------------- lighting and texgen (GL semantics)
// Light positions and eye planes are transformed by the model-view matrix
// current at the call, as GL specifies. Enables: Enable(GL_LIGHTING /
// GL_LIGHTi / GL_COLOR_MATERIAL / GL_TEXTURE_GEN_x).
void TexGeni(unsigned coord, unsigned pname, int v);
void TexGenfv(unsigned coord, unsigned pname, const float* v);
void Lightfv(unsigned light, unsigned pname, const float* v);
void Lightf(unsigned light, unsigned pname, float v);
void Materialfv(unsigned face, unsigned pname, const float* v);
void LightModelfv(unsigned pname, const float* v);
void ColorMaterial(unsigned face, unsigned mode);

// ---------------------------------------------------------------- current vertex attributes
void SetColor(const float rgba[4]);
void SetTexCoord(int u, float s, float t, float r, float q);
void SetNormal(float x, float y, float z);

// ---------------------------------------------------------------- drawing
enum Prim : uint8_t { PRIM_TRIANGLES, PRIM_TRIANGLE_STRIP, PRIM_TRIANGLE_FAN, PRIM_LINES, PRIM_LINE_STRIP, PRIM_POINTS, PRIM_LINE_LOOP };
enum Attr : uint8_t { ATTR_POS, ATTR_COLOR, ATTR_NORMAL, ATTR_TEX0, ATTR_TEX1, ATTR_TEX2, ATTR_TEX3, ATTR_TEX4, ATTR_TEX5, ATTR_TEX6, ATTR_TEX7,
                      ATTR_COLOR1, ATTR_COUNT };   // texcoord unit u: ATTR_TEX0 + u
enum CompType : uint8_t { CT_FLOAT, CT_UBYTE, CT_SHORT };
struct AttribDesc { uint8_t attr, size, type, normalized; uint16_t offset; };
struct Layout { uint16_t stride; uint8_t count; AttribDesc attrib[ATTR_COUNT]; };
int RegisterLayout(const Layout& l);
// Colour stream semantics: 0 colour, 1 terrain brightness in alpha (x2),
// 2 vegetation light (x2).
// Client-memory geometry: copied into the frame ring.
void DrawClient(Prim p, int layout, const void* verts, int numVerts, const uint16_t* idx, int numIdx, int colorSemantic = 0);

// Immediate mode (glBegin/glVertex/glEnd), like Quake 3's tessellator:
// vertices take the current colour and texcoords; End records one draw.
// GL primitive values; quads, quad strips and polygons become triangles.
void Begin(unsigned glPrim);
void Vertex(float x, float y, float z);
void End();

// ---------------------------------------------------------------- buffers (engine leaf buffers)
// GL semantics of the engine's buffer calls. An update to a buffer that
// recorded draws still use is recorded too (ordered, data copied), so those
// draws see the old contents; otherwise it goes to GL at once. Map returns
// CPU memory uploaded at Unmap (whole-buffer glBufferData: the driver
// orphans instead of waiting for the GPU). Targets: GL_ARRAY_BUFFER (0x8892),
// GL_ELEMENT_ARRAY_BUFFER (0x8893).
void GenBuffers(int n, unsigned* out);
void DeleteBuffers(int n, const unsigned* b);
void BindBuffer(unsigned target, unsigned b);
void BufferData(unsigned target, size_t size, const void* data, unsigned usage);
void BufferSubData(unsigned target, size_t offset, size_t size, const void* data);
void* MapBuffer(unsigned target);
bool UnmapBuffer(unsigned target);

// ---------------------------------------------------------------- vertex arrays (GL client-array semantics)
// Pointers refer to the buffer bound to GL_ARRAY_BUFFER at the call (offset),
// or to client memory when none is bound (copied into the frame ring at draw).
void ArrayPointer(Attr a, int size, unsigned glType, bool normalized, int stride, const void* ptr);
void EnableArray(Attr a, bool on);
void ClientActiveTexture(int u);                 // texcoord arrays go to ATTR_TEX0 + u
int ClientActiveTextureUnit();
// Colour semantics of the next colour array (port specials, see DrawClient).
void SetColorSemantic(int semantic);
// Draws with the current arrays. Index type GL_UNSIGNED_SHORT or GL_UNSIGNED_INT;
// indices: offset into the bound element buffer, or client memory.
void DrawElements(unsigned glMode, int count, unsigned glType, const void* indices);
void DrawRangeElements(unsigned glMode, unsigned start, unsigned end, int count, unsigned glType, const void* indices);
void DrawArrays(unsigned glMode, int first, int count);

// ---------------------------------------------------------------- ARB vertex/fragment programs
// The engine's Cg programs, translated by GLRCg (cgCreateProgram); the text
// passed here is GLRCg::MakeProgramString output. Program parameters are the
// GL "env" registers; draws snapshot them. Enable(GL_VERTEX_PROGRAM_ARB /
// GL_FRAGMENT_PROGRAM_ARB) switches them on.
void GenPrograms(int n, unsigned* out);
void DeletePrograms(int n, const unsigned* ids);
void BindProgram(unsigned target, unsigned id);
void ProgramString(unsigned target, const char* text, int len);
void ProgramEnvParameter(unsigned target, unsigned index, const float v[4]);
bool ProgramsSupported();
// End of a level load: build the programs this level used while playing last
// time (from the binary cache), so none is built during gameplay.
void WarmUp(const char* level);            // native mode with the shader tier enabled

// ---------------------------------------------------------------- render thread
// FARCRY_GLR_THREAD (default on in native mode): after the first frame a
// render thread owns the GL context; recorded frames are replayed there while
// the game thread records the next one. Every other GL call runs there too:
// Post() queues it (in order with the frames), Sync() runs it and waits.
bool Threaded();
bool OnRenderThread();                       // true when not threaded
void Post(std::function<void()> fn);
// Recorded into the current frame's command stream (runs on the render thread
// between the draws recorded before and after it); direct call when not threaded.
void Record(std::function<void()> fn);
void Sync(std::function<void()> fn);
// Frame end (GLSystem): the present work is posted after the frame's replay;
// the game thread may then record one more frame before it waits.
void PostFrameEnd(std::function<void()> present);
// glClipPlane: plane in object space of the current model-view (GL rules); plane 0 only.
void ClipPlane(int index, const float eq[4]);
// Texture work of the bridge (GLRBridge.h): direct when single-threaded,
// recorded (data copied) or synchronous when threaded.
void TexGenNames(int n, unsigned* out);
void TexDeleteNames(int n, const unsigned* tex);
void TexParam(unsigned tex, unsigned target, unsigned pname, int v);
void TexParamF(unsigned tex, unsigned target, unsigned pname, float v);
void TexImage2D(unsigned tex, unsigned target, int level, int internal, int w, int h, int border, unsigned format, unsigned type, const void* pixels);
void TexSubImage2D(unsigned tex, unsigned target, int level, int x, int y, int w, int h, unsigned format, unsigned type, const void* pixels);
void TexCompressedImage2D(unsigned tex, unsigned target, int level, unsigned internal, int w, int h, int border, int size, const void* data);
void TexCompressedSubImage2D(unsigned tex, unsigned target, int level, int x, int y, int w, int h, unsigned format, int size, const void* data);
void ReadPixels(int x, int y, int w, int h, unsigned format, unsigned type, void* pixels);
void CopyTexSubImage2D(unsigned tex, unsigned target, int level, int xo, int yo, int x, int y, int w, int h);
void CopyTexImage2D(unsigned tex, unsigned target, int level, unsigned internal, int x, int y, int w, int h, int border);
unsigned TexSyncCount();
void StartThread(void* sdlWindow, void* glContext);   // at the first frame end
void StopThread();                                    // renderer shutdown: GL returns to the caller

// ---------------------------------------------------------------- diagnostics
void SelfTest();                     // FARCRY_GLR_SELFTEST: a test pattern each frame
void TraceNextDraws(int n);
void SetDebugLabel(const char* shaderName);   // diagnostics: engine shader of following draws          // log the next n recorded draws (snapshot signal)
void FrameStats();
}
