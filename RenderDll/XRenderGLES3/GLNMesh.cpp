/*=============================================================================
  GLNMesh.cpp : native mesh draws (stage "mesh"). See GLNMesh.h.

  Texture-stage semantics follow CGLRenderer::EF_SetColorOp: each stage
  combines up to three arguments (texture, vertex colour, previous stage,
  stage constant) with modulate (x1/x2/x4), replace, add, add-signed or an
  interpolation by vertex/texture alpha; alpha likewise. Lighting (per
  vertex), texgen and texture matrices follow the fixed-function semantics
  the translation layer implements (GLES3FFP.cpp BuildProgram, the
  reference for the exact GLSL).
=============================================================================*/
#include "GLNativeCore.h"
#include "GLNMesh.h"
#include <map>
#include <time.h>
#include <unordered_map>

const char* GLNMeshCurrentShader();   // GLRendPipeline.cpp: current engine shader name

namespace
{
// engine codes (CryCommon/IShader.h)
enum { CO_NOSET, CO_DISABLE, CO_REPLACE, CO_DECAL, CO_ARG2, CO_MODULATE, CO_MODULATE2X, CO_MODULATE4X,
       CO_BLENDDIFFUSEALPHA, CO_BLENDTEXTUREALPHA, CO_DETAIL, CO_ADD, CO_ADDSIGNED };
enum { CA_SPECULAR, CA_TEXTURE, CA_DIFFUSE, CA_PREVIOUS, CA_CONSTANT };

#pragma pack(push, 1)
struct Key
{
  uint8_t stages;
  uint8_t co[4], ao[4], ca[4], aa[4];
  uint8_t colorArray, colorSemantic, alphaTest, fog;
  uint8_t texStream;     // bit u: stage u has a texcoord stream (else the current coordinate, 0,0)
  uint8_t lights;        // 0 unlit, else 1 + number of lights
  uint8_t colorMaterial;
  uint8_t gen[4][4];     // per stage and s/t/r/q: 0 none 1 object 2 eye 3 sphere 4 reflection 5 normal
  uint8_t texMat;        // bit u: texture matrix on stage u
  uint8_t cube;          // bit u: stage u samples a cube map
};
#pragma pack(pop)

bool OpSupported(uint8_t op)
{
  switch (op)
  {
    case CO_DISABLE: case CO_REPLACE: case CO_DECAL: case CO_MODULATE: case CO_MODULATE2X: case CO_MODULATE4X:
    case CO_BLENDDIFFUSEALPHA: case CO_BLENDTEXTUREALPHA: case CO_ADD: case CO_ADDSIGNED: return true;
  }
  return false;
}

std::string Src(int arg, int u, bool alpha)
{
  char b[32];
  switch (arg)
  {
    case CA_TEXTURE: snprintf(b, sizeof(b), "t%d", u); break;
    case CA_DIFFUSE: snprintf(b, sizeof(b), "vColor"); break;
    case CA_CONSTANT: snprintf(b, sizeof(b), "uConst%d", u); break;
    case CA_SPECULAR: case CA_PREVIOUS: default: snprintf(b, sizeof(b), "prev"); break;
  }
  return std::string(b) + (alpha ? ".a" : ".rgb");
}

std::string Combine(int op, int args, int u, bool alpha)
{
  const std::string a0 = Src(args & 7, u, alpha), a1 = Src((args >> 3) & 7, u, alpha);
  switch (op)
  {
    case CO_REPLACE: case CO_DECAL: case CO_DISABLE: return a0;
    case CO_ADD: return a0 + " + " + a1;
    case CO_ADDSIGNED: return a0 + " + " + a1 + (alpha ? " - 0.5" : " - vec3(0.5)");
    case CO_BLENDDIFFUSEALPHA:
      if (alpha) return a0 + " * " + a1;   // the engine leaves the alpha combiner at modulate
      return "mix(" + a1 + ", " + a0 + ", vColor.a)";
    case CO_BLENDTEXTUREALPHA:
      if (alpha) return a0 + " * " + a1;
      { char b[16]; snprintf(b, sizeof(b), "t%d.a", u); return "mix(" + a1 + ", " + a0 + ", " + b + ")"; }
    case CO_MODULATE: case CO_MODULATE2X: case CO_MODULATE4X: default: return a0 + " * " + a1;
  }
}

bool NeedsNormal(const Key& k)
{
  if (k.lights) return true;
  for (int u = 0; u < k.stages; ++u) for (int c = 0; c < 4; ++c) if (k.gen[u][c] >= 3) return true;
  return false;
}

std::string BuildVS(const Key& k)
{
  const bool nrm = NeedsNormal(k);
  std::string vs = "#version 300 es\nin vec4 aPos; in vec4 aColor; in vec3 aNormal; in vec4 aTex0; in vec4 aTex1; in vec4 aTex2; in vec4 aTex3;\n"
                   "uniform mat4 uMV; uniform mat4 uP; uniform mat3 uNormalMat; uniform vec4 uCurColor; uniform vec4 uCurTex[4];\n"
                   "out mediump vec4 vColor; out highp float vFogDist;\n";
  char b[256];
  if (k.lights)
  {
    snprintf(b, sizeof(b), "uniform vec4 uLightPos[%d]; uniform vec4 uLightAmb[%d]; uniform vec4 uLightDiff[%d]; uniform vec3 uLightAtt[%d];\n",
             k.lights - 1, k.lights - 1, k.lights - 1, k.lights - 1);
    vs += b;
    vs += "uniform vec4 uMatAmb; uniform vec4 uMatDiff; uniform vec4 uMatEmis; uniform vec4 uSceneAmb;\n";
  }
  for (int u = 0; u < k.stages; ++u)
  {
    snprintf(b, sizeof(b), "out highp vec4 vTex%d; uniform mat4 uGenObj%d; uniform mat4 uGenEye%d; uniform mat4 uTexMat%d;\n", u, u, u, u);
    vs += b;
  }
  vs += "void main() {\n  vec4 eye = uMV * aPos;\n  gl_Position = uP * eye;\n  vFogDist = abs(eye.z);\n";
  if (!k.colorArray) vs += "  vec4 inColor = uCurColor;\n";
  else if (k.colorSemantic == 1) vs += "  vec4 inColor = vec4(aColor.aaa * 2.0, 1.0);\n";
  else if (k.colorSemantic == 2) vs += "  vec4 inColor = vec4(min(aColor.rgb * 2.0, vec3(1.0)), 1.0);\n";
  else vs += "  vec4 inColor = aColor;\n";
  if (nrm) vs += "  vec3 N = normalize(uNormalMat * aNormal);\n";
  if (k.lights)
  {
    vs += k.colorMaterial ? "  vec4 mAmb = inColor; vec4 mDiff = inColor;\n" : "  vec4 mAmb = uMatAmb; vec4 mDiff = uMatDiff;\n";
    vs += "  vec3 c = uMatEmis.rgb + uSceneAmb.rgb * mAmb.rgb;\n";
    if (k.lights > 1)
    {
      snprintf(b, sizeof(b), "  for (int i = 0; i < %d; ++i) {\n", k.lights - 1); vs += b;
      vs += "    vec3 L; float att = 1.0;\n"
            "    if (uLightPos[i].w == 0.0) L = normalize(uLightPos[i].xyz);\n"
            "    else { vec3 d = uLightPos[i].xyz - eye.xyz; float dl = length(d); L = d / max(dl, 1e-6);\n"
            "           att = 1.0 / max(uLightAtt[i].x + uLightAtt[i].y * dl + uLightAtt[i].z * dl * dl, 1e-6); }\n"
            "    c += att * (uLightAmb[i].rgb * mAmb.rgb + max(dot(N, L), 0.0) * uLightDiff[i].rgb * mDiff.rgb);\n"
            "  }\n";
    }
    vs += "  vColor = vec4(clamp(c, 0.0, 1.0), mDiff.a);\n";
  }
  else
    vs += "  vColor = inColor;\n";
  static const char comp[4] = { 'x', 'y', 'z', 'w' };
  for (int u = 0; u < k.stages; ++u)
  {
    if (k.texStream & (1 << u)) snprintf(b, sizeof(b), "  vec4 tc%d = aTex%d;\n", u, u);
    else snprintf(b, sizeof(b), "  vec4 tc%d = uCurTex[%d];   // no array: the current texcoord\n", u, u);
    vs += b;
    bool anyGen = false;
    for (int c = 0; c < 4; ++c) if (k.gen[u][c]) anyGen = true;
    if (anyGen) vs += nrm ? "  { vec3 R = reflect(normalize(eye.xyz), N);\n" : "  {\n";
    for (int c = 0; c < 4 && anyGen; ++c)
    {
      switch (k.gen[u][c])
      {
        case 1: snprintf(b, sizeof(b), "    tc%d.%c = dot(aPos, uGenObj%d[%d]);\n", u, comp[c], u, c); break;
        case 2: snprintf(b, sizeof(b), "    tc%d.%c = dot(eye, uGenEye%d[%d]);\n", u, comp[c], u, c); break;
        case 3: snprintf(b, sizeof(b), "    { float m = 2.0 * sqrt(R.x*R.x + R.y*R.y + (R.z+1.0)*(R.z+1.0)); tc%d.%c = %s; }\n",
                         u, comp[c], c == 0 ? "R.x / m + 0.5" : (c == 1 ? "R.y / m + 0.5" : "0.0")); break;
        case 4: snprintf(b, sizeof(b), "    tc%d.%c = R.%c;\n", u, comp[c], comp[c < 3 ? c : 2]); break;
        case 5: snprintf(b, sizeof(b), "    tc%d.%c = N.%c;\n", u, comp[c], comp[c < 3 ? c : 2]); break;
        default: b[0] = 0;
      }
      vs += b;
    }
    if (anyGen) vs += "  }\n";
    if (k.texMat & (1 << u)) { snprintf(b, sizeof(b), "  tc%d = uTexMat%d * tc%d;\n", u, u, u); vs += b; }
    snprintf(b, sizeof(b), "  vTex%d = tc%d;\n", u, u); vs += b;
  }
  vs += "}\n";
  return vs;
}

std::string BuildFS(const Key& k)
{
  std::string fs = "#version 300 es\nprecision mediump float;\nin mediump vec4 vColor; in highp float vFogDist;\n";
  char b[256];
  for (int u = 0; u < k.stages; ++u)
  {
    snprintf(b, sizeof(b), "in highp vec4 vTex%d; uniform %s uTex%d; uniform vec4 uConst%d; uniform vec2 uScale%d;\n",
             u, (k.cube & (1 << u)) ? "samplerCube" : "sampler2D", u, u, u);
    fs += b;
  }
  fs += "uniform vec4 uFog; uniform vec3 uFogColor; uniform float uAlphaRef;\nout vec4 oColor;\nvoid main() {\n  vec4 prev = vColor;\n";
  for (int u = 0; u < k.stages; ++u)
  {
    if (k.cube & (1 << u)) snprintf(b, sizeof(b), "  vec4 t%d = texture(uTex%d, vTex%d.xyz);\n", u, u, u);
    else snprintf(b, sizeof(b), "  vec4 t%d = textureProj(uTex%d, vTex%d.xyw);\n", u, u, u);
    fs += b;
    fs += "  {\n    vec3 r = " + Combine(k.co[u], k.ca[u], u, false) + ";\n";
    fs += "    float a = " + Combine(k.ao[u], k.aa[u], u, true) + ";\n";
    snprintf(b, sizeof(b), "    prev = clamp(vec4(r * uScale%d.x, a * uScale%d.y), 0.0, 1.0);\n  }\n", u, u); fs += b;
  }
  static const char* const cmp[4] = { "true", "prev.a > uAlphaRef", "prev.a < uAlphaRef", "prev.a >= uAlphaRef" };
  if (k.alphaTest) { fs += "  if (!("; fs += cmp[k.alphaTest & 3]; fs += ")) discard;\n"; }
  // uFog: x start, y end, z 1/(end-start), w density
  if (k.fog == 1) fs += "  float f = clamp((uFog.y - vFogDist) * uFog.z, 0.0, 1.0);\n";
  else if (k.fog == 2) fs += "  float f = clamp(exp(-uFog.w * vFogDist), 0.0, 1.0);\n";
  else if (k.fog == 3) fs += "  float f = clamp(exp(-(uFog.w * vFogDist) * (uFog.w * vFogDist)), 0.0, 1.0);\n";
  if (k.fog) fs += "  prev.rgb = mix(uFogColor, prev.rgb, f);\n";
  static const bool s_flat = getenv("FARCRY_NATIVE_FLATFS") != NULL;   // diagnostic: vertex colour only
  fs += s_flat ? "  oColor = vColor;\n}\n" : "  oColor = prev;\n}\n";
  return fs;
}

struct Prog
{
  GLN::Program p; bool ok = false;
  GLint uMV, uP, uNormalMat, uFog, uFogColor, uAlphaRef, uTex[4], uConst[4], uScale[4], uGenObj[4], uGenEye[4], uTexMat[4];
  GLint uLightPos, uLightAmb, uLightDiff, uLightAtt, uMatAmb, uMatDiff, uMatEmis, uSceneAmb, uCurColor, uCurTex;
  // last uploaded values: skip unchanged uniforms
  float mv[16], proj[16], nm[9], fog[4], fogColor[3], alphaRef, cnst[4][4], scale[4][2];
  bool fresh = true;
};
std::unordered_map<std::string, Prog*> s_progs;
Key s_lastKey; Prog* s_lastProg = nullptr;

Prog* GetProgram(const Key& k)
{
  if (s_lastProg && !memcmp(&k, &s_lastKey, sizeof(k))) return s_lastProg;
  const std::string key((const char*)&k, sizeof(k));
  auto it = s_progs.find(key);
  if (it != s_progs.end()) { s_lastKey = k; s_lastProg = it->second; return it->second; }
  Prog* p = new Prog();
  p->ok = p->p.Build("mesh", BuildVS(k), BuildFS(k));
  if (getenv("FARCRY_NATIVE_DUMP_SHADERS"))
  {
    fprintf(stderr, "GLNATIVE PROGRAM for %s\n--- VS\n%s--- FS\n%s--- END\n", GLNMeshCurrentShader(), BuildVS(k).c_str(), BuildFS(k).c_str());
  }
  if (p->ok)
  {
    GLN::Program& g = p->p;
    p->uMV = g.Uniform("uMV"); p->uP = g.Uniform("uP"); p->uNormalMat = g.Uniform("uNormalMat");
    p->uFog = g.Uniform("uFog"); p->uFogColor = g.Uniform("uFogColor"); p->uAlphaRef = g.Uniform("uAlphaRef");
    p->uLightPos = g.Uniform("uLightPos"); p->uLightAmb = g.Uniform("uLightAmb"); p->uLightDiff = g.Uniform("uLightDiff");
    p->uLightAtt = g.Uniform("uLightAtt"); p->uMatAmb = g.Uniform("uMatAmb"); p->uMatDiff = g.Uniform("uMatDiff");
    p->uMatEmis = g.Uniform("uMatEmis"); p->uSceneAmb = g.Uniform("uSceneAmb");
    p->uCurColor = g.Uniform("uCurColor"); p->uCurTex = g.Uniform("uCurTex");
    char b[16];
    g.Use();
    for (int u = 0; u < 4; ++u)
    {
      snprintf(b, sizeof(b), "uTex%d", u); p->uTex[u] = g.Uniform(b);
      if (p->uTex[u] >= 0) GLN::gl.glUniform1i(p->uTex[u], u);
      snprintf(b, sizeof(b), "uConst%d", u); p->uConst[u] = g.Uniform(b);
      snprintf(b, sizeof(b), "uScale%d", u); p->uScale[u] = g.Uniform(b);
      snprintf(b, sizeof(b), "uGenObj%d", u); p->uGenObj[u] = g.Uniform(b);
      snprintf(b, sizeof(b), "uGenEye%d", u); p->uGenEye[u] = g.Uniform(b);
      snprintf(b, sizeof(b), "uTexMat%d", u); p->uTexMat[u] = g.Uniform(b);
    }
  }
  s_progs[key] = p;
  s_lastKey = k; s_lastProg = p;
  return p;
}

template <size_t N> inline bool Changed(float (&dst)[N], const float* src) { if (!memcmp(dst, src, sizeof(dst))) return false; memcpy(dst, src, sizeof(dst)); return true; }

unsigned s_native = 0, s_fallback = 0;
// FARCRY_NATIVE_PROFILE=1: time spent per step of native draws (diagnostic)
bool s_prof = false; double s_t[6] = {};
std::map<std::string, double> s_gpuByShader;
inline double NowMs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
std::map<std::string, unsigned> s_reasons;
} // namespace

