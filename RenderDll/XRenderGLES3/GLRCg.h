/*=============================================================================
  GLRCg.h : Far Cry Cg programs -> GLSL ES 3.00 (native backend, materials).

  The engine assembles Cg source per shader technique and expects compiled
  ARB program text back (cgCreateProgram / cgGetProgramString), from whose
  '#var' comment lines it learns each parameter's register; it then sets
  parameters with glProgramEnvParameter4fvARB(register).
  GLRCg translates the Cg source to GLSL ES and produces that ARB-style text
  itself: the '#var' lines carry the registers (explicit register(cN) kept,
  the rest assigned), and the GLSL plus the original Cg travel inside the
  text as escaped comment lines, so the engine's on-disk program cache stays
  self-contained (a newer translator re-translates from the embedded Cg).
  In the GLSL, parameters read from one uniform array per stage (Cv for
  vertex programs, Cf for fragment programs), indexed by register.
  No GL calls here (used by the offline test tool too).
=============================================================================*/
#pragma once
#include <string>
#include <vector>

namespace GLRCg
{
const int kVersion = 5;                  // bump when the translation changes
const int kVertexRegs = 96, kFragmentRegs = 32;

struct Param { std::string name, type; int reg = -1, count = 1; bool explicitReg = false; };
struct Sampler { std::string name, type; int unit; };
// Vertex inputs / stage interface by semantic (POSITION, NORMAL, COLOR0, COLOR1, TEXCOORDn, FOG).
struct Io { std::string semantic, type, field; };

struct Program
{
  bool vertex = true;
  std::string glsl;                      // GLSL ES 3.00 source
  std::vector<Param> params;
  std::vector<Sampler> samplers;
  std::vector<Io> inputs, outputs;       // vertex: attributes / varyings; fragment: varyings / -
  std::string error;
};

// Translate one program (profile: vertex or fragment). False on failure (error set).
bool Translate(const std::string& cg, bool vertex, Program& out);

// The ARB-style text handed back to the engine (cgGetProgramString), and the
// reverse: recover the GLSL (re-translating when the embedded version is old).
std::string MakeProgramString(const Program& p, const std::string& cg);
bool ParseProgramString(const std::string& text, Program& out);
}
