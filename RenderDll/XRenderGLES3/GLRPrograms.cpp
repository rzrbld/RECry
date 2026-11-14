/*=============================================================================
  GLRPrograms.cpp : material programs of the native backend.

  A ProgramKey (built from the engine's current stage state at record time)
  selects a generated GLSL ES 3.00 program. Stage semantics follow
  CGLRenderer::EF_SetColorOp: each stage combines up to two arguments
  (texture, vertex colour, previous stage, stage constant) with replace,
  modulate (x1/x2/x4 via the stage scale), add, add-signed or an
  interpolation by vertex/texture alpha; alpha likewise. Checked against the
  translation layer's generator (GLES3FFP.cpp BuildProgram) image by image.
  Lighting and texgen are added with the mesh passes (stage 2).
  Programs are binary-cached on disk (FARCRY_TEXCACHE/../native-programs).
=============================================================================*/
#include "GLRCore.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

namespace GLR
{
namespace
{
// engine codes (CryCommon/IShader.h)
enum { CO_NOSET, CO_DISABLE, CO_REPLACE, CO_DECAL, CO_ARG2, CO_MODULATE, CO_MODULATE2X, CO_MODULATE4X,
       CO_BLENDDIFFUSEALPHA, CO_BLENDTEXTUREALPHA, CO_DETAIL, CO_ADD, CO_ADDSIGNED };
enum { CA_SPECULAR, CA_TEXTURE, CA_DIFFUSE, CA_PREVIOUS, CA_CONSTANT };

std::string Src(int arg, int u, bool alpha)
{
  char b[32];
  switch (arg)
  {
    case CA_TEXTURE: snprintf(b, sizeof(b), "t%d", u); break;
    case CA_DIFFUSE: snprintf(b, sizeof(b), "vColor"); break;
    case CA_CONSTANT: snprintf(b, sizeof(b), "uStageColor%d", u); break;
    case CA_SPECULAR: case CA_PREVIOUS: default: snprintf(b, sizeof(b), "prev"); break;
  }
  return std::string(b) + (alpha ? ".a" : ".rgb");
}

std::string Combine(int op, int args, int u, bool alpha)
{
  const std::string a0 = Src(args & 7, u, alpha), a1 = Src((args >> 3) & 7, u, alpha);
  switch (op)
  {
    case CO_REPLACE: case CO_DECAL: case CO_DISABLE: case CO_NOSET: return a0;
    case CO_ARG2: return a1;
    case CO_ADD: return a0 + " + " + a1;
    case CO_ADDSIGNED: return a0 + " + " + a1 + (alpha ? " - 0.5" : " - vec3(0.5)");
    case CO_BLENDDIFFUSEALPHA:
      if (alpha) return a0 + " * " + a1;   // the engine leaves the alpha combiner at modulate
      return "mix(" + a1 + ", " + a0 + ", vColor.a)";
    case CO_BLENDTEXTUREALPHA:
      if (alpha) return a0 + " * " + a1;
      { char b[16]; snprintf(b, sizeof(b), "t%d.a", u); return "mix(" + a1 + ", " + a0 + ", " + b + ")"; }
    case CO_MODULATE: case CO_MODULATE2X: case CO_MODULATE4X: case CO_DETAIL: default: return a0 + " * " + a1;
  }
}

} // namespace
bool NeedsNormal(const ProgramKey& k)
{
  if (k.lights) return true;
  for (int u = 0; u < k.stages; ++u) for (int c = 0; c < 4; ++c) if (k.gen[u][c] >= 3) return true;
  return false;
}

std::string BuildVS(const ProgramKey& k)
{
  const bool nrm = NeedsNormal(k);
  std::string vs =
    "#version 300 es\n"
    "in vec4 aPos; in vec4 aColor; in vec3 aNormal; in vec4 aTex0; in vec4 aTex1; in vec4 aTex2; in vec4 aTex3;\n"
    "uniform mat4 uMV; uniform mat4 uP; uniform vec4 uCurColor; uniform vec4 uCurTex[4];\n"
    "uniform mat3 uNormalMat; uniform vec3 uCurNormal;\n"
    "out mediump vec4 vColor; out highp float vFogDist;\n";
  char b[300];
  if (k.lights > 1)
  {
    snprintf(b, sizeof(b), "uniform vec4 uLightPos[%d]; uniform vec4 uLightAmb[%d]; uniform vec4 uLightDiff[%d]; uniform vec3 uLightAtt[%d];\n",
             k.lights - 1, k.lights - 1, k.lights - 1, k.lights - 1);
    vs += b;
  }
  if (k.lights) vs += "uniform vec4 uMatAmb; uniform vec4 uMatDiff; uniform vec4 uMatEmis; uniform vec4 uSceneAmb;\n";
  for (int u = 0; u < k.stages; ++u)
  { snprintf(b, sizeof(b), "out highp vec4 vTex%d; uniform mat4 uGenObj%d; uniform mat4 uGenEye%d; uniform mat4 uTexMat%d;\n", u, u, u, u); vs += b; }
  vs += "void main() {\n  vec4 eye = uMV * aPos;\n  gl_Position = uP * eye;\n  vFogDist = abs(eye.z);\n";
  if (!k.colorArray) vs += "  vec4 inColor = uCurColor;\n";
  else if (k.colorSemantic == 1) vs += "  vec4 inColor = vec4(aColor.aaa * 2.0, 1.0);   // terrain: brightness in alpha, Modulate2X\n";
  else if (k.colorSemantic == 2) vs += "  vec4 inColor = vec4(min(aColor.rgb * 2.0, vec3(1.0)), 1.0);   // vegetation light\n";
  else vs += "  vec4 inColor = aColor;\n";
  if (nrm) vs += k.normalArray ? "  vec3 N = normalize(uNormalMat * aNormal);\n" : "  vec3 N = normalize(uNormalMat * uCurNormal);\n";
  if (k.lights)
  {
    // GL per-vertex lighting (no specular): emission + scene ambient * ambient
    // + sum over lights of attenuation * (ambient + max(N.L, 0) * diffuse).
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
  else vs += "  vColor = inColor;\n";
  static const char comp[4] = { 'x', 'y', 'z', 'w' };
  for (int u = 0; u < k.stages; ++u)
  {
    if (k.texStream & (1 << u)) snprintf(b, sizeof(b), "  vec4 tc%d = aTex%d;\n", u, u);
    else snprintf(b, sizeof(b), "  vec4 tc%d = uCurTex[%d];   // no texcoord stream: the current texcoord\n", u, u);
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

std::string BuildFS(const ProgramKey& k)
{
  std::string fs = "#version 300 es\nprecision mediump float;\nin mediump vec4 vColor; in highp float vFogDist;\n";
  char b[200];
  for (int u = 0; u < k.stages; ++u)
  {
    snprintf(b, sizeof(b), "in highp vec4 vTex%d; uniform %s uTex%d; uniform vec4 uStageColor%d; uniform vec2 uStageScale%d;\n",
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
    snprintf(b, sizeof(b), "    prev = clamp(vec4(r * uStageScale%d.x, a * uStageScale%d.y), 0.0, 1.0);\n  }\n", u, u);
    fs += b;
  }
  static const char* const cmp[8] = { "true", "prev.a > uAlphaRef", "prev.a < uAlphaRef", "prev.a >= uAlphaRef",
                                      "prev.a <= uAlphaRef", "prev.a == uAlphaRef", "prev.a != uAlphaRef", "false" };
  if (k.alphaTest) { fs += "  if (!("; fs += cmp[k.alphaTest & 7]; fs += ")) discard;\n"; }
  // uFog: start, end, 1/(end-start), density
  if (k.fog == 1) fs += "  float f = clamp((uFog.y - vFogDist) * uFog.z, 0.0, 1.0);\n";
  else if (k.fog == 2) fs += "  float f = clamp(exp(-uFog.w * vFogDist), 0.0, 1.0);\n";
  else if (k.fog == 3) fs += "  float f = clamp(exp(-(uFog.w * vFogDist) * (uFog.w * vFogDist)), 0.0, 1.0);\n";
  if (k.fog) fs += "  prev.rgb = mix(uFogColor, prev.rgb, f);\n";
  fs += "  oColor = prev;\n}\n";
  return fs;
}

namespace
{
std::string CacheDir()
{
  static std::string dir; static bool init = false;
  if (!init)
  {
    init = true;
    if (const char* d = getenv("FARCRY_NATIVE_PROGCACHE")) dir = d;
    else if (const char* t = getenv("FARCRY_TEXCACHE")) dir = std::string(t) + "/../native-programs";
    if (!dir.empty()) mkdir(dir.c_str(), 0755);
  }
  return dir;
}
uint64_t Fnv(const std::string& s, uint64_t h = 1469598103934665603ULL) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; } return h; }

GLuint Compile(GLenum type, const std::string& src, const char* name)
{
  GLuint sh = gl.glCreateShader(type);
  const char* p = src.c_str();
  gl.glShaderSource(sh, 1, &p, nullptr);
  gl.glCompileShader(sh);
  GLint ok = 0; gl.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[2048] = ""; gl.glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
    Log("GLR: %s %s shader failed: %s\n%s", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log, src.c_str());
    gl.glDeleteShader(sh);
    return 0;
  }
  return sh;
}

struct KeyHash { size_t operator()(const std::string& s) const { return (size_t)Fnv(s); } };
std::unordered_map<std::string, Program*, KeyHash> s_programs;
ProgramKey s_lastKey; Program* s_lastProg = nullptr;
} // namespace

bool BuildProgram(Program& p, const char* name, const std::string& vs, const std::string& fs)
{
  const char* ren = (const char*)gl.glGetString(GL_RENDERER);
  const char* ver = (const char*)gl.glGetString(GL_VERSION);
  const uint64_t h = Fnv(fs, Fnv(vs, Fnv(std::string(ren ? ren : "") + (ver ? ver : ""))));
  const std::string dir = CacheDir();
  char path[512] = "";
  if (!dir.empty()) snprintf(path, sizeof(path), "%s/%016llx.bin", dir.c_str(), (unsigned long long)h);
  p.prog = gl.glCreateProgram();
  bool linked = false;
  if (path[0])
    if (FILE* f = fopen(path, "rb"))
    {
      GLenum fmt = 0; std::vector<char> data;
      if (fread(&fmt, sizeof(fmt), 1, f) == 1)
      {
        fseek(f, 0, SEEK_END); const long n = ftell(f) - (long)sizeof(fmt); fseek(f, sizeof(fmt), SEEK_SET);
        if (n > 0) { data.resize(n); if (fread(data.data(), 1, n, f) != (size_t)n) data.clear(); }
      }
      fclose(f);
      if (!data.empty())
      {
        gl.glProgramBinary(p.prog, fmt, data.data(), (GLsizei)data.size());
        GLint ok = 0; gl.glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
        linked = ok != 0;
      }
    }
  if (!linked)
  {
    GLuint v = Compile(GL_VERTEX_SHADER, vs, name), f = v ? Compile(GL_FRAGMENT_SHADER, fs, name) : 0;
    if (!v || !f) { if (v) gl.glDeleteShader(v); gl.glDeleteProgram(p.prog); p.prog = 0; return false; }
    gl.glAttachShader(p.prog, v); gl.glAttachShader(p.prog, f);
    static const char* names[ATTR_COUNT] = { "aPos", "aColor", "aNormal", "aTex0", "aTex1", "aTex2", "aTex3", "aTex4", "aTex5", "aTex6", "aTex7", "aColor1" };
    for (int a = 0; a < ATTR_COUNT; ++a) gl.glBindAttribLocation(p.prog, a, names[a]);
    if (path[0]) gl.glProgramParameteri(p.prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    gl.glLinkProgram(p.prog);
    gl.glDeleteShader(v); gl.glDeleteShader(f);
    GLint ok = 0; gl.glGetProgramiv(p.prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
      char log[2048] = ""; gl.glGetProgramInfoLog(p.prog, sizeof(log), nullptr, log);
      Log("GLR: %s link failed: %s", name, log);
      gl.glDeleteProgram(p.prog); p.prog = 0;
      return false;
    }
    if (path[0])
    {
      GLint len = 0; gl.glGetProgramiv(p.prog, GL_PROGRAM_BINARY_LENGTH, &len);
      if (len > 0)
      {
        std::vector<char> data(len); GLenum fmt = 0; GLsizei got = 0;
        gl.glGetProgramBinary(p.prog, len, &got, &fmt, data.data());
        const std::string tmp = std::string(path) + ".tmp";
        if (FILE* out = fopen(tmp.c_str(), "wb"))
        {
          const bool w = fwrite(&fmt, sizeof(fmt), 1, out) == 1 && fwrite(data.data(), 1, got, out) == (size_t)got;
          fclose(out);
          if (w) rename(tmp.c_str(), path); else remove(tmp.c_str());
        }
      }
    }
  }
  // Uniform locations; samplers fixed to their units once.
  p.uMV = gl.glGetUniformLocation(p.prog, "uMV"); p.uP = gl.glGetUniformLocation(p.prog, "uP");
  p.uCurColor = gl.glGetUniformLocation(p.prog, "uCurColor"); p.uCurTex = gl.glGetUniformLocation(p.prog, "uCurTex"); p.uAlphaRef = gl.glGetUniformLocation(p.prog, "uAlphaRef");
  p.uFog = gl.glGetUniformLocation(p.prog, "uFog"); p.uFogColor = gl.glGetUniformLocation(p.prog, "uFogColor");
  p.uLightPos = gl.glGetUniformLocation(p.prog, "uLightPos"); p.uLightAmb = gl.glGetUniformLocation(p.prog, "uLightAmb");
  p.uLightDiff = gl.glGetUniformLocation(p.prog, "uLightDiff"); p.uLightAtt = gl.glGetUniformLocation(p.prog, "uLightAtt");
  p.uMatAmb = gl.glGetUniformLocation(p.prog, "uMatAmb"); p.uMatDiff = gl.glGetUniformLocation(p.prog, "uMatDiff");
  p.uMatEmis = gl.glGetUniformLocation(p.prog, "uMatEmis"); p.uSceneAmb = gl.glGetUniformLocation(p.prog, "uSceneAmb");
  p.uNormalMat = gl.glGetUniformLocation(p.prog, "uNormalMat"); p.uCurNormal = gl.glGetUniformLocation(p.prog, "uCurNormal");
  gl.glUseProgram(p.prog);
  for (int u = 0; u < 4; ++u)
  {
    char b[32];
    snprintf(b, sizeof(b), "uTex%d", u); const GLint t = gl.glGetUniformLocation(p.prog, b); if (t >= 0) gl.glUniform1i(t, u);
    snprintf(b, sizeof(b), "uStageColor%d", u); p.uStageColor[u] = gl.glGetUniformLocation(p.prog, b);
    snprintf(b, sizeof(b), "uStageScale%d", u); p.uStageScale[u] = gl.glGetUniformLocation(p.prog, b);
    snprintf(b, sizeof(b), "uGenObj%d", u); p.uGenObj[u] = gl.glGetUniformLocation(p.prog, b);
    snprintf(b, sizeof(b), "uGenEye%d", u); p.uGenEye[u] = gl.glGetUniformLocation(p.prog, b);
    snprintf(b, sizeof(b), "uTexMat%d", u); p.uTexMat[u] = gl.glGetUniformLocation(p.prog, b);
  }
  p.ok = true; p.fresh = true;
  return true;
}

Program* GetProgram(const ProgramKey& k)
{
  if (s_lastProg && !memcmp(&k, &s_lastKey, sizeof(k))) return s_lastProg;
  const std::string key((const char*)&k, sizeof(k));
  auto it = s_programs.find(key);
  Program* p;
  if (it != s_programs.end()) p = it->second;
  else
  {
    p = new Program();
    BuildProgram(*p, "material", BuildVS(k), BuildFS(k));
    if (getenv("FARCRY_GLR_DUMP_PROGRAMS")) Log("GLR program %zu:\n--- VS\n%s--- FS\n%s---", s_programs.size(), BuildVS(k).c_str(), BuildFS(k).c_str());
    s_programs[key] = p;
  }
  s_lastKey = k; s_lastProg = p;
  return p;
}

size_t ProgramCount() { return s_programs.size(); }
void ProgramsShutdown()
{
  for (auto& e : s_programs) { if (e.second->prog) gl.glDeleteProgram(e.second->prog); delete e.second; }
  s_programs.clear(); s_lastProg = nullptr;
}
} // namespace GLR
