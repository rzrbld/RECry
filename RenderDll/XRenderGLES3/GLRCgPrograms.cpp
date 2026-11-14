/*=============================================================================
  GLRCgPrograms.cpp : the engine's Cg programs on the native backend.

  The engine (GLCGVProgram / GLCGPShader) loads each program with
  glProgramStringARB using text made by GLRCg::MakeProgramString (in
  cgGetProgramString), binds it, enables GL_VERTEX/FRAGMENT_PROGRAM_ARB and
  sets parameters with glProgramEnvParameter4fvARB. Here a program id holds
  the translated GLSL; a draw links its vertex and fragment sides into one
  GL program (a missing side is generated from the fixed-function key, as
  ARB programs pair with fixed-function stages), cached per combination.
  Parameter registers are snapshotted per draw (GLRCore.cpp) and uploaded
  as the uniform arrays Cv / Cf.
=============================================================================*/
#include "GLRCore.h"
#include "GLRCg.h"
#include <stdio.h>
#include <stdlib.h>
#include <map>

namespace GLR
{
struct CgProgram
{
  unsigned id = 0;
  bool vertex = true, ok = false;
  uint32_t serial = 0;             // changes with every ProgramString
  GLRCg::Program p;
  int regs = 0;                    // registers used (highest + 1)
  uint8_t texUnits = 0, texCube = 0;
  int maxTexIn = -1;               // fragment: highest vTexN read
};

namespace
{
std::map<unsigned, CgProgram*> s_progs;
unsigned s_nextId = 1;
uint32_t s_serial = 0;
std::map<std::string, Program*> s_pairs;
unsigned s_bound[2] = {};          // 0 vertex, 1 fragment

const char* const kAlphaCmp[8] = { nullptr, "o_Color.a > uAlphaRef", "o_Color.a < uAlphaRef", "o_Color.a >= uAlphaRef",
                                    "o_Color.a <= uAlphaRef", "o_Color.a == uAlphaRef", "o_Color.a != uAlphaRef", "false" };
} // namespace

bool ProgramsSupported()
{
  static int s = -1;
  if (s < 0)
  {
    const char* t = getenv("FARCRY_GLES3_SHADER_TIER");
    s = Active() && (!t || atoi(t) != 0) ? 1 : 0;
  }
  return s == 1;
}

void GenPrograms(int n, unsigned* out) { for (int i = 0; i < n; ++i) out[i] = s_nextId++; }
void DeletePrograms(int n, const unsigned* ids)
{
  for (int i = 0; i < n; ++i)
  {
    auto it = s_progs.find(ids[i]);
    if (it == s_progs.end()) continue;
    // Linked pairs keep their GL programs (keyed by serial; never reused).
    delete it->second; s_progs.erase(it);
  }
}
unsigned CgBound(bool vertex) { return s_bound[vertex ? 0 : 1]; }
void BindProgram(unsigned target, unsigned id) { s_bound[target == 0x8620 /*GL_VERTEX_PROGRAM_ARB*/ ? 0 : 1] = id; }

void ProgramString(unsigned target, const char* text, int len)
{
  const bool vertex = target == 0x8620;
  const unsigned id = s_bound[vertex ? 0 : 1];
  CgProgram*& cp = s_progs[id];
  if (!cp) cp = new CgProgram();
  cp->id = id; cp->vertex = vertex; cp->serial = ++s_serial; cp->ok = false; cp->texUnits = cp->texCube = 0; cp->maxTexIn = -1; cp->regs = 0;
  const std::string s(text ? text : "", text ? (size_t)len : 0);
  if (!GLRCg::ParseProgramString(s, cp->p))
  {
    static int n = 0;
    if (n++ < 5) Log("GLR: program %u has no translation (stale shader cache entry?) - fixed function used", id);
    return;
  }
  // Compiled when a draw first uses it (CgPair): the engine loads several
  // hundred programs per level and draws with a few dozen; compiling them all
  // here cost load time and driver memory. A program that fails to link is
  // marked then and its draws use fixed function.
  for (const GLRCg::Param& q : cp->p.params) if (q.reg + q.count > cp->regs) cp->regs = q.reg + q.count;
  for (const GLRCg::Sampler& sm : cp->p.samplers)
    if (sm.unit >= 0 && sm.unit < 4)
    {
      cp->texUnits |= (uint8_t)(1 << sm.unit);
      if (sm.type == "samplerCube") cp->texCube |= (uint8_t)(1 << sm.unit);
    }
  for (const GLRCg::Io& io : cp->p.inputs)
    if (!io.semantic.compare(0, 8, "TEXCOORD")) cp->maxTexIn = std::max(cp->maxTexIn, atoi(io.semantic.c_str() + 8));
  cp->ok = true;
  static int loaded = 0;
  if ((++loaded % 50) == 1) Log("GLR: %d engine programs loaded (last: %s %u, %d registers)", loaded, vertex ? "vertex" : "fragment", id, cp->regs);
}

void CgMemStats(size_t& glslBytes, int& programs, int& pairs)
{
  glslBytes = 0; programs = (int)s_progs.size(); pairs = (int)s_pairs.size();
  for (auto& e : s_progs) glslBytes += e.second->p.glsl.capacity();
}
CgProgram* CgFind(unsigned id, bool vertex)
{
  auto it = s_progs.find(id);
  return it != s_progs.end() && it->second->ok && it->second->vertex == vertex ? it->second : nullptr;
}

Program* CgPair(CgProgram* vp, CgProgram* fp, const ProgramKey& key, uint8_t& texUnits, uint8_t& texCube)
{
  texUnits = fp ? fp->texUnits : 0; texCube = fp ? fp->texCube : 0;
  // Pair key: serials + the fixed-function key where a side is generated.
  ProgramKey k = key;
  if (fp && !vp)
  {
    // Generated vertex side must feed every texcoord the program reads.
    const int need = std::min(fp->maxTexIn + 1, 4);
    for (int u = k.stages; u < need; ++u) { k.co[u] = k.ao[u] = k.ca[u] = k.aa[u] = 0; }
    if (need > k.stages) k.stages = (uint8_t)need;
  }
  char head[48];
  snprintf(head, sizeof(head), "%u|%u|", vp ? vp->serial : 0, fp ? fp->serial : 0);
  std::string pk(head);
  if (!vp || !fp) pk.append((const char*)&k, sizeof(k));
  else pk += (char)k.alphaTest;
  static std::string s_lastKey; static Program* s_last = nullptr;
  if (s_last && pk == s_lastKey) return s_last->ok ? s_last : nullptr;
  Program*& p = s_pairs[pk];
  if (!p)
  {
    const std::string vs = vp ? vp->p.glsl : BuildVS(k);
    std::string fs;
    if (fp)
    {
      fs = fp->p.glsl;
      if (k.alphaTest)
      {
        const size_t nl = fs.find('\n') + 1;   // after #version
        fs.insert(nl, std::string("#define GLR_ALPHA_TEST ") + kAlphaCmp[k.alphaTest & 7] + "\n");
      }
    }
    else fs = BuildFS(k);
    char name[64]; snprintf(name, sizeof(name), "cg vp %u fp %u", vp ? vp->id : 0, fp ? fp->id : 0);
    ProgramMeta meta;
    meta.nCv = vp ? std::min(vp->regs, kCgVRegs) : 0;
    meta.nCf = fp ? std::min(fp->regs, kCgFRegs) : 0;
    if (fp) for (const GLRCg::Sampler& sm : fp->p.samplers) meta.samplers.push_back({ sm.name, sm.unit });
    p = ProgramFor(name, vs, fs, &meta);
    if (p->ok)
    {
      // FARCRY_GLR_DUMP_PROGRAMS=<dir>: the pair's sources as files.
      if (const char* d = getenv("FARCRY_GLR_DUMP_PROGRAMS"))
        if (strcmp(d, "1"))
        {
          char path[512];
          snprintf(path, sizeof(path), "%s/vp%u_fp%u_%u.vs", d, vp ? vp->id : 0, fp ? fp->id : 0, p->prog);
          if (FILE* f = fopen(path, "w")) { fputs(vs.c_str(), f); fclose(f); }
          snprintf(path, sizeof(path), "%s/vp%u_fp%u_%u.fs", d, vp ? vp->id : 0, fp ? fp->id : 0, p->prog);
          if (FILE* f = fopen(path, "w")) { fputs(fs.c_str(), f); fclose(f); }
        }
    }
    else
    {
      Log("GLR: engine program pair vp %u fp %u failed to compile/link - fixed function used for it", vp ? vp->id : 0, fp ? fp->id : 0);
      if (vp) vp->ok = false;   // CgFind skips it from now on
      if (fp) fp->ok = false;
    }
  }
  s_lastKey = pk; s_last = p;
  return p->ok ? p : nullptr;
}
} // namespace GLR
