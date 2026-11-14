/*=============================================================================
  GLRCg.cpp : Far Cry Cg programs -> GLSL ES 3.00 (see GLRCg.h).

  Conventions
  - Cg matrices are row-major (each register holds a row). In GLSL a Cg
    floatRxC is a matRxC whose COLUMNS are the Cg rows, so:
      mul(a, b) == (b) * (a)       for matrix*vector, vector*matrix, matrix*matrix
      M[i]      == row i           M._RC / M._mRC == M[R][C]
  - Parameters read from the stage's register array: Cv[] (vertex) or Cf[]
    (fragment); the engine writes it with glProgramEnvParameter4fvARB.
  - Stage interface by semantic: attributes a_<SEM>, varyings v_<SEM> (vec4).
=============================================================================*/
#include "GLRCg.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <set>

namespace GLRCg
{
namespace
{
enum TokKind { T_WS, T_ID, T_NUM, T_PUNCT, T_PRE };
struct Tok { TokKind k; std::string s; };

void Tokenize(const std::string& src, std::vector<Tok>& out)
{
  size_t i = 0, n = src.size();
  bool lineStart = true;
  while (i < n)
  {
    const char c = src[i];
    if (c == '\n') { out.push_back({ T_WS, "\n" }); ++i; lineStart = true; continue; }
    if (isspace((unsigned char)c)) { size_t j = i; while (j < n && isspace((unsigned char)src[j]) && src[j] != '\n') ++j; out.push_back({ T_WS, src.substr(i, j - i) }); i = j; continue; }
    if (c == '/' && i + 1 < n && src[i + 1] == '/') { size_t j = src.find('\n', i); if (j == std::string::npos) j = n; out.push_back({ T_WS, " " }); i = j; continue; }
    if (c == '/' && i + 1 < n && src[i + 1] == '*') { size_t j = src.find("*/", i + 2); j = j == std::string::npos ? n : j + 2; out.push_back({ T_WS, " " }); i = j; continue; }
    if (c == '#' && lineStart) { size_t j = src.find('\n', i); if (j == std::string::npos) j = n; out.push_back({ T_PRE, src.substr(i, j - i) }); i = j; continue; }
    lineStart = false;
    if (isalpha((unsigned char)c) || c == '_') { size_t j = i; while (j < n && (isalnum((unsigned char)src[j]) || src[j] == '_')) ++j; out.push_back({ T_ID, src.substr(i, j - i) }); i = j; continue; }
    if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)src[i + 1])))
    {
      size_t j = i;
      bool dot = false;
      while (j < n && (isdigit((unsigned char)src[j]) || (src[j] == '.' && !dot))) { if (src[j] == '.') dot = true; ++j; }
      if (j < n && (src[j] == 'e' || src[j] == 'E') && j + 1 < n && (isdigit((unsigned char)src[j + 1]) || src[j + 1] == '-' || src[j + 1] == '+'))
      { j += 2; while (j < n && isdigit((unsigned char)src[j])) ++j; }
      if (j < n && (src[j] == 'f' || src[j] == 'F' || src[j] == 'h' || src[j] == 'H' || src[j] == 'x' || src[j] == 'X')) ++j;
      out.push_back({ T_NUM, src.substr(i, j - i) }); i = j; continue;
    }
    out.push_back({ T_PUNCT, std::string(1, c) }); ++i;
  }
}

// Next / previous non-whitespace token index.
size_t NextNw(const std::vector<Tok>& t, size_t i) { ++i; while (i < t.size() && t[i].k == T_WS) ++i; return i; }
size_t PrevNw(const std::vector<Tok>& t, size_t i) { if (!i) return (size_t)-1; --i; while (i > 0 && t[i].k == T_WS) --i; return t[i].k == T_WS ? (size_t)-1 : i; }

// Cg type name -> GLSL type name ("" if not a Cg type).
std::string MapType(const std::string& s)
{
  static const char* const bases[] = { "float", "half", "fixed", "double" };
  for (const char* b : bases)
  {
    const size_t bl = strlen(b);
    if (s.compare(0, bl, b) != 0) continue;
    const std::string rest = s.substr(bl);
    if (rest.empty()) return "float";
    if (rest.size() == 1 && rest[0] >= '2' && rest[0] <= '4') return std::string("vec") + rest;
    if (rest.size() == 3 && rest[1] == 'x' && rest[0] >= '1' && rest[0] <= '4' && rest[2] >= '1' && rest[2] <= '4')
    {
      // Cg floatRxC: R rows of C -> GLSL R columns of C
      if (rest[0] == '1') return rest[2] == '1' ? "float" : std::string("vec") + rest[2];
      if (rest[0] == rest[2]) return std::string("mat") + rest[0];
      return std::string("mat") + rest[0] + "x" + rest[2];
    }
  }
  if (s == "int2" || s == "int3" || s == "int4") return std::string("ivec") + s[3];
  if (s == "bool2" || s == "bool3" || s == "bool4") return std::string("bvec") + s[4];
  if (s == "sampler2D" || s == "samplerCube" || s == "sampler3D") return s;
  if (s == "samplerCUBE") return "samplerCube";
  if (s == "samplerRECT" || s == "sampler1D") return "sampler2D";
  return "";
}
// Rows and row width of a Cg type (float4 -> 1x4, float3x4 -> 3x4).
void Shape(const std::string& cgType, int& rows, int& cols)
{
  rows = 1; cols = 4;
  std::string t = cgType;
  for (const char* b : { "float", "half", "fixed", "double" }) if (t.compare(0, strlen(b), b) == 0) { t = t.substr(strlen(b)); break; }
  if (t.empty()) { cols = 1; return; }
  if (t.size() == 1) { cols = t[0] - '0'; return; }
  if (t.size() == 3 && t[1] == 'x') { rows = t[0] - '0'; cols = t[2] - '0'; }
}

std::string NormSem(std::string s)
{
  for (auto& c : s) c = (char)toupper((unsigned char)c);
  if (s == "COLOR" || s == "COL0" || s == "DIFFUSE") return "COLOR0";
  if (s == "COL1" || s == "SPECULAR") return "COLOR1";
  if (s == "TEXCOORD") return "TEXCOORD0";
  if (s == "HPOS" || s == "POS") return "POSITION";
  if (s == "NRML") return "NORMAL";
  return s;
}

struct Field { std::string type, name, sem; };
struct Struct { std::vector<Field> fields; size_t close = 0; };

