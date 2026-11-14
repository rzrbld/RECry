// Real GLES sampling of the upload sequence used by generated spotlight cubes.
#include <SDL.h>
#include <GLES3/gl3.h>
#include "../../RenderDll/XRenderGLES3/GLTexUpload.h"
#include "../../RenderDll/XRenderGLES3/GLES3TexConvert.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
bool TexConv_Running(){return false;}
uint32_t TexConv_Etc2Format(uint32_t){return GL_COMPRESSED_RGB8_ETC2;}
size_t TexConv_Etc2Size(uint32_t,int,int){return 8;}
bool TexConv_TryCached(uint32_t,int,int,const void*,size_t,std::vector<uint8_t>&){return false;}
void TexConv_Placeholder(uint32_t,int,int,const void*,std::vector<uint8_t>&){}
void TexConv_Enqueue(uint32_t,uint32_t,uint32_t,int,uint32_t,int,int,const void*,size_t){}
bool TexConv_Pop(STexConvResult&){return false;}
size_t TexConv_Pending(){return 0;}
static void check(bool ok,const char* msg){if(!ok){fprintf(stderr,"FAIL: %s (GL 0x%x)\n",msg,glGetError());exit(1);}}
static GLuint shader(GLenum kind,const char* text){
 GLuint s=glCreateShader(kind);glShaderSource(s,1,&text,nullptr);glCompileShader(s);
 GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);check(ok,"shader compilation");return s;
}
int main(){
 SDL_SetHint(SDL_HINT_VIDEODRIVER,"offscreen");check(SDL_Init(SDL_INIT_VIDEO)==0,"SDL video");
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
 SDL_Window* w=SDL_CreateWindow("projected cube",0,0,16,16,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 SDL_GLContext ctx=w?SDL_GL_CreateContext(w):nullptr;check(ctx,"GLES context");
 GLuint vs=shader(GL_VERTEX_SHADER,"#version 300 es\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));gl_Position=vec4(p*2.-1.,0,1);}");
 GLuint fs=shader(GL_FRAGMENT_SHADER,"#version 300 es\nprecision highp float;uniform samplerCube cube;uniform vec3 direction;uniform float lod;out vec4 color;void main(){color=textureLod(cube,direction,lod);}");
 GLuint program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
 GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);check(ok,"shader link");glUseProgram(program);
 GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);glViewport(0,0,16,16);glDisable(GL_DITHER);
 GLTex::Backend be;be.bind=[](unsigned t,unsigned n){if(t>=GL_TEXTURE_CUBE_MAP_POSITIVE_X&&t<=GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)t=GL_TEXTURE_CUBE_MAP;glBindTexture(t,n);};
 const float directions[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
 for(bool decode:{false,true}){
  check(GLTex::Init(be,decode,false,2),"upload initialization");
  for(unsigned format:{0x83f0u,0x83f2u,0x83f3u}){
   GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_CUBE_MAP,tex);
   unsigned char block[16]={};int offset=format==0x83f0?0:8;int bytes=offset+8;
   if(format==0x83f2)memset(block,255,8);
   if(format==0x83f3)block[0]=255;
   block[offset]=block[offset+1]=block[offset+2]=block[offset+3]=255;
   // First the engine replicates the source image, then replaces five faces
   // with raw black pixels. All mip levels must agree on storage format.
   for(int face=0;face<6;++face)for(int level=0;level<3;++level)
    GLTex::CompressedImage2D(tex,GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,level,format,4>>level,4>>level,0,bytes,block);
   unsigned char black[4*4*4]={};
   for(int face=1;face<6;++face)for(int level=0;level<3;++level)
    GLTex::Image2D(tex,GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,level,format,4>>level,4>>level,0,0x80e1,GL_UNSIGNED_BYTE,black);
   GLTex::Param(tex,GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_NEAREST);
   GLTex::Param(tex,GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
   GLTex::Param(tex,GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAX_LEVEL,2);
   for(int face=0;face<6;++face)for(int level=0;level<3;++level){
    glUniform3fv(glGetUniformLocation(program,"direction"),1,directions[face]);glUniform1f(glGetUniformLocation(program,"lod"),(float)level);
    glDrawArrays(GL_TRIANGLES,0,3);unsigned char pixel[4];glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    for(int c=0;c<3;++c)check(face==0?pixel[c]>=250:pixel[c]==0,"spotlight source is visible and generated faces stay black at every mip");
   }
   GLenum error=glGetError();if(error)fprintf(stderr,"cube test decode=%d format=%x error=%x\n",decode,format,error);check(error==GL_NO_ERROR,"valid cube uploads");GLTex::DeleteNames(1,&tex);
  }
 }
 glDeleteVertexArrays(1,&vao);glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(fs);
 SDL_GL_DeleteContext(ctx);SDL_DestroyWindow(w);SDL_Quit();
 puts("PASS: mixed DXT/raw spotlight cube faces and mip levels sample correctly with native and software DXT paths");
}
