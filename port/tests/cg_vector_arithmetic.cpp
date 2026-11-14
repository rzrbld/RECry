// Compile and render translated Cg vector arithmetic on a real GLES context.
// Run from the repository root: python3 port/tests/cg_vector_arithmetic.py
#include <SDL.h>
#include <GLES3/gl3.h>
#include "../../RenderDll/XRenderGLES3/GLRCg.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>

static GLuint shader(GLenum type, const std::string& text)
{
  GLuint s=glCreateShader(type); const char* p=text.c_str(); glShaderSource(s,1,&p,nullptr); glCompileShader(s);
  GLint ok; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
  if(!ok) { char log[8192]; glGetShaderInfoLog(s,sizeof(log),nullptr,log); fprintf(stderr,"%s\n%s",log,p); exit(1); }
  return s;
}
int main()
{
  SDL_SetHint(SDL_HINT_VIDEODRIVER,"offscreen");
  if(SDL_Init(SDL_INIT_VIDEO)) return 1;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
  SDL_Window* w=SDL_CreateWindow("vector arithmetic",0,0,16,16,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
  SDL_GLContext context=w?SDL_GL_CreateContext(w):nullptr;
  if(!context) { fprintf(stderr,"%s\n",SDL_GetError()); return 1; }
  GLuint vertex=shader(GL_VERTEX_SHADER,"#version 300 es\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));gl_Position=vec4(p*2.0-1.0,0,1);}");
  // Expected values calculated component by component. Nonzero discarded W
  // components catch accidental broadcasting/padding rather than truncation.
  struct Test { const char* expression; float rgb[3]; } tests[]={
    {"(A+B)*A",{.06f,.24f,.54f}},
    {"(B+A)*A",{.06f,.24f,.54f}},
    {"A*B+A",{.22f,.48f,.78f}},
    {"(A-B)/A",{.5f,.5f,.5f}},
    {"saturate(2*(A+B)*A)*A",{.024f,.192f,.6f}},
    {"float3(dot(A,A))",{1.2f,1.2f,1.2f}}, // reductions must keep all four inputs
    {"float3((A.xy+B.xy)*A.xy,0.5)",{.06f,.24f,.5f}},
  };
  for(const auto& test:tests)
  {
    const std::string cg=std::string("struct pixout { float4 Color : COLOR; };\npixout main(uniform float4 A : register(c0), uniform float3 B : register(c1)) : COLOR { pixout OUT; float3 result = ")+test.expression+"; OUT.Color=float4(result,1); return OUT; }";
    GLRCg::Program translated;
    if(!GLRCg::Translate(cg,false,translated)) { fprintf(stderr,"%s\n",translated.error.c_str()); return 1; }
    GLuint fragment=shader(GL_FRAGMENT_SHADER,translated.glsl), program=glCreateProgram();
    glAttachShader(program,vertex); glAttachShader(program,fragment); glLinkProgram(program);
    GLint ok; glGetProgramiv(program,GL_LINK_STATUS,&ok);
    if(!ok) { char log[4096]; glGetProgramInfoLog(program,sizeof(log),nullptr,log); fprintf(stderr,"%s\n",log); return 1; }
    glUseProgram(program);
    const float registers[]={.2f,.4f,.6f,.8f, .1f,.2f,.3f,.9f};
    glUniform4fv(glGetUniformLocation(program,"Cf[0]"),2,registers);
    GLuint vao; glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glViewport(0,0,16,16); glDisable(GL_DITHER); glDrawArrays(GL_TRIANGLES,0,3);
    unsigned char pixel[4]; glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    for(int c=0;c<3;++c)
    {
      float expected=fminf(1.f,fmaxf(0.f,test.rgb[c]))*255;
      if(fabsf(pixel[c]-expected)>1.5f) { fprintf(stderr,"%s channel %d: got %u expected %.2f\n",test.expression,c,pixel[c],expected); return 1; }
    }
    if(glGetError()!=GL_NO_ERROR) return 1;
    glDeleteVertexArrays(1,&vao); glDeleteProgram(program); glDeleteShader(fragment);
  }
  glDeleteShader(vertex); SDL_GL_DeleteContext(context); SDL_DestroyWindow(w); SDL_Quit();
  puts("PASS: Cg mixed vector arithmetic GPU pixels, operand order, precedence, saturation, swizzles and dot reduction");
}
