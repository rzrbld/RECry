/* GLES3 renderer build: the vendored Cg headers include <GL/gl.h> only for GL
 * types. Use the renderer's own desktop GL definitions (same include guard)
 * instead of depending on a system desktop OpenGL header. */
#include "../../../RenderDll/XRenderGLES3/Mygl.h"