GLNMeshStream g_GLNTexStream[4];
GLNMeshStream g_GLNPosStream, g_GLNColorStream, g_GLNNormalStream;
int g_GLNColorSemantic = 0;
unsigned g_GLNIndexBuffer = 0;

void GLNMesh_Fallback(const char* reason) { ++s_fallback; ++s_reasons[reason]; }

bool GLNMesh_Draw(const GLNMeshDraw& dIn, int count, unsigned idxOffset)
{
  if (!GLN::Enabled(GLN_STAGE_MESH)) return false;
  // Streams come from the layer's array state: engine paths set vertex
  // pointers in several places (SArrayPointer_*, the flush path, DrawBuffer).
  GLNMeshDraw d = dIn;
  float curColor[4], curTex[4][4];
  if (!GLES3FFPGetArrays(&d, curColor, curTex)) { GLNMesh_Fallback("client-memory data"); return false; }
  if (!d.pos.buffer || !d.indexBuffer) { GLNMesh_Fallback("client-memory data"); return false; }
  if (d.numStages > 4) { GLNMesh_Fallback("more than 4 stages"); return false; }
  if (GLES3FFPHasEnvOverride()) { GLNMesh_Fallback("layer env override"); return false; }
  // Diagnostic: FARCRY_NATIVE_MESH_SKIP=<part of a shader name>[,...] keeps those draws on the layer.
  static const char* s_skip = getenv("FARCRY_NATIVE_MESH_SKIP");
  if (s_skip && *s_skip)
  {
    const char* sh = GLNMeshCurrentShader();
    for (const char* p0 = s_skip; *p0;)
    {
      const char* e = strchr(p0, ','); const size_t len = e ? (size_t)(e - p0) : strlen(p0);
      if (len && strstr(sh, std::string(p0, len).c_str())) { GLNMesh_Fallback("skipped by FARCRY_NATIVE_MESH_SKIP"); return false; }
      if (!e) break; p0 = e + 1;
    }
  }
  Key k; memset(&k, 0, sizeof(k));
  GLNTexGen tg[4];
  int n = 0;
  for (int u = 0; u < d.numStages; ++u)
  {
    const GLNMeshStage& s = d.stage[u];
    if (!s.target) break;                       // fixed function stops at the first disabled unit
    if (!OpSupported(s.colorOp) || !OpSupported(s.alphaOp)) { GLNMesh_Fallback("unsupported combiner op"); return false; }
    GLES3FFPGetTexGen(u, &tg[u]);
    if (s.target != GL_TEXTURE_2D && !tg[u].cube) { GLNMesh_Fallback("unsupported texture target"); return false; }
    if (tg[u].cube) k.cube |= (uint8_t)(1 << u);
    for (int c = 0; c < 4; ++c) k.gen[n][c] = tg[u].gen[c];
    if (tg[u].texMatOn) k.texMat |= (uint8_t)(1 << u);
    if (d.tex[u].buffer) k.texStream |= (uint8_t)(1 << u);
    k.co[n] = s.colorOp; k.ao[n] = s.alphaOp; k.ca[n] = s.colorArg; k.aa[n] = s.alphaArg;
    ++n;
  }
  k.stages = (uint8_t)n;
  k.colorArray = d.color.buffer ? 1 : 0;
  k.colorSemantic = (uint8_t)d.colorSemantic;
  k.alphaTest = (uint8_t)d.alphaTestMode;
  // Fog actually in effect (some engine paths change it with direct glFog
  // calls that the engine's m_FS does not track).
  int fogMode; float fogStart, fogEnd, fogDensity, fogColor[3];
  GLES3FFPGetFog(&fogMode, &fogStart, &fogEnd, &fogDensity, fogColor);
  k.fog = (uint8_t)fogMode;
  GLNLighting L;
  GLES3FFPGetLighting(&L);
  if (L.on) { k.lights = (uint8_t)(1 + L.count); k.colorMaterial = L.colorMaterial ? 1 : 0; }
  const bool nrm = NeedsNormal(k);
  if (nrm && !d.normal.buffer) { GLNMesh_Fallback("lighting/texgen without a normal stream"); return false; }
  Prog* p = GetProgram(k);
  if (!p->ok) { GLNMesh_Fallback("program build failed"); return false; }

  static const bool s_profOn = (s_prof = getenv("FARCRY_NATIVE_PROFILE") != NULL);
  (void)s_profOn;
  double t0 = s_prof ? NowMs() : 0;
  GLNSection section;
  // All streams of one leaf buffer share its VBO and stride; extra streams
  // (lightmap coordinates) live in other buffers: then bind attributes directly.
  // (interleaved: same buffer and stride, and inside one vertex; the layout
  // stores attribute offsets relative to the position as one byte)
  auto inVertex = [&d](const GLNMeshStream& s) {
    return s.buffer == d.pos.buffer && s.stride == d.pos.stride && s.offset >= d.pos.offset && s.offset - d.pos.offset < (unsigned)d.pos.stride && s.offset - d.pos.offset < 256;
  };
  bool singleBuffer = (!d.color.buffer || inVertex(d.color)) && (!nrm || inVertex(d.normal));
  for (int u = 0; u < n; ++u) singleBuffer = singleBuffer && (!d.tex[u].buffer || inVertex(d.tex[u]));
  // FARCRY_NATIVE_ONEVAO=1: one VAO re-pointed per draw (as the layer does) instead of cached VAOs.
  static const bool s_oneVao = getenv("FARCRY_NATIVE_ONEVAO") && atoi(getenv("FARCRY_NATIVE_ONEVAO"));
  if (s_oneVao) singleBuffer = false;
  if (singleBuffer)
  {
    GLN::Layout l; l.stride = (uint16_t)d.pos.stride;
    l.Add(GLN_POS, d.pos.size, d.pos.type, false, 0);
    if (d.color.buffer) l.Add(GLN_COLOR, d.color.size, d.color.type, true, (int)(d.color.offset - d.pos.offset));
    if (nrm) l.Add(GLN_NORMAL, 3, d.normal.type, false, (int)(d.normal.offset - d.pos.offset));
    for (int u = 0; u < n; ++u) if (d.tex[u].buffer) l.Add((GLNAttr)(GLN_TEX0 + u), d.tex[u].size, d.tex[u].type, false, (int)(d.tex[u].offset - d.pos.offset));
    GLN::BindVertexArray(l, d.pos.buffer, d.pos.offset, d.indexBuffer);
  }
  else
  {
    // FARCRY_NATIVE_ONEVAO=2: the default vertex array object 0, as the layer uses
    static GLuint s_vao = 0;
    static const bool s_vao0 = getenv("FARCRY_NATIVE_ONEVAO") && atoi(getenv("FARCRY_NATIVE_ONEVAO")) == 2;
    if (!s_vao && !s_vao0) GLN::gl.glGenVertexArrays(1, &s_vao);
    GLN::gl.glBindVertexArray(s_vao0 ? 0 : s_vao);
    auto bind = [](int attr, const GLNMeshStream& s, int size, bool norm) {
      GLN::gl.glBindBuffer(GL_ARRAY_BUFFER, s.buffer);
      GLN::gl.glVertexAttribPointer(attr, size, s.type, norm ? GL_TRUE : GL_FALSE, s.stride, (const void*)(size_t)s.offset);
      GLN::gl.glEnableVertexAttribArray(attr);
    };
    for (int a = 0; a < GLN_NUM_ATTRS; ++a) GLN::gl.glDisableVertexAttribArray(a);
    bind(GLN_POS, d.pos, d.pos.size, false);
    if (d.color.buffer) bind(GLN_COLOR, d.color, d.color.size, true);
    if (nrm) bind(GLN_NORMAL, d.normal, 3, false);
    for (int u = 0; u < n; ++u) if (d.tex[u].buffer) bind(GLN_TEX0 + u, d.tex[u], d.tex[u].size, false);
  }
  GLN::gl.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indexBuffer);

  double t1 = s_prof ? NowMs() : 0;
  p->p.Use();
  const bool all = p->fresh; p->fresh = false;
  float mv[16], proj[16];
  GLES3FFPGetMatrices(mv, proj);
  if (Changed(p->mv, mv) || all) GLN::gl.glUniformMatrix4fv(p->uMV, 1, GL_FALSE, mv);
  if (Changed(p->proj, proj) || all) GLN::gl.glUniformMatrix4fv(p->uP, 1, GL_FALSE, proj);
  if (nrm && (Changed(p->nm, L.normalMat) || all)) GLN::gl.glUniformMatrix3fv(p->uNormalMat, 1, GL_FALSE, L.normalMat);
  if (!k.colorArray && p->uCurColor >= 0) GLN::gl.glUniform4fv(p->uCurColor, 1, curColor);
  if (p->uCurTex >= 0 && (k.texStream & ((1 << n) - 1)) != ((1 << n) - 1)) GLN::gl.glUniform4fv(p->uCurTex, 4, &curTex[0][0]);
  if (k.lights)
  {
    float pos[16], amb[16], diff[16], att[12];
    for (int i = 0; i < L.count; ++i) { memcpy(&pos[i * 4], L.pos[i], 16); memcpy(&amb[i * 4], L.amb[i], 16); memcpy(&diff[i * 4], L.diff[i], 16); memcpy(&att[i * 3], L.att[i], 12); }
    if (L.count)
    {
      GLN::gl.glUniform4fv(p->uLightPos, L.count, pos); GLN::gl.glUniform4fv(p->uLightAmb, L.count, amb);
      GLN::gl.glUniform4fv(p->uLightDiff, L.count, diff); GLN::gl.glUniform3fv(p->uLightAtt, L.count, att);
    }
    GLN::gl.glUniform4fv(p->uMatAmb, 1, L.matAmb); GLN::gl.glUniform4fv(p->uMatDiff, 1, L.matDiff);
    GLN::gl.glUniform4fv(p->uMatEmis, 1, L.matEmis); GLN::gl.glUniform4fv(p->uSceneAmb, 1, L.sceneAmb);
  }
  if (k.fog)
  {
    const float range = fogEnd - fogStart;
    const float fog[4] = { fogStart, fogEnd, range != 0 ? 1.0f / range : 0.0f, fogDensity };
    if (Changed(p->fog, fog) || all) GLN::gl.glUniform4fv(p->uFog, 1, fog);
    if (Changed(p->fogColor, fogColor) || all) GLN::gl.glUniform3fv(p->uFogColor, 1, fogColor);
  }
  if (k.alphaTest && (all || p->alphaRef != d.alphaRef)) { p->alphaRef = d.alphaRef; GLN::gl.glUniform1f(p->uAlphaRef, d.alphaRef); }
  double t2 = s_prof ? NowMs() : 0;
  for (int u = 0; u < n; ++u)
  {
    const GLNMeshStage& s = d.stage[u];
    GLN::gl.glActiveTexture(GL_TEXTURE0 + u);
    GLN::gl.glBindTexture((k.cube & (1 << u)) ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, tg[u].texture);
    // stage colour and scales as actually set (engine paths also call glTexEnv directly)
    if (Changed(p->cnst[u], tg[u].envColor) || all) GLN::gl.glUniform4fv(p->uConst[u], 1, tg[u].envColor);
    const float sc[2] = { tg[u].scaleRGB, tg[u].scaleA };
    if (Changed(p->scale[u], sc) || all) GLN::gl.glUniform2fv(p->uScale[u], 1, sc);
    bool gen = false; for (int c2 = 0; c2 < 4; ++c2) if (k.gen[u][c2]) gen = true;
    if (gen)
    {
      GLN::gl.glUniformMatrix4fv(p->uGenObj[u], 1, GL_FALSE, tg[u].obj);
      GLN::gl.glUniformMatrix4fv(p->uGenEye[u], 1, GL_FALSE, tg[u].eye);
    }
    if (k.texMat & (1 << u)) GLN::gl.glUniformMatrix4fv(p->uTexMat[u], 1, GL_FALSE, tg[u].texMat);
  }
  static const bool s_finish = getenv("FARCRY_NATIVE_FINISH") != NULL;   // diagnostic: GPU time per draw
  if (s_finish) GLN::gl.glFinish();
  double t3 = s_prof ? NowMs() : 0;
  static const bool s_noDraw = getenv("FARCRY_NATIVE_NODRAW") != NULL;   // diagnostic: all state work, no draw call
  if (!s_noDraw) GLN::gl.glDrawElements(d.primitive, count, GL_UNSIGNED_SHORT, (const void*)(size_t)idxOffset);
  if (s_finish)
  {
    GLN::gl.glFinish();
    const double ms = NowMs() - t3;
    s_gpuByShader[GLNMeshCurrentShader()] += ms;
    if (ms > 5.0) GLN::Log("GLNative slow draw: %.1f ms, shader %s, prim 0x%x, %d indices, %d stages, lights %d", ms, GLNMeshCurrentShader(), d.primitive, count, n, k.lights);
  }
  if (s_prof) { const double t4 = NowMs(); s_t[0] += t1 - t0; s_t[1] += t2 - t1; s_t[2] += t3 - t2; s_t[3] += t4 - t3; }
  ++s_native;
  return true;
}

