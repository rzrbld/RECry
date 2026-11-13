/*=============================================================================
  GLAudit.cpp : GL call audit (see GLAudit.h).
=============================================================================*/
#include "GLAudit.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace
{
const int kMaxIds = 2048;
struct Entry
{
  const char* name = nullptr;
  bool forward = false;      // native mode: forwarded although audited
  uint64_t calls = 0;
  std::unordered_map<void*, uint64_t> sites;
};
Entry* s_entries = nullptr;
uint64_t s_frames = 0;
int s_enabled = -1;
bool s_trap = false;
std::string s_path;

void Report()
{
  if (!s_entries) return;
  FILE* f = fopen(s_path.c_str(), "w");
  if (!f) { fprintf(stderr, "GLAudit: cannot write %s\n", s_path.c_str()); return; }
  fprintf(f, "# GL audit: %llu frames\n# function\tcalls\tcalls_per_frame\tsites\n", (unsigned long long)s_frames);
  std::vector<int> ids;
  for (int i = 0; i < kMaxIds; ++i) if (s_entries[i].calls) ids.push_back(i);
  std::sort(ids.begin(), ids.end(), [](int a, int b) { return s_entries[a].calls > s_entries[b].calls; });
  for (int id : ids)
  {
    const Entry& e = s_entries[id];
    fprintf(f, "F\t%s\t%llu\t%.2f\t%zu\n", e.name ? e.name : "?", (unsigned long long)e.calls,
            s_frames ? (double)e.calls / (double)s_frames : 0.0, e.sites.size());
    std::vector<std::pair<void*, uint64_t>> sites(e.sites.begin(), e.sites.end());
    std::sort(sites.begin(), sites.end(), [](const std::pair<void*, uint64_t>& a, const std::pair<void*, uint64_t>& b) { return a.second > b.second; });
    for (auto& s : sites)
    {
      Dl_info info; memset(&info, 0, sizeof(info));
      const bool ok = dladdr(s.first, &info) && info.dli_fbase;
      // Module offset of the instruction after the call (addr2line wants the call: offset - 1).
      fprintf(f, "S\t%s\t%s\t0x%llx\t%llu\n", e.name ? e.name : "?", ok && info.dli_fname ? info.dli_fname : "?",
              (unsigned long long)(ok ? (uintptr_t)s.first - (uintptr_t)info.dli_fbase : (uintptr_t)s.first),
              (unsigned long long)s.second);
    }
  }
  fclose(f);
  fprintf(stderr, "GLAudit: report written to %s (%zu functions, %llu frames)\n", s_path.c_str(), ids.size(), (unsigned long long)s_frames);
}
} // namespace

bool GLAudit_Enabled()
{
  if (s_enabled < 0)
  {
    const char* v = getenv("FARCRY_GL_AUDIT");
    const char* r = getenv("FARCRY_RENDERER");
    s_trap = r && !strcmp(r, "native");
    if (s_trap && !(v && *v && strcmp(v, "0"))) v = "1";   // native mode always audits
    s_enabled = (v && *v && strcmp(v, "0")) ? 1 : 0;
    if (s_enabled)
    {
      s_path = strcmp(v, "1") ? v : "logs/gl-audit.txt";
      s_entries = new Entry[kMaxIds];
      atexit(Report);
      fprintf(stderr, "GLAudit: %s engine GL calls, report at exit to %s\n", s_trap ? "trapping (native renderer)" : "auditing", s_path.c_str());
    }
  }
  return s_enabled == 1;
}

// Native mode: rendering calls are trapped; queries (glGet*, glIs*) and
// platform entry points (wgl*) only read or manage the context and keep
// working (still counted), so start-up sees real limits and strings.
bool GLAudit_Trap(int id)
{
  if (!s_trap) return false;
  if (!s_entries || id < 0 || id >= kMaxIds) return true;
  return !s_entries[id].forward;
}

void GLAudit_Name(int id, const char* name)
{
  if (!s_entries || id < 0 || id >= kMaxIds) return;
  s_entries[id].name = name;
  const char* n = name[0] == 'p' ? name + 1 : name;
  s_entries[id].forward = !strncmp(n, "glGet", 5) || !strncmp(n, "glIs", 4) || !strncmp(n, "wgl", 3);
}

void GLAudit_Hit(int id, void* site)
{
  if (!s_entries || id < 0 || id >= kMaxIds) return;
  Entry& e = s_entries[id];
  ++e.calls;
  ++e.sites[site];
}

void GLAudit_Frame() { if (s_entries && ++s_frames % 600 == 0) Report(); }   // periodic: device runs may be killed