const char* kPrelude =
  "#define CGC true\n"
  "float saturate(float x) { return clamp(x, 0.0, 1.0); }\n"
  "vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }\n"
  "vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }\n"
  "vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }\n"
  "vec4 tex2D(sampler2D s, vec2 t) { return texture(s, t); }\n"
  "vec4 tex2D(sampler2D s, vec3 t) { return texture(s, t.xy); }\n"
  "vec4 tex2D(sampler2D s, vec4 t) { return texture(s, t.xy); }\n"
  "vec4 tex2Dproj(sampler2D s, vec3 t) { return textureProj(s, t); }\n"
  "vec4 tex2Dproj(sampler2D s, vec4 t) { return textureProj(s, t); }\n"
  "vec4 texCUBE(samplerCube s, vec3 t) { return texture(s, t); }\n"
  "vec4 texCUBE(samplerCube s, vec4 t) { return texture(s, t.xyz); }\n"
  // Rectangle textures (pixel coordinates) are 2D textures here.
  "vec4 texRECT(sampler2D s, vec2 t) { return texture(s, t / vec2(textureSize(s, 0))); }\n"
  "vec4 texRECT(sampler2D s, vec3 t) { return texture(s, t.xy / vec2(textureSize(s, 0))); }\n"
  "vec4 texRECTproj(sampler2D s, vec3 t) { return texture(s, (t.xy / t.z) / vec2(textureSize(s, 0))); }\n"
  "vec4 texRECTproj(sampler2D s, vec4 t) { return texture(s, (t.xy / t.w) / vec2(textureSize(s, 0))); }\n"
  // PS 1.x offset textures (offsettex2D): the PS 2.0 formula of the same shaders.
  "vec4 offsettex2D(sampler2D s, vec2 t, vec4 prev, vec4 m) { return texture(s, t + m.xy * prev.xx + m.zw * prev.yy); }\n"
  "vec4 offsettexRECT(sampler2D s, vec2 t, vec4 prev, vec4 m) { return texRECT(s, t + m.xy * prev.xx + m.zw * prev.yy); }\n"
  // Cg promotes scalars in intrinsics; GLSL does not (and built-ins cannot
  // be overloaded in GLSL ES): dot/pow are renamed to cg_dot/cg_pow.
  "float cg_dot(float a, float b) { return a * b; }\n"
  "float cg_dot(vec2 a, vec2 b) { return dot(a, b); }\n"
  "float cg_dot(vec3 a, vec3 b) { return dot(a, b); }\n"
  "float cg_dot(vec4 a, vec4 b) { return dot(a, b); }\n"
  "float cg_dot(vec2 a, float b) { return dot(a, vec2(b)); }\n"
  "float cg_dot(vec3 a, float b) { return dot(a, vec3(b)); }\n"
  "float cg_dot(vec4 a, float b) { return dot(a, vec4(b)); }\n"
  "float cg_dot(float a, vec2 b) { return dot(vec2(a), b); }\n"
  "float cg_dot(float a, vec3 b) { return dot(vec3(a), b); }\n"
  "float cg_dot(float a, vec4 b) { return dot(vec4(a), b); }\n"
  "float cg_step(float a, float b) { return step(a, b); }\n"
  "vec2 cg_step(vec2 a, vec2 b) { return step(a, b); }\n"
  "vec3 cg_step(vec3 a, vec3 b) { return step(a, b); }\n"
  "vec4 cg_step(vec4 a, vec4 b) { return step(a, b); }\n"
  "vec2 cg_step(float a, vec2 b) { return step(a, b); }\n"
  "vec3 cg_step(float a, vec3 b) { return step(a, b); }\n"
  "vec4 cg_step(float a, vec4 b) { return step(a, b); }\n"
  "vec2 cg_step(vec2 a, float b) { return step(a, vec2(b)); }\n"
  "vec3 cg_step(vec3 a, float b) { return step(a, vec3(b)); }\n"
  "vec4 cg_step(vec4 a, float b) { return step(a, vec4(b)); }\n"
  "float cg_pow(float a, float b) { return pow(a, b); }\n"
  "vec2 cg_pow(vec2 a, vec2 b) { return pow(a, b); }\n"
  "vec3 cg_pow(vec3 a, vec3 b) { return pow(a, b); }\n"
  "vec4 cg_pow(vec4 a, vec4 b) { return pow(a, b); }\n"
  "vec2 cg_pow(vec2 a, float b) { return pow(a, vec2(b)); }\n"
  "vec3 cg_pow(vec3 a, float b) { return pow(a, vec3(b)); }\n"
  "vec4 cg_pow(vec4 a, float b) { return pow(a, vec4(b)); }\n"
  "vec2 cg_v2(float x) { return vec2(x); }\n"
  "vec2 cg_v2(vec2 x) { return x; }\n"
  "vec2 cg_v2(vec3 x) { return x.xy; }\n"
  "vec2 cg_v2(vec4 x) { return x.xy; }\n"
  "vec3 cg_v3(float x) { return vec3(x); }\n"
  "vec3 cg_v3(vec3 x) { return x; }\n"
  "vec3 cg_v3(vec4 x) { return x.xyz; }\n"
  "vec4 cg_v4(float x) { return vec4(x); }\n"
  "vec4 cg_v4(vec4 x) { return x; }\n"
  "vec2 cg_rep2(float x) { return vec2(x); } vec2 cg_rep2(vec2 x) { return x.xx; } vec2 cg_rep2(vec3 x) { return x.xx; } vec2 cg_rep2(vec4 x) { return x.xx; }\n"
  "vec3 cg_rep3(float x) { return vec3(x); } vec3 cg_rep3(vec2 x) { return x.xxx; } vec3 cg_rep3(vec3 x) { return x.xxx; } vec3 cg_rep3(vec4 x) { return x.xxx; }\n"
  "vec4 cg_rep4(float x) { return vec4(x); } vec4 cg_rep4(vec2 x) { return x.xxxx; } vec4 cg_rep4(vec3 x) { return x.xxxx; } vec4 cg_rep4(vec4 x) { return x.xxxx; }\n"
  "vec4 toV4(float x) { return vec4(x, 0.0, 0.0, 1.0); }\n"
  "vec4 toV4(vec2 x) { return vec4(x, 0.0, 1.0); }\n"
  "vec4 toV4(vec3 x) { return vec4(x, 1.0); }\n"
  "vec4 toV4(vec4 x) { return x; }\n";

