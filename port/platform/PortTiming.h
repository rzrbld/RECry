/* Per-frame phase timing for performance work (written into the GLES3
 * renderer's frame log, FARCRY_GLES_FRAMELOG). Accumulators are reset after
 * each logged frame. Cheap enough to stay enabled. */
#pragma once
#include <time.h>
#include <stdlib.h>

enum EPortPhase { PORT_PHASE_SYS, PORT_PHASE_GAME, PORT_PHASE_RENDER, PORT_PHASE_END, PORT_PHASE_SWAP, PORT_PHASE_COUNT };
extern double g_PortPhaseMs[PORT_PHASE_COUNT];
// PortNowMs() when the local player first existed outside menus after a level
// load (0 = not yet). With FARCRY_BENCH_RELATIVE=1, FARCRY_CMD_AT,
// FARCRY_CAPTURE_TIMES and FARCRY_GLES_FRAMELOG_WARMUP count from here, so
// benchmarks measure gameplay whatever the level's load time.
extern double g_PortGameplayStartMs;
inline bool PortBenchRelative() { static const bool b = getenv("FARCRY_BENCH_RELATIVE") != 0; return b; }

inline double PortNowMs()
{
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1.0e6;
}

struct SPortPhaseScope
{
  int phase; double t0;
  explicit SPortPhaseScope(int p) : phase(p), t0(PortNowMs()) {}
  ~SPortPhaseScope() { g_PortPhaseMs[phase] += PortNowMs() - t0; }
};
