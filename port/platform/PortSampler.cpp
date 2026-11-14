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
#include <pthread.h>
#include "PortTiming.h"

extern "C" char __executable_start;

namespace
{
  enum { kMaxDepth = 32, kMaxSamples = 30000 };
  struct SSample { uint8_t depth; void* pc[kMaxDepth]; };
  // [0] main thread, [1] the renderer's thread (registered by it)
  SSample* s_samples[2] = { NULL, NULL };
  volatile int s_count[2] = { 0, 0 };
  volatile bool s_active = false;
  timer_t s_timer[2];
  bool s_hasTimer[2] = { false, false };
  volatile pid_t s_tid[2] = { 0, 0 };
  pthread_t s_renderThread;
  double s_startMs = 0, s_seconds = 20;
  const char* s_path = NULL;
  bool s_done = false;

  void OnSample(int, siginfo_t*, void*)
  {
    if (!s_active) return;
    const int t = (pid_t)syscall(SYS_gettid) == s_tid[1] ? 1 : 0;
    const int i = s_count[t];
    if (i >= kMaxSamples) return;
    s_samples[t][i].depth = (uint8_t)backtrace(s_samples[t][i].pc, kMaxDepth);
    s_count[t] = i + 1;
  }

  void WriteOne(int t, const char* path)
  {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "base %lx\n", (unsigned long)(uintptr_t)&__executable_start);
    for (int i = 0; i < s_count[t]; ++i)
    {
      // Skip the handler and the signal trampoline frames.
      for (int d = 2; d < s_samples[t][i].depth; ++d)
        fprintf(f, "%lx ", (unsigned long)(uintptr_t)s_samples[t][i].pc[d]);
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
    fprintf(stderr, "PORT sampler: %d samples written to %s\n", (int)s_count[t], path);
  }
  void Write()
  {
    s_active = false;
    for (int t = 0; t < 2; ++t) if (s_hasTimer[t]) timer_delete(s_timer[t]);
    WriteOne(0, s_path);
    if (s_hasTimer[1]) { char p[1024]; snprintf(p, sizeof(p), "%s.render", s_path); WriteOne(1, p); }
  }
  bool StartTimer(int t, clockid_t clock, pid_t tid, long periodNs)
  {
    struct sigevent sev; memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev._sigev_un._tid = tid;
    if (timer_create(clock, &sev, &s_timer[t]) != 0) return false;
    s_samples[t] = (SSample*)calloc(kMaxSamples, sizeof(SSample));
    s_hasTimer[t] = true;
    struct itimerspec its; memset(&its, 0, sizeof(its));
    its.it_interval.tv_nsec = its.it_value.tv_nsec = periodNs;
    timer_settime(s_timer[t], 0, &its, NULL);
    return true;
  }
}

// Called by the renderer's own thread when it starts (sampled as well).
void PortSamplerRegisterRenderThread()
{
  s_renderThread = pthread_self();
  s_tid[1] = (pid_t)syscall(SYS_gettid);
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
    void* warm[4]; backtrace(warm, 4); // load the unwinder outside the handler
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = OnSample; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    const bool wall = getenv("FARCRY_SAMPLE_WALL") && atoi(getenv("FARCRY_SAMPLE_WALL")) != 0; // include blocked time
    s_tid[0] = (pid_t)syscall(SYS_gettid);
    s_startMs = PortNowMs();
    s_active = true;
    if (!StartTimer(0, wall ? CLOCK_MONOTONIC : CLOCK_THREAD_CPUTIME_ID, s_tid[0], periodNs)) { s_done = true; s_active = false; return; }
    // The render thread (if one registered): its own CPU clock, samples to <file>.render.
    if (s_tid[1])
    {
      clockid_t c = CLOCK_MONOTONIC;
      if (!wall) pthread_getcpuclockid(s_renderThread, &c);
      StartTimer(1, c, s_tid[1], periodNs);
    }
    fprintf(stderr, "PORT sampler: profiling main%s thread every %.1f ms for %.0f s\n", s_hasTimer[1] ? " and render" : "", periodNs / 1e6, s_seconds);
    return;
  }
  if (s_active && (PortNowMs() - s_startMs) / 1000.0 >= s_seconds)
  {
    s_done = true;
    Write();
  }
}