// Only the helper functions a program uses (and what those use): the full
// set in every program cost the GPU driver compile time and memory.
std::string UsedPrelude(const char* prelude, const std::string& code)
{
  struct Line { std::string text, name; bool on = false; };
  std::vector<Line> lines;
  for (const char* p = prelude; *p;)
  {
    const char* e = strchr(p, '\n'); if (!e) e = p + strlen(p);
    Line l; l.text.assign(p, e - p + (*e ? 1 : 0));
    // "<type> <name>(" -> name; other lines (#define) always kept
    const size_t par = l.text.find('(');
    if (l.text[0] != '#' && par != std::string::npos)
    {
      size_t a = par; while (a > 0 && l.text[a - 1] == ' ') --a;
      size_t b = a; while (b > 0 && (isalnum((unsigned char)l.text[b - 1]) || l.text[b - 1] == '_')) --b;
      l.name = l.text.substr(b, a - b);
    }
    else l.on = true;
    lines.push_back(l);
    p = *e ? e + 1 : e;
  }
  auto uses = [](const std::string& text, const std::string& name) {
    for (size_t i = text.find(name); i != std::string::npos; i = text.find(name, i + 1))
    {
      const bool l = i == 0 || !(isalnum((unsigned char)text[i - 1]) || text[i - 1] == '_');
      const size_t j = i + name.size();
      const bool r = j >= text.size() || !(isalnum((unsigned char)text[j]) || text[j] == '_');
      if (l && r) return true;
    }
    return false;
  };
  std::string scope = code;
  for (bool changed = true; changed;)
  {
    changed = false;
    for (auto& l : lines)
      if (!l.on && uses(scope, l.name)) { l.on = true; changed = true; }
    if (changed) { scope = code; for (auto& l : lines) if (l.on) scope += l.text; }
  }
  std::string out;
  for (auto& l : lines) if (l.on) out += l.text;
  return out;
}

std::string Join(const std::vector<Tok>& t, size_t a, size_t b) { std::string s; for (size_t i = a; i < b && i < t.size(); ++i) s += t[i].s; return s; }

// Split "(a, b)" starting at the '(' token: returns the argument token ranges and the index of ')'.
bool SplitArgs(const std::vector<Tok>& t, size_t open, std::vector<std::pair<size_t, size_t>>& args, size_t& close)
{
  int depth = 0; size_t start = open + 1;
  for (size_t i = open; i < t.size(); ++i)
  {
    const std::string& s = t[i].s;
    if (t[i].k != T_PUNCT) continue;
    if (s == "(" || s == "[") ++depth;
    else if (s == ")" || s == "]")
    {
      if (--depth == 0) { args.push_back({ start, i }); close = i; return true; }
    }
    else if (s == "," && depth == 1) { args.push_back({ start, i }); start = i + 1; }
  }
  return false;
}

// mul(a, b) -> ((b) * (a)), innermost first (on the joined text, re-tokenised).
std::string RewriteMul(const std::string& src)
{
  std::string s = src;
  for (int guard = 0; guard < 2000; ++guard)
  {
    std::vector<Tok> t; Tokenize(s, t);
    size_t found = (size_t)-1;
    for (size_t i = 0; i < t.size(); ++i)
      if (t[i].k == T_ID && t[i].s == "mul")
      {
        const size_t o = NextNw(t, i);
        if (o >= t.size() || t[o].s != "(") continue;
        // innermost: no further mul inside its arguments
        std::vector<std::pair<size_t, size_t>> a; size_t c;
        if (!SplitArgs(t, o, a, c) || a.size() != 2) continue;
        bool inner = false;
        for (size_t j = o; j < c; ++j) if (t[j].k == T_ID && t[j].s == "mul") inner = true;
        if (!inner) { found = i; break; }
      }
    if (found == (size_t)-1) return s;
    const size_t o = NextNw(t, found);
    std::vector<std::pair<size_t, size_t>> a; size_t c;
    SplitArgs(t, o, a, c);
    const std::string repl = "((" + Join(t, a[1].first, a[1].second) + ") * (" + Join(t, a[0].first, a[0].second) + "))";
    s = Join(t, 0, found) + repl + Join(t, c + 1, t.size());
  }
  return s;
}

// ---- conditional preprocessing (#if/#ifdef/#ifndef/#elif/#else/#endif):
// the stage interface (struct fields) depends on it. #define / #undef lines
// are tracked and kept for the GLSL preprocessor (function-like macros).
struct PPExpr
{
  const std::string& s; size_t i; const std::map<std::string, std::string>& defs;
  void ws() { while (i < s.size() && isspace((unsigned char)s[i])) ++i; }
  long prim()
  {
    ws();
    if (i < s.size() && s[i] == '!') { ++i; return !prim(); }
    if (i < s.size() && s[i] == '(') { ++i; long v = orx(); ws(); if (i < s.size() && s[i] == ')') ++i; return v; }
    if (i < s.size() && (isalpha((unsigned char)s[i]) || s[i] == '_'))
    {
      size_t j = i; while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '_')) ++j;
      std::string id = s.substr(i, j - i); i = j;
      if (id == "defined")
      {
        ws(); bool par = i < s.size() && s[i] == '('; if (par) ++i; ws();
        size_t k = i; while (k < s.size() && (isalnum((unsigned char)s[k]) || s[k] == '_')) ++k;
        std::string nm = s.substr(i, k - i); i = k; ws(); if (par && i < s.size() && s[i] == ')') ++i;
        return defs.count(nm) ? 1 : 0;
      }
      auto d = defs.find(id);
      return d == defs.end() ? 0 : atol(d->second.c_str());
    }
    size_t j = i; while (j < s.size() && isdigit((unsigned char)s[j])) ++j;
    long v = atol(s.substr(i, j - i).c_str()); i = j; return v;
  }
  long cmp()
  {
    long a = prim();
    for (;;)
    {
      ws();
      if (!s.compare(i, 2, "==")) { i += 2; a = a == prim(); }
      else if (!s.compare(i, 2, "!=")) { i += 2; a = a != prim(); }
      else if (!s.compare(i, 2, ">=")) { i += 2; a = a >= prim(); }
      else if (!s.compare(i, 2, "<=")) { i += 2; a = a <= prim(); }
      else if (i < s.size() && s[i] == '>' ) { ++i; a = a > prim(); }
      else if (i < s.size() && s[i] == '<' ) { ++i; a = a < prim(); }
      else return a;
    }
  }
  long andx() { long a = cmp(); for (;;) { ws(); if (!s.compare(i, 2, "&&")) { i += 2; long b = cmp(); a = a && b; } else return a; } }
  long orx() { long a = andx(); for (;;) { ws(); if (!s.compare(i, 2, "||")) { i += 2; long b = andx(); a = a || b; } else return a; } }
};

