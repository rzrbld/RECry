/* On-device sampling profiler for performance work on handhelds without perf
 * or gdb. FARCRY_SAMPLE_PROFILE=<file> samples the main thread's call stack
 * every FARCRY_SAMPLE_MS (default 2) ms of its CPU time, starting when gameplay
 * begins, for FARCRY_SAMPLE_SECONDS (default 20) s of wall time, then writes:
 *   base <hex load address of the executable>
 *   <hex pc> <hex pc> ...      one line per sample, innermost frame first
 * Symbolise offline with addr2line against the unstripped build
 * (address - base for this PIE). Off unless the variable is set. */
#include <execinfo.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "PortTiming.h"

extern "C" char __executable_start;

namespace
{
  enum { kMaxDepth = 32, kMaxSamples = 30000 };
  struct SSample { uint8_t depth; void* pc[kMaxDepth]; };
  SSample* s_samples = NULL;
  volatile int s_count = 0;
  volatile bool s_active = false;
  timer_t s_timer;
  double s_startMs = 0, s_seconds = 20;
  const char* s_path = NULL;
  bool s_done = false;

  void OnSample(int, siginfo_t*, void*)
  {
    if (!s_active) return;
    const int i = s_count;
    if (i >= kMaxSamples) return;
    s_samples[i].depth = (uint8_t)backtrace(s_samples[i].pc, kMaxDepth);
    s_count = i + 1;
  }

  void Write()
  {
    s_active = false;
    timer_delete(s_timer);
    FILE* f = fopen(s_path, "w");
    if (!f) return;
    fprintf(f, "base %lx\n", (unsigned long)(uintptr_t)&__executable_start);
    for (int i = 0; i < s_count; ++i)
    {
      // Skip the handler and the signal trampoline frames.
      for (int d = 2; d < s_samples[i].depth; ++d)
        fprintf(f, "%lx ", (unsigned long)(uintptr_t)s_samples[i].pc[d]);
      fputc('\n', f);
    }
    // Library mappings, to attribute samples outside the executable.
    if (FILE* m = fopen("/proc/self/maps", "r"))
    {
      char line[512];
      while (fgets(line, sizeof(line), m)) if (strstr(line, " r-xp ") || strstr(line, " r-x")) fprintf(f, "map %s", line);
      fclose(m);
    }
    fclose(f);
    fprintf(stderr, "PORT sampler: %d samples written to %s\n", (int)s_count, s_path);
  }
}

// Called every game frame from the main thread.
void PortSamplerPoll()
{
  if (s_done || g_PortGameplayStartMs == 0) return;
  static bool s_init = false;
  if (!s_init)
  {
    // FARCRY_SAMPLE_DELAY: seconds after gameplay start before sampling.
    static const double s_delay = getenv("FARCRY_SAMPLE_DELAY") ? atof(getenv("FARCRY_SAMPLE_DELAY")) : 0.0;
    if ((PortNowMs() - (double)g_PortGameplayStartMs) / 1000.0 < s_delay) return;
    s_init = true;
    s_path = getenv("FARCRY_SAMPLE_PROFILE");
    if (!s_path || !*s_path) { s_done = true; return; }
    if (const char* v = getenv("FARCRY_SAMPLE_SECONDS")) s_seconds = atof(v);
    const char* ms = getenv("FARCRY_SAMPLE_MS");
    const long periodNs = (long)((ms ? atof(ms) : 2.0) * 1e6);
    s_samples = (SSample*)calloc(kMaxSamples, sizeof(SSample));
    void* warm[4]; backtrace(warm, 4); // load the unwinder outside the handler
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = OnSample; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct sigevent sev; memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
    const bool wall = getenv("FARCRY_SAMPLE_WALL") && atoi(getenv("FARCRY_SAMPLE_WALL")) != 0; // include blocked time
    if (timer_create(wall ? CLOCK_MONOTONIC : CLOCK_THREAD_CPUTIME_ID, &sev, &s_timer) != 0) { s_done = true; return; }
    struct itimerspec its; memset(&its, 0, sizeof(its));
    its.it_interval.tv_nsec = its.it_value.tv_nsec = periodNs;
    s_startMs = PortNowMs();
    s_active = true;
    timer_settime(s_timer, 0, &its, NULL);
    fprintf(stderr, "PORT sampler: profiling main thread every %.1f ms for %.0f s\n", periodNs / 1e6, s_seconds);
    return;
  }
  if (s_active && (PortNowMs() - s_startMs) / 1000.0 >= s_seconds)
  {
    s_done = true;
    Write();
  }
}
