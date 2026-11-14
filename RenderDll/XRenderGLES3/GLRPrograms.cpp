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
#include <SDL.h>
#include "../../port/platform/PortTiming.h"
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

static bool BuildProgramImpl(Program& p, const char* name, const std::string& vs, const std::string& fs, bool& fromCache);
// Every build is logged with its cost: builds while playing are frame hitches
// (shader warm-up moves them to the loading screen).
unsigned g_GLRBuilds = 0; double g_GLRBuildMs = 0;
bool BuildProgram(Program& p, const char* name, const std::string& vs, const std::string& fs)
{
  const uint64_t t0 = SDL_GetPerformanceCounter();
  bool fromCache = false;
  bool ok = false;
  Sync([&] { ok = BuildProgramImpl(p, name, vs, fs, fromCache); });   // GL lives on the render thread
  const double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
  ++g_GLRBuilds; g_GLRBuildMs += ms;
  const double gt = g_PortGameplayStartMs > 0 ? (PortNowMs() - g_PortGameplayStartMs) / 1000.0 : -1.0;
  Log("GLR build: %.1f ms %s, %s%s", ms, fromCache ? "(binary cache)" : "(compiled)", name,
      gt >= 0 ? (gt > 0.5 ? " - DURING GAMEPLAY" : "") : "");
  return ok;
}
static bool BuildProgramImpl(Program& p, const char* name, const std::string& vs, const std::string& fs, bool& fromCache)
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
        fromCache = linked;
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
  p.uClip = gl.glGetUniformLocation(p.prog, "uGlrClipPlane");
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

// ---------------------------------------------------------------- shared programs + warm-up
// Programs are shared by content (vertex + fragment text): the same source
// built twice is one GL program, and a level's programs can be built again in
// the next run before its first frame. Programs built after a level finished
// loading (i.e. while playing) are remembered per level in
// <cache>/warm-<level>.txt with their sources; WarmUp() builds them at the end
// of the next load of that level, from the binary cache, so no program is
// compiled or linked during gameplay on a repeat visit.
namespace
{
std::unordered_map<uint64_t, Program*> s_byContent;
std::string s_warmLevel;                 // current level's warm list ("" before the first load)
std::unordered_map<uint64_t, bool> s_warmKnown;
uint64_t ContentHash(const std::string& vs, const std::string& fs) { return Fnv(fs, Fnv(vs, 0xcbf29ce484222325ULL)); }
std::string Slurp(const std::string& path)
{
  std::string out; FILE* f = fopen(path.c_str(), "rb"); if (!f) return out;
  char b[8192]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n); fclose(f); return out;
}
void Spit(const std::string& path, const std::string& text)
{
  const std::string tmp = path + ".tmp";
  if (FILE* f = fopen(tmp.c_str(), "wb")) { const bool w = fwrite(text.data(), 1, text.size(), f) == text.size(); fclose(f); if (w) rename(tmp.c_str(), path.c_str()); else remove(tmp.c_str()); }
}
std::string MetaText(const ProgramMeta* m)
{
  std::string t = m ? std::to_string(m->nCv) + " " + std::to_string(m->nCf) + "\n" : "0 0\n";
  if (m) for (auto& sm : m->samplers) t += sm.first + " " + std::to_string(sm.second) + "\n";
  return t;
}
bool ParseMeta(const std::string& t, ProgramMeta& m)
{
  const char* p = t.c_str(); int a = 0, b = 0, used = 0;
  if (sscanf(p, "%d %d%n", &a, &b, &used) < 2) return false;
  m.nCv = a; m.nCf = b; p += used;
  char name[128]; int unit;
  while (sscanf(p, "%127s %d%n", name, &unit, &used) == 2) { m.samplers.push_back({ name, unit }); p += used; }
  return true;
}
void ApplyMeta(Program& p, const ProgramMeta& m)
{
  p.uCv = gl.glGetUniformLocation(p.prog, "Cv"); if (p.uCv < 0) p.uCv = gl.glGetUniformLocation(p.prog, "Cv[0]");
  p.uCf = gl.glGetUniformLocation(p.prog, "Cf"); if (p.uCf < 0) p.uCf = gl.glGetUniformLocation(p.prog, "Cf[0]");
  p.nCv = m.nCv; p.nCf = m.nCf;
  if (p.uAlphaRef < 0) p.uAlphaRef = gl.glGetUniformLocation(p.prog, "uAlphaRef");
  if (!m.samplers.empty())
  {
    gl.glUseProgram(p.prog);
    for (auto& sm : m.samplers) { const GLint l = gl.glGetUniformLocation(p.prog, sm.first.c_str()); if (l >= 0) gl.glUniform1i(l, sm.second); }
  }
}
} // namespace