std::string Preprocess(const std::string& src)
{
  std::map<std::string, std::string> defs;
  defs["CGC"] = "1";
  struct Level { bool parentOn, taken, on; };
  std::vector<Level> st;
  auto active = [&]() { return st.empty() || st.back().on; };
  std::string out; size_t pos = 0;
  while (pos < src.size())
  {
    size_t e = src.find('\n', pos); if (e == std::string::npos) e = src.size();
    std::string line = src.substr(pos, e - pos);
    // continuation lines
    while (!line.empty() && line.back() == '\\' && e < src.size()) { size_t e2 = src.find('\n', e + 1); if (e2 == std::string::npos) e2 = src.size(); line = line.substr(0, line.size() - 1) + src.substr(e + 1, e2 - e - 1); e = e2; }
    pos = e + 1;
    size_t k = 0; while (k < line.size() && isspace((unsigned char)line[k])) ++k;
    if (k < line.size() && line[k] == '#')
    {
      size_t a = k + 1; while (a < line.size() && isspace((unsigned char)line[a])) ++a;
      size_t b = a; while (b < line.size() && isalpha((unsigned char)line[b])) ++b;
      const std::string dir = line.substr(a, b - a), rest = line.substr(b);
      auto eval = [&](const std::string& x) { PPExpr p{ x, 0, defs }; return p.orx() != 0; };
      auto name = [&](const std::string& x) { size_t i = 0; while (i < x.size() && isspace((unsigned char)x[i])) ++i; size_t j = i; while (j < x.size() && (isalnum((unsigned char)x[j]) || x[j] == '_')) ++j; return x.substr(i, j - i); };
      if (dir == "if" || dir == "ifdef" || dir == "ifndef")
      {
        const bool par = active();
        bool c = dir == "if" ? eval(rest) : (dir == "ifdef" ? defs.count(name(rest)) > 0 : defs.count(name(rest)) == 0);
        st.push_back({ par, par && c, par && c });
        out += "\n"; continue;
      }
      if (dir == "elif") { if (!st.empty()) { Level& L = st.back(); bool c = !L.taken && L.parentOn && eval(rest); L.on = c; if (c) L.taken = true; } out += "\n"; continue; }
      if (dir == "else") { if (!st.empty()) { Level& L = st.back(); L.on = L.parentOn && !L.taken; L.taken = true; } out += "\n"; continue; }
      if (dir == "endif") { if (!st.empty()) st.pop_back(); out += "\n"; continue; }
      if (!active()) { out += "\n"; continue; }
      if (dir == "define")
      {
        const std::string nm = name(rest);
        size_t v = rest.find(nm) + nm.size();
        if (v < rest.size() && rest[v] == '(') { defs[nm] = "1"; out += line + "\n"; continue; }   // function-like: GLSL expands it
        std::string val = rest.substr(v); size_t q = 0; while (q < val.size() && isspace((unsigned char)val[q])) ++q; val = val.substr(q);
        defs[nm] = val.empty() ? "1" : val;
        out += line + "\n"; continue;
      }
      if (dir == "undef") { defs.erase(name(rest)); out += line + "\n"; continue; }
      out += "\n"; continue;   // #pragma, #line, ... dropped
    }
    out += active() ? line + "\n" : "\n";
  }
  return out;
}
} // namespace