void GLNMesh_FrameStats()
{
  static unsigned frames = 0;
  if (++frames % 30) return;
  if (s_native || s_fallback)
  {
    std::string r;
    for (auto& e : s_reasons) { char b[96]; snprintf(b, sizeof(b), " [%s: %u]", e.first.c_str(), e.second); r += b; }
    GLN::Log("GLNative mesh: last 30 frames %u native draws, %u on the layer%s", s_native, s_fallback, r.c_str());
    if (s_prof)
    {
      extern double g_GLNRestoreMs;
      GLN::Log("GLNative profile (ms per frame): vao %.2f, program+uniforms %.2f, textures %.2f, draw %.2f, layer restore %.2f",
               s_t[0] / 30, s_t[1] / 30, s_t[2] / 30, s_t[3] / 30, g_GLNRestoreMs / 30);
      memset(s_t, 0, sizeof(s_t)); g_GLNRestoreMs = 0;
      std::string g; for (auto& e : s_gpuByShader) { char b[96]; snprintf(b, sizeof(b), " [%s %.1f]", e.first.c_str(), e.second / 30); g += b; }
      if (!g.empty()) GLN::Log("GLNative GPU ms per frame by shader:%s", g.c_str());
      s_gpuByShader.clear();
    }
  }
  s_native = s_fallback = 0; s_reasons.clear();
}