Program* ProgramFor(const char* name, const std::string& vs, const std::string& fs, const ProgramMeta* meta)
{
  const uint64_t h = ContentHash(vs, fs);
  auto it = s_byContent.find(h);
  if (it != s_byContent.end()) return it->second;
  Program* p = new Program();
  if (BuildProgram(*p, name, vs, fs) && meta) Sync([p, meta] { ApplyMeta(*p, *meta); });
  s_byContent[h] = p;
  // Built while playing: remember it for this level's next load.
  const std::string dir = CacheDir();
  if (p->ok && !s_warmLevel.empty() && !dir.empty() && !s_warmKnown.count(h))
  {
    s_warmKnown[h] = true;
    char base[512]; snprintf(base, sizeof(base), "%s/src-%016llx", dir.c_str(), (unsigned long long)h);
    Spit(std::string(base) + ".vs", vs); Spit(std::string(base) + ".fs", fs); Spit(std::string(base) + ".meta", MetaText(meta));
    if (FILE* f = fopen((dir + "/warm-" + s_warmLevel + ".txt").c_str(), "a")) { fprintf(f, "%016llx\n", (unsigned long long)h); fclose(f); }
  }
  return p;
}

void WarmUp(const char* level)
{
  // level path -> file-safe key
  std::string key;
  for (const char* c = level ? level : ""; *c; ++c) key += isalnum((unsigned char)*c) ? (char)tolower((unsigned char)*c) : '_';
  while (!key.empty() && key.back() == '_') key.pop_back();
  s_warmLevel = key; s_warmKnown.clear();
  const std::string dir = CacheDir();
  if (key.empty() || dir.empty()) return;
  const uint64_t t0 = SDL_GetPerformanceCounter();
  int built = 0, missing = 0;
  const std::string list = Slurp(dir + "/warm-" + key + ".txt");
  for (size_t i = 0; i + 16 <= list.size(); )
  {
    const size_t e = list.find('\n', i);
    const std::string hex = list.substr(i, (e == std::string::npos ? list.size() : e) - i);
    i = e == std::string::npos ? list.size() : e + 1;
    const uint64_t h = strtoull(hex.c_str(), nullptr, 16);
    if (!h || s_warmKnown.count(h)) continue;
    s_warmKnown[h] = true;
    if (s_byContent.count(h)) continue;
    char base[512]; snprintf(base, sizeof(base), "%s/src-%016llx", dir.c_str(), (unsigned long long)h);
    const std::string vs = Slurp(std::string(base) + ".vs"), fs = Slurp(std::string(base) + ".fs");
    ProgramMeta m;
    if (vs.empty() || fs.empty() || !ParseMeta(Slurp(std::string(base) + ".meta"), m)) { ++missing; continue; }
    ProgramFor("warm-up", vs, fs, &m);
    ++built;
  }
  Log("GLR warm-up %s: %d programs built in %.0f ms%s", key.c_str(), built,
      (SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency(), missing ? " (some sources missing)" : "");
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
    std::string vs = BuildVS(k), fs = BuildFS(k);
    if (k.clip) AddClipPlane(vs, fs);
    p = ProgramFor("material", vs, fs, nullptr);
    if (getenv("FARCRY_GLR_DUMP_PROGRAMS")) Log("GLR program %zu:\n--- VS\n%s--- FS\n%s---", s_programs.size(), BuildVS(k).c_str(), BuildFS(k).c_str());
    s_programs[key] = p;
  }
  s_lastKey = k; s_lastProg = p;
  return p;
}

void AddClipPlane(std::string& vs, std::string& fs)
{
  // Declarations just before main (after any #version/#extension lines); the
  // vertex side sets the distance at the end of main (the last function of
  // every program here), the fragment side discards first thing in main.
  const size_t vm = vs.find("void main()");
  if (vm != std::string::npos)
  {
    vs.insert(vm, "uniform highp vec4 uGlrClipPlane;\nout highp float vGlrClip;\n");
    const size_t vend = vs.rfind('}');
    vs.insert(vend, "  vGlrClip = dot(uGlrClipPlane, gl_Position);\n");
  }
  const size_t fm = fs.find("void main()");
  if (fm != std::string::npos)
  {
    fs.insert(fm, "in highp float vGlrClip;\n");
    const size_t fb = fs.find('{', fm);
    fs.insert(fb + 1, "\n  if (vGlrClip < 0.0) discard;\n");
  }
}

size_t ProgramCount() { return s_programs.size(); }
void ProgramsShutdown()
{
  for (auto& e : s_programs) { if (e.second->prog) gl.glDeleteProgram(e.second->prog); delete e.second; }
  s_programs.clear(); s_lastProg = nullptr;
}
} // namespace GLR