bool Translate(const std::string& cg, bool vertex, Program& out)
{
  out = Program(); out.vertex = vertex;
  std::vector<Tok> t; Tokenize(Preprocess(cg), t);

  // ---- structs with field semantics (semantics removed from the text)
  std::map<std::string, Struct> structs;
  for (size_t i = 0; i < t.size(); ++i)
  {
    if (!(t[i].k == T_ID && t[i].s == "struct")) continue;
    const size_t nm = NextNw(t, i), br = NextNw(t, nm);
    if (br >= t.size() || t[br].s != "{") continue;
    Struct st;
    size_t j = br + 1;
    while (j < t.size() && t[j].s != "}")
    {
      // a field: TYPE NAME [: SEM] ;
      size_t a = j; while (a < t.size() && t[a].k == T_WS) ++a;
      if (a >= t.size() || t[a].s == "}") break;
      size_t semi = a; while (semi < t.size() && t[semi].s != ";" && t[semi].s != "}") ++semi;
      Field f; size_t ty = a, fn = NextNw(t, ty);
      f.type = t[ty].s; f.name = fn < semi ? t[fn].s : "";
      for (size_t k = a; k < semi; ++k)
        if (t[k].s == ":")
        {
          const size_t sm = NextNw(t, k);
          if (sm < semi) f.sem = NormSem(t[sm].s);
          for (size_t z = k; z < semi; ++z) { t[z].s = ""; t[z].k = T_WS; }   // drop ": SEM"
          break;
        }
      st.fields.push_back(f);
      j = semi + 1;
    }
    while (j < t.size() && t[j].s != "}") ++j;
    st.close = j;
    structs[t[nm].s] = st;
  }

  // ---- main(): input struct, uniforms, return type
  size_t mainId = (size_t)-1;
  for (size_t i = 0; i < t.size(); ++i)
    if (t[i].k == T_ID && t[i].s == "main") { const size_t o = NextNw(t, i); if (o < t.size() && t[o].s == "(") { mainId = i; break; } }
  if (mainId == (size_t)-1) { out.error = "no main()"; return false; }
  const size_t retTok = PrevNw(t, mainId);
  const std::string retType = retTok != (size_t)-1 ? t[retTok].s : "";
  const size_t open = NextNw(t, mainId);
  std::vector<std::pair<size_t, size_t>> args; size_t close;
  if (!SplitArgs(t, open, args, close)) { out.error = "bad main() signature"; return false; }
  std::string inStruct, inName;
  std::set<int> usedRegs;
  struct Uni { std::string type, name; int arr = 0; int reg = -1; int unit = -1; bool sampler = false; };
  std::vector<Uni> unis;
  for (auto& a : args)
  {
    std::vector<size_t> w; for (size_t k = a.first; k < a.second; ++k) if (t[k].k != T_WS) w.push_back(k);
    if (w.empty()) continue;
    if (t[w[0]].s == "uniform")
    {
      Uni u; if (w.size() < 3) continue;
      u.type = t[w[1]].s; u.name = t[w[2]].s;
      size_t p = 3;
      if (p < w.size() && t[w[p]].s == "[") { u.arr = atoi(t[w[p + 1]].s.c_str()); p += 3; }
      u.sampler = u.type.compare(0, 7, "sampler") == 0;
      for (; p < w.size(); ++p)
      {
        const std::string& s = t[w[p]].s;
        if (s == "register" && p + 2 < w.size()) { const std::string r = t[w[p + 2]].s; if (r[0] == 'c' || r[0] == 'C') u.reg = atoi(r.c_str() + 1); else if (r[0] == 's' || r[0] == 'S') u.unit = atoi(r.c_str() + 1); }
        if (!strncasecmp(s.c_str(), "texunit", 7)) u.unit = atoi(s.c_str() + 7);
        if (!strncasecmp(s.c_str(), "TEXUNIT", 7)) u.unit = atoi(s.c_str() + 7);
      }
      if (u.sampler && u.unit < 0) u.unit = 0;
      unis.push_back(u);
    }
    else if (w.size() >= 2) { inStruct = t[w[0]].s; inName = t[w[1]].s; }
  }
  // registers: explicit ones first, the rest packed into free slots
  const int maxRegs = vertex ? kVertexRegs : kFragmentRegs;
  std::vector<char> taken(maxRegs + 64, 0);
  auto size = [](const Uni& u) { int r, c; Shape(u.type, r, c); return (u.arr ? u.arr : 1) * r; };
  for (auto& u : unis) if (!u.sampler && u.reg >= 0) for (int k = 0; k < size(u); ++k) if (u.reg + k < (int)taken.size()) taken[u.reg + k] = 1;
  for (auto& u : unis)
  {
    if (u.sampler || u.reg >= 0) continue;
    const int need = size(u);
    for (int r = 0; r + need <= maxRegs; ++r)
    {
      bool ok = true; for (int k = 0; k < need; ++k) if (taken[r + k]) ok = false;
      if (ok) { u.reg = r; for (int k = 0; k < need; ++k) taken[r + k] = 1; break; }
    }
    if (u.reg < 0) { out.error = "out of registers for " + u.name; return false; }
  }
  std::map<std::string, Uni*> uniByName;
  for (auto& u : unis) uniByName[u.name] = &u;

  // main header -> cg_main(<in struct> IN); drop ": COLOR" after ')'
  {
    const size_t afterClose = NextNw(t, close);
    std::string hdr = "cg_main(" + (inStruct.empty() ? std::string() : inStruct + " " + inName) + ")";
    for (size_t k = mainId; k <= close; ++k) { t[k].s = ""; t[k].k = T_WS; }
    t[mainId].s = hdr;
    if (afterClose < t.size() && t[afterClose].s == ":")
    {
      const size_t sem = NextNw(t, afterClose);
      t[afterClose].s = ""; if (sem < t.size()) t[sem].s = "";
    }
  }

  // ---- token rewrites
  const char* arr = vertex ? "Cv" : "Cf";
  // Parameters are only visible in main()'s body (helpers may reuse names).
  size_t bodyA = close, bodyB = t.size();
  {
    while (bodyA < t.size() && t[bodyA].s != "{") ++bodyA;
    int d = 0;
    for (size_t k = bodyA; k < t.size(); ++k) { if (t[k].s == "{") ++d; else if (t[k].s == "}" && --d == 0) { bodyB = k; break; } }
  }
  auto inMain = [&](size_t i) { return i > bodyA && i < bodyB; };   // bodyB follows token insertions
  // pass 1: element swizzles (need the parameter names, replaced in pass 2)
  for (size_t i = 0; i < t.size(); ++i)
  {
    Tok& k = t[i];
    if (k.k != T_ID) continue;
    const size_t pv = PrevNw(t, i);
    const bool member = pv != (size_t)-1 && t[pv].s == ".";
    // element swizzles: X._RC... / X._mRC...
    if (member && k.s.size() >= 3 && k.s[0] == '_')
    {
      std::vector<std::pair<int, int>> el; size_t p = 0; bool ok = true;
      while (p < k.s.size())
      {
        if (k.s[p] != '_') { ok = false; break; }
        ++p; bool zero = false;
        if (p < k.s.size() && k.s[p] == 'm') { zero = true; ++p; }
        if (p + 1 >= k.s.size() || !isdigit((unsigned char)k.s[p]) || !isdigit((unsigned char)k.s[p + 1])) { ok = false; break; }
        int r = k.s[p] - '0', c = k.s[p + 1] - '0'; if (!zero) { --r; --c; }
        el.push_back({ r, c }); p += 2;
      }
      if (ok && !el.empty())
      {
        // base: the token before '.' (an identifier: a parameter or a local)
        const size_t base = PrevNw(t, pv);
        std::string b = base != (size_t)-1 ? t[base].s : "";
        auto u = inMain(i) ? uniByName.find(b) : uniByName.end();
        if (u != uniByName.end() && !u->second->sampler)
        {
          int rows, cols; Shape(u->second->type, rows, cols);
          char buf[64];
          std::string e = el.size() == 1 ? "" : "vec" + std::to_string(el.size()) + "(";
          for (size_t q = 0; q < el.size(); ++q)
          { snprintf(buf, sizeof(buf), "%s%s[%d].%c", q ? ", " : "", arr, u->second->reg + el[q].first, "xyzw"[el[q].second]); e += buf; }
          if (el.size() > 1) e += ")";
          t[base].s = e; t[pv].s = ""; k.s = "";
        }
        else
        {
          std::string e = el.size() == 1 ? "" : "vec" + std::to_string(el.size()) + "(";
          for (size_t q = 0; q < el.size(); ++q) e += (q ? ", " : "") + b + "[" + std::to_string(el[q].first) + "][" + std::to_string(el[q].second) + "]";
          if (el.size() > 1) e += ")";
          if (base != (size_t)-1) t[base].s = e;
          t[pv].s = ""; k.s = "";
        }
        continue;
      }
    }
  }
  for (size_t i = 0; i < t.size(); ++i)
  {
    Tok& k = t[i];
    if (k.k == T_NUM)
    {
      std::string s = k.s;
      const char last = s.empty() ? 0 : s.back();
      if (last == 'h' || last == 'H' || last == 'f' || last == 'F' || last == 'x' || last == 'X') s.pop_back();
      const size_t pv = PrevNw(t, i);
      const bool index = pv != (size_t)-1 && (t[pv].s == "[" || (t[pv].s == "(" && PrevNw(t, pv) != (size_t)-1 && t[PrevNw(t, pv)].s.size() > 3 && !t[PrevNw(t, pv)].s.compare(t[PrevNw(t, pv)].s.size() - 3, 3, "int") && isdigit((unsigned char)t[PrevNw(t, pv)].s[0])));
      if (!index && s.find('.') == std::string::npos && s.find('e') == std::string::npos && s.find('E') == std::string::npos) s += ".0";
      if (!s.empty() && s.back() == '.') s += "0";
      if (!s.empty() && s[0] == '.') s = "0" + s;
      k.s = s;
      continue;
    }
    if (k.k != T_ID) continue;
    const size_t pv = PrevNw(t, i);
    const bool member = pv != (size_t)-1 && t[pv].s == ".";
    if (member) continue;
    // parameters -> register reads
    auto u = inMain(i) ? uniByName.find(k.s) : uniByName.end();
    if (u != uniByName.end() && !u->second->sampler)
    {
      const Uni& U = *u->second;
      int rows, cols; Shape(U.type, rows, cols);
      static const char* const sw[] = { "", ".x", ".xy", ".xyz", "" };
      char buf[256];
      if (U.arr)
      {
        // name[i] -> Cv[reg + (i) * rows]  (array of vectors; rows == 1 in practice)
        const size_t ob = NextNw(t, i);
        if (ob < t.size() && t[ob].s == "[")
        {
          // Cv [ reg + rows * ( index ) ]swizzle - the index stays its own tokens
          size_t cb = ob + 1; int d = 0;
          for (; cb < t.size(); ++cb) { if (t[cb].s == "[") ++d; else if (t[cb].s == "]") { if (d == 0) break; --d; } }
          k.s = arr;
          snprintf(buf, sizeof(buf), "%d + %d * int", U.reg, rows);
          if (cb < t.size())
          {
            t[cb].s = std::string("]") + sw[cols];
            t.insert(t.begin() + cb, Tok{ T_PUNCT, ")" });
            t.insert(t.begin() + ob + 1, Tok{ T_PUNCT, "(" });
            t.insert(t.begin() + ob + 1, Tok{ T_PUNCT, buf });
            if (ob < bodyB) bodyB += 3;
          }
        }
        continue;
      }
      if (rows == 1) { snprintf(buf, sizeof(buf), "%s[%d]%s", arr, U.reg, sw[cols]); k.s = buf; }
      else
      {
        std::string e = MapType(U.type) + "(";
        for (int r = 0; r < rows; ++r) { snprintf(buf, sizeof(buf), "%s%s[%d]%s", r ? ", " : "", arr, U.reg + r, sw[cols]); e += buf; }
        k.s = e + ")";
      }
      continue;
    }
    const std::string m = MapType(k.s);
    if (!m.empty()) { k.s = m; continue; }
    if (k.s == "lerp") k.s = "mix";
    else if (k.s == "dot") k.s = "cg_dot";
    else if (k.s == "pow") k.s = "cg_pow";
    else if (k.s == "step") k.s = "cg_step";
    else if (k.s == "frac") k.s = "fract";
    else if (k.s == "rsqrt") k.s = "inversesqrt";
    else if (k.s == "uniform" || k.s == "inline" || k.s == "static") k.s = "";
  }
  // Fields the engine writes without declaring them (OUT.Tex3 = clip-plane
  // distance added by the engine): declare as float4 TEXCOORDn.
  if (structs.count(retType))
  {
    Struct& rs = structs[retType];
    for (size_t i = 0; i + 2 < t.size(); ++i)
    {
      if (!(t[i].k == T_ID && t[i].s == "OUT" && t[i + 1].s == "." && t[i + 2].k == T_ID)) continue;
      const std::string fn = t[i + 2].s;
      bool have = false; for (auto& f : rs.fields) if (f.name == fn) have = true;
      if (have || fn.size() != 4 || fn.compare(0, 3, "Tex") || !isdigit((unsigned char)fn[3])) continue;
      rs.fields.push_back({ "float4", fn, "TEXCOORD" + fn.substr(3) });
      t[rs.close].s = "  vec4 " + fn + ";\n}";
    }
  }
  // Scalar swizzles (s.xxx, (0.5).xx): not in GLSL ES -> vecN(s).
  {
    std::set<std::string> scalars;
    for (size_t i = 0; i + 1 < t.size(); ++i)
      if (t[i].k == T_ID && t[i].s == "float")
      {
        const size_t nm = NextNw(t, i);
        if (nm < t.size() && t[nm].k == T_ID) { const size_t a = NextNw(t, nm); if (a < t.size() && t[a].s != "(") scalars.insert(t[nm].s); }
      }
    for (size_t i = 2; i < t.size(); ++i)
    {
      if (t[i].k != T_ID || t[i - 1].s != ".") continue;
      const std::string& w = t[i].s;
      if (w.size() < 2 || w.size() > 4 || w.find_first_not_of(w[0] == 'x' ? "x" : "r") != std::string::npos) continue;
      const size_t b = i - 2;
      std::string vt = "vec" + std::to_string(w.size());
      // In arithmetic a replicated scalar is just the scalar (GLSL broadcasts it
      // to the other operand's size, as Cg's truncation would end up).
      const size_t bp = PrevNw(t, b), an = NextNw(t, i);
      auto arith = [&](size_t k) { return k < t.size() && (t[k].s == "*" || t[k].s == "+" || t[k].s == "-" || t[k].s == "/"); };
      const bool inArith = (bp != (size_t)-1 && arith(bp)) || arith(an);
      const std::string wrapA = inArith ? "(" : vt + "(";
      if (t[b].k == T_NUM) { t[b].s = wrapA + t[b].s + ")"; t[i - 1].s = ""; t[i].s = ""; }
      else if (t[b].k == T_ID && scalars.count(t[b].s) && (b == 0 || t[b - 1].s != "."))
      { t[b].s = wrapA + t[b].s + ")"; t[i - 1].s = ""; t[i].s = ""; }
      else if (t[b].s == ")")
      {
        // ( number ) or ( scalar )
        size_t o = b; { int dd = 0; for (; o > 0; --o) { if (t[o].s == ")") ++dd; else if (t[o].s == "(" && --dd == 0) break; } }
        std::string inner; for (size_t k = o + 1; k < b; ++k) if (t[k].k != T_WS) inner += t[k].s;
        bool num = !inner.empty() && inner.find_first_not_of("0123456789.-") == std::string::npos;
        if (num || scalars.count(inner)) { t[o].s = vt + "("; t[i - 1].s = ""; t[i].s = ""; }
        else { t[o].s = "cg_rep" + std::to_string(w.size()) + "("; t[i - 1].s = ""; t[i].s = ""; }   // type unknown: overloads
      }
    }
  }
  // C-style casts "(type)x" -> "type(x)".
  {
    auto isType = [](const std::string& w) {
      return w == "float" || w == "int" || w == "bool" || (w.size() >= 4 && (!w.compare(0, 3, "vec") || !w.compare(0, 3, "mat"))) ||
             (w.size() == 5 && (!w.compare(0, 4, "ivec") || !w.compare(0, 4, "bvec")));
    };
    auto matchClose = [&](size_t o) -> size_t {
      int d = 0;
      for (size_t k = o; k < t.size(); ++k) { if (t[k].s == "(" || t[k].s == "[") ++d; else if (t[k].s == ")" || t[k].s == "]") { if (--d == 0) return k; } }
      return t.size();
    };
    for (size_t i = 0; i + 3 < t.size(); ++i)
    {
      if (t[i].s != "(") continue;
      const size_t ty = NextNw(t, i); if (ty >= t.size() || t[ty].k != T_ID || !isType(t[ty].s)) continue;
      const size_t cl = NextNw(t, ty); if (cl >= t.size() || t[cl].s != ")") continue;
      const size_t q = NextNw(t, cl); if (q >= t.size()) continue;
      // a cast only when an operand follows (not "(float) + x" or a declaration list)
      size_t end;
      if (t[q].s == "(") end = matchClose(q);
      else if (t[q].k == T_ID || t[q].k == T_NUM)
      {
        end = q;
        const size_t c2 = NextNw(t, end);
        if (t[q].k == T_ID && c2 < t.size() && t[c2].s == "(") end = matchClose(c2);
      }
      else continue;
      if (end >= t.size()) continue;
      // postfix: .member, [index]
      for (;;)
      {
        const size_t nx = NextNw(t, end);
        if (nx + 1 < t.size() && t[nx].s == "." && t[nx + 1].k == T_ID) { end = nx + 1; continue; }
        if (nx < t.size() && t[nx].s == "[") { end = matchClose(nx); if (end >= t.size()) break; continue; }
        break;
      }
      t[i].s = ""; t[cl].s = "("; t.insert(t.begin() + end + 1, Tok{ T_PUNCT, ")" });
    }
  }
  // Cg promotes scalars (and truncates vectors) on assignment:
  // "v.xyz = s;" / "vec3 v = s;" -> right side through cg_vN() overloads.
  std::map<std::string, int> retCols;    // fields of main's return struct (OUT.x)
  if (structs.count(retType)) for (auto& f : structs[retType].fields) { int r, c; Shape(f.type, r, c); if (r == 1 && c >= 2) retCols[f.name] = c; }
  std::map<std::string, int> fieldCols;  // struct field -> vector size (unambiguous names only)
  {
    std::map<std::string, std::set<int>> seen;
    for (auto& st : structs) for (auto& f : st.second.fields) { int r, c; Shape(f.type, r, c); if (r == 1) seen[f.name].insert(c); }
    for (auto& e : seen) if (e.second.size() == 1 && *e.second.begin() >= 2) fieldCols[e.first] = *e.second.begin();
  }
  std::map<std::string, int> vecVars;   // local "vecN name" declarations
  for (size_t i = 0; i + 1 < t.size(); ++i)
    if (t[i].k == T_ID && t[i].s.size() == 4 && t[i].s.compare(0, 3, "vec") == 0)
    { const size_t nm = NextNw(t, i); if (nm < t.size() && t[nm].k == T_ID) vecVars[t[nm].s] = t[i].s[3] - '0'; }
  for (size_t i = 1; i + 1 < t.size(); ++i)
  {
    if (t[i].s != "=" ) continue;
    const std::string& pr = t[i - 1].s; const std::string& nx = t[i + 1].s;
    if (nx == "=" || pr == "=" || pr == "!" || pr == "<" || pr == ">" || pr == "+" || pr == "-" || pr == "*" || pr == "/") continue;
    const size_t l = PrevNw(t, i);
    if (l == (size_t)-1 || t[l].k != T_ID) continue;
    int n = 0;
    const size_t d = PrevNw(t, l);
    if (d != (size_t)-1 && t[d].s == ".")
    {
      const std::string& w = t[l].s;
      if (w.size() >= 2 && w.size() <= 4 && w.find_first_not_of("xyzwrgba") == std::string::npos) n = (int)w.size();
      else if (PrevNw(t, d) != (size_t)-1 && t[PrevNw(t, d)].s == "OUT" && retCols.count(w)) n = retCols[w];
      else if (fieldCols.count(w)) n = fieldCols[w];
    }
    else if (d != (size_t)-1 && t[d].k == T_ID && t[d].s.size() == 4 && t[d].s.compare(0, 3, "vec") == 0) n = t[d].s[3] - '0';
    else if (d != (size_t)-1 && t[d].s == "." && PrevNw(t, d) != (size_t)-1 && t[PrevNw(t, d)].s == "OUT" && retCols.count(t[l].s)) n = retCols[t[l].s];
    else if (d != (size_t)-1 && t[d].s == "." && fieldCols.count(t[l].s)) n = fieldCols[t[l].s];
    else if (vecVars.count(t[l].s) && (d == (size_t)-1 || (!t[d].s.empty() && (t[d].s.back() == ';' || t[d].s.back() == '{' || t[d].s.back() == '}'))))
    {
      // nearest preceding declaration of the name (locals are per function)
      for (size_t k = l; k-- > 0;)
        if (t[k].k == T_ID && t[k].s == t[l].s)
        {
          const size_t ty = PrevNw(t, k);
          if (ty == (size_t)-1) break;
          if (t[ty].s == "float") { n = 1; break; }
          if (t[ty].s.size() == 4 && !t[ty].s.compare(0, 3, "vec")) { n = t[ty].s[3] - '0'; break; }
        }
    }
    if (n < 2) continue;
    size_t e = i + 1; int depth = 0;
    for (; e < t.size(); ++e) { const std::string& q = t[e].s; if (q == "(" || q == "[") ++depth; else if (q == ")" || q == "]") --depth; else if ((q == ";" || q == ",") && depth == 0) break; if (depth < 0) break; }
    if (e >= t.size()) continue;
    const std::string fn = "cg_v" + std::to_string(n) + "(";
    const size_t r0 = NextNw(t, i);
    if (r0 < e && t[r0].s == "vec" + std::to_string(n)) continue;   // already that type (also keeps const initializers constant)
    size_t qm = 0, co = 0; depth = 0;
    for (size_t k = i + 1; k < e; ++k) { const std::string& q = t[k].s; if (q == "(" || q == "[") ++depth; else if (q == ")" || q == "]") --depth; else if (depth == 0 && q == "?") qm = k; else if (depth == 0 && q == ":" && qm) co = k; }
    if (qm && co) { t[qm].s = "? " + fn; t[co].s = ") : " + fn; }
    t[i].s = "= " + fn;
    t[e].s = (qm && co ? "))" : ")") + t[e].s;
  }
  std::string body = RewriteMul(Join(t, 0, t.size()));

  // ---- interface and wrapper main()
  std::string g = "#version 300 es\n";
  g += vertex ? "precision highp float;\n" : "precision highp float;\nprecision highp sampler2D;\nprecision highp samplerCube;\n";
  char buf[256];
  snprintf(buf, sizeof(buf), "uniform vec4 %s[%d];\n", arr, maxRegs); g += buf;
  for (auto& u : unis)
    if (u.sampler) { g += "uniform " + MapType(u.type) + " " + u.name + ";\n"; out.samplers.push_back({ u.name, MapType(u.type), u.unit }); }
    else
    {
      Param p; p.name = u.name; p.type = u.type; p.reg = u.reg; int r, c; Shape(u.type, r, c); p.count = (u.arr ? u.arr : 1) * r;
      out.params.push_back(p);
    }
  const Struct* in = structs.count(inStruct) ? &structs[inStruct] : nullptr;
  const Struct* ret = structs.count(retType) ? &structs[retType] : nullptr;
  // Interface names shared with the native fixed-function programs
  // (GLRPrograms.cpp), so Cg and generated stages pair in any combination:
  // attributes aPos aColor aNormal aTex0-3; varyings vColor vColor1 vTex0-7 vFogDist.
  auto attrName = [](const std::string& sem) -> std::string {
    if (sem == "POSITION") return "aPos";
    if (sem == "COLOR0") return "aColor";
    if (sem == "NORMAL") return "aNormal";
    if (sem == "COLOR1") return "aColor1";
    if (sem.size() == 9 && !sem.compare(0, 8, "TEXCOORD") && sem[8] >= '0' && sem[8] <= '7') return std::string("aTex") + sem[8];
    return "";
  };
  auto varName = [](const std::string& sem) -> std::string {
    if (sem == "COLOR0") return "vColor";
    if (sem == "COLOR1") return "vColor1";
    if (sem == "FOG") return "vFogDist";
    if (sem.size() == 9 && !sem.compare(0, 8, "TEXCOORD") && sem[8] >= '0' && sem[8] <= '7') return std::string("vTex") + sem[8];
    return "";
  };
  static const char* const sw[] = { "", ".x", ".xy", ".xyz", "" };
  std::string pre, post;
  if (vertex)
  {
    if (in) for (auto& f : in->fields)
    {
      if (f.sem.empty()) continue;
      const std::string a = attrName(f.sem);
      if (a.empty()) { out.error = "unsupported vertex input " + f.sem; return false; }
      g += "in " + MapType(f.type) + " " + a + ";\n";
      out.inputs.push_back({ f.sem, MapType(f.type), f.name });
      pre += "  IN." + f.name + " = " + a + ";\n";
    }
    // Everything a generated fragment stage may read gets a value.
    g += "out vec4 vColor; out vec4 vColor1; out vec4 vTex0; out vec4 vTex1; out vec4 vTex2; out vec4 vTex3; out float vFogDist;\n";
    post += "  vColor = vec4(1.0); vColor1 = vec4(0.0); vTex0 = vTex1 = vTex2 = vTex3 = vec4(0.0, 0.0, 0.0, 1.0); vFogDist = 0.0;\n";
    std::set<std::string> extra;
    if (ret) for (auto& f : ret->fields)
    {
      if (f.sem == "POSITION") { post += "  gl_Position = toV4(OUT." + f.name + ");\n"; continue; }
      if (f.sem.empty() || f.sem == "PSIZE") continue;
      const std::string v = varName(f.sem);
      if (v.empty()) { out.error = "unsupported vertex output " + f.sem; return false; }
      out.outputs.push_back({ f.sem, MapType(f.type), f.name });
      int r, c; Shape(f.type, r, c);
      if (v == "vFogDist") post += "  vFogDist = toV4(OUT." + f.name + ").x;\n";
      else if (v == "vColor" || v == "vColor1") post += "  " + v + " = clamp(toV4(OUT." + f.name + "), 0.0, 1.0);\n";   // ARB clamps colour outputs
      else
      {
        if (v[4] >= '4' && extra.insert(v).second) g += "out vec4 " + v + ";\n";
        post += "  " + v + " = toV4(OUT." + f.name + ");\n";
      }
    }
  }
  else
  {
    if (in) for (auto& f : in->fields)
    {
      if (f.sem.empty() || f.sem == "POSITION") continue;
      const std::string v = varName(f.sem);
      if (v.empty()) { out.error = "unsupported fragment input " + f.sem; return false; }
      g += v == "vFogDist" ? "in float vFogDist;\n" : "in vec4 " + v + ";\n";
      out.inputs.push_back({ f.sem, MapType(f.type), f.name });
      int r, c; Shape(f.type, r, c);
      pre += "  IN." + f.name + " = " + (v == "vFogDist" ? std::string("cg_v") + std::to_string(c < 2 ? 2 : c) + "(vFogDist)" + (c == 1 ? ".x" : "") : v + sw[c]) + ";\n";
    }
    g += "uniform float uAlphaRef;\nout vec4 o_Color;\n";
    if (ret) { for (auto& f : ret->fields) if (f.sem == "COLOR0") post += "  o_Color = toV4(OUT." + f.name + ");\n"; }
    else post += "  o_Color = toV4(OUT);\n";
    // Fixed-function alpha test (still applies after an ARB fragment
    // program): the pairing code defines GLR_ALPHA_TEST to the comparison.
    post += "#ifdef GLR_ALPHA_TEST\n  if (!(GLR_ALPHA_TEST)) discard;\n#endif\n";
  }
  g += UsedPrelude(kPrelude, body + pre + post);
  g += body;
  g += "\nvoid main() {\n";
  if (!inStruct.empty()) g += "  " + inStruct + " IN;\n";
  g += pre;
  g += "  " + (ret ? retType : std::string("vec4")) + " OUT = cg_main(" + (inStruct.empty() ? "" : "IN") + ");\n";
  g += post + "}\n";
  out.glsl = g;
  return true;
}

