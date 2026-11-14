// Port diagnostic: FARCRY_SLOW_CALL_MS=<ms> logs calls of instrumented engine
// functions (decals, particles, sounds) slower than that.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include "PortTiming.h"
struct SPortSlowScope
{
  const char* what; const char* detail; double t;
  static double Threshold() { static const double v = getenv("FARCRY_SLOW_CALL_MS") ? atof(getenv("FARCRY_SLOW_CALL_MS")) : 0.0; return v; }
  SPortSlowScope(const char* w, const char* d = "") : what(w), detail(d ? d : ""), t(Threshold() > 0 ? PortNowMs() : 0) {}
  ~SPortSlowScope() { if (Threshold() > 0) { const double ms = PortNowMs() - t; if (ms >= Threshold()) fprintf(stderr, "PORT slow call %.1f ms: %s %s\n", ms, what, detail); } }
};
