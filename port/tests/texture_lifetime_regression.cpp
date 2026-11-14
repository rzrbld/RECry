// Used by port/tests/texture_lifetime_regression.py against production GLTexUpload.
#include <SDL.h>
#include <GLES3/gl3.h>
#include "../../RenderDll/XRenderGLES3/GLTexUpload.h"
#include "../../RenderDll/XRenderGLES3/GLES3TexConvert.h"
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>
// This test isolates allocation and ordinary streaming updates. Conversion
// workers are disabled; no fake worker results are used as conversion evidence.
bool TexConv_Running(){return false;}
uint32_t TexConv_Etc2Format(uint32_t){return GL_COMPRESSED_RGB8_ETC2;}
size_t TexConv_Etc2Size(uint32_t,int,int){return 8;}
bool TexConv_TryCached(uint32_t,int,int,const void*,size_t,std::vector<uint8_t>&){return false;}
void TexConv_Placeholder(uint32_t,int,int,const void*,std::vector<uint8_t>&){}
void TexConv_Enqueue(uint32_t,uint32_t,uint32_t,int,uint32_t,int,int,const void*,size_t){}
bool TexConv_Pop(STexConvResult&){return false;}
size_t TexConv_Pending(){return 0;}
namespace GLR { void NoteTextureTarget(unsigned,unsigned); unsigned TextureTarget(unsigned); }
bool EngineRejectsTexture(int);
static void check(bool ok,const char* message){if(!ok){fprintf(stderr,"FAIL: %s (GL 0x%x)\n",message,glGetError());exit(1);}}
int main(){
 SDL_SetHint(SDL_HINT_VIDEODRIVER,"offscreen");check(SDL_Init(SDL_INIT_VIDEO)==0,"SDL initialization");
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
 SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
 SDL_Window* w=SDL_CreateWindow("texture lifetime",0,0,16,16,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 auto ctx=w?SDL_GL_CreateContext(w):nullptr;check(ctx!=nullptr,"GLES context");
 GLTex::Backend be;be.bind=[](unsigned t,unsigned n){glBindTexture(t==0x84f5?GL_TEXTURE_2D:t,n);};
 check(GLTex::Init(be,false,false,2),"texture module initialization");
 const unsigned count=5000;std::vector<unsigned> names(count);std::set<unsigned> unique;
 GLTex::GenNames(count,names.data(),false); // same allocator path as threaded renderer
 for(unsigned i=0;i<count;++i){
  auto name=names[i];check(unique.insert(name).second,"unique generated names");
  check(name<0xfa0 || name>=0x4000,"generated names must not collide with engine reserved names");
  check(!EngineRejectsTexture((int)name),"engine must accept generated texture names");
  GLR::NoteTextureTarget(name,GL_TEXTURE_2D);check(GLR::TextureTarget(name)==GL_TEXTURE_2D,"large-name target metadata");
  unsigned char pixel[4]={(unsigned char)i,(unsigned char)(i>>8),73,255};
  GLTex::Image2D(name,GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
 }
 // A fixed render target first appears after thousands of allocations.
 GLTex::NoteBound(0xfff);GLR::NoteTextureTarget(0xfff,0x84f5);
 unsigned char fixed[4]={9,99,199,255};GLTex::Image2D(0xfff,GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,fixed);
 GLuint fbo;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
 for(unsigned i=0;i<count;i+=37){
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,names[i],0);
  check(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"texture attachment");
  unsigned char pixel[4];glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
  check(pixel[0]==(i&255)&&pixel[1]==(i>>8)&&pixel[2]==73,"texture contents after long allocation sequence");
 }
 // Reuse names and update them repeatedly as terrain sectors are recycled.
 for(int round=0;round<10;++round){
  const unsigned old=names[round];GLTex::DeleteNames(1,&old);GLTex::GenNames(1,&names[round],false);
  check(!EngineRejectsTexture((int)names[round]),"recycled name accepted");
  GLR::NoteTextureTarget(names[round],0x84f5);check(GLR::TextureTarget(names[round])==0x84f5,"recycled rectangle target");
  GLR::NoteTextureTarget(names[round],GL_TEXTURE_2D);
  GLTex::Image2D(names[round],GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,fixed);
  unsigned char sand[4]={212,180,110,255};GLTex::SubImage2D(names[round],GLR::TextureTarget(names[round]),0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,sand);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,names[round],0);
  unsigned char pixel[4];glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
  check(pixel[0]==212&&pixel[1]==180&&pixel[2]==110,"recycled terrain update reaches intended texture");
 }
 check(glGetError()==GL_NO_ERROR,"no GL errors");
 GLTex::DeleteNames(count,names.data());glDeleteFramebuffers(1,&fbo);
 SDL_GL_DeleteContext(ctx);SDL_DestroyWindow(w);SDL_Quit();
 puts("PASS: 5000 texture identities, late fixed target, GPU contents and recycled terrain updates");
}