// ---------------------------------------------------------------- program strings
namespace
{
// GLSL / Cg text inside the ARB string: one comment line each, '#' and
// newlines escaped, so the engine's '#'-separated parser never sees them.
std::string Escape(const std::string& s)
{
  std::string o;
  for (char c : s)
  {
    if (c == '#') o += "\\h";
    else if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') {}
    else o += c;
  }
  return o;
}
std::string Unescape(const std::string& s)
{
  std::string o;
  for (size_t i = 0; i < s.size(); ++i)
  {
    if (s[i] == '\\' && i + 1 < s.size())
    {
      const char n = s[++i];
      o += n == 'h' ? '#' : (n == 'n' ? '\n' : n);
    }
    else o += s[i];
  }
  return o;
}
} // namespace

std::string MakeProgramString(const Program& p, const std::string& cg)
{
  std::string s = p.vertex ? "!!ARBvp1.0\n" : "!!ARBfp1.0\n";
  char buf[256];
  for (const Param& q : p.params)
  {
    snprintf(buf, sizeof(buf), "#var %s %s :  : c[%d], %d : %d : 1\n", q.type.c_str(), q.name.c_str(), q.reg, q.count, q.count);
    s += buf;
  }
  snprintf(buf, sizeof(buf), "#GLRCG %d\n", kVersion); s += buf;
  s += "#GLRCG-CG " + Escape(cg) + "\n";
  s += "#GLRCG-GLSL " + Escape(p.glsl) + "\n";
  s += "END\n";
  return s;
}

bool ParseProgramString(const std::string& text, Program& out)
{
  const bool vertex = text.find("!!ARBvp1.0") != std::string::npos;
  const size_t v = text.find("#GLRCG ");
  if (v == std::string::npos) return false;
  const int ver = atoi(text.c_str() + v + 7);
  auto line = [&](const char* tag) -> std::string {
    const size_t a = text.find(tag);
    if (a == std::string::npos) return std::string();
    const size_t b = text.find('\n', a);
    return Unescape(text.substr(a + strlen(tag), (b == std::string::npos ? text.size() : b) - a - strlen(tag)));
  };
  const std::string cg = line("#GLRCG-CG ");
  // An older translation (engine program cache): translate the Cg again.
  if (ver != kVersion && !cg.empty()) return Translate(cg, vertex, out);
  Program p; p.vertex = vertex;
  p.glsl = line("#GLRCG-GLSL ");
  if (p.glsl.empty()) return Translate(cg, vertex, out);
  // Interface information is recomputed from the Cg (cheap, and keeps the
  // string format small); the GLSL above is used as is.
  Program q;
  if (!cg.empty() && Translate(cg, vertex, q)) { q.glsl = p.glsl; out = q; return true; }
  out = p;
  return true;
}
} // namespace GLRCg
