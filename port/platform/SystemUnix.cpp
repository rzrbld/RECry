#include "StdAfx.h"
#include "System.h"
#include <ISystem.h>
#include <ILog.h>
#include <pthread.h>
#include <unistd.h>
#include <pwd.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <I3DEngine.h>
#include <ICryAnimation.h>
#include <IPhysics.h>
#include <IScriptSystem.h>
#include <IEntitySystem.h>
#include <IGame.h>
#include <CrySizer.h>
#include <IRenderer.h>
#include "CryPak.h"
#include "RefStreamEngine.h"
#include <set>
#include <map>
#include <string>
#include <vector>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

void CSystem::SetAffinity()
{
    // Keep the engine on all cores; the scheduler handles the H700 big/LITTLE mix.
#if defined(__linux__)
    cpu_set_t set; CPU_ZERO(&set);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    for (long i = 0; i < n && i < CPU_SETSIZE; ++i) CPU_SET((int)i, &set);
    sched_setaffinity(0, sizeof(set), &set);
#endif
}

class PortMemorySizer : public ICrySizer {
public:
    std::set<const void*> seen;
    std::vector<std::string> names;
    std::map<std::string, size_t> totals;
    bool AddObject(const void* p, size_t bytes) {
        if (!p || !bytes || !seen.insert(p).second) return false;
        std::string name;
        for (size_t i=0; i<names.size(); ++i) {
            if (!name.empty()) name += "/";
            name += names[i];
            totals[name] += bytes;
        }
        return true;
    }
    void Push(const char* name) { names.push_back(name); }
    void PushSubcomponent(const char* name) { Push(name); }
    void Pop() { if (!names.empty()) names.pop_back(); }
};
void CSystem::DumpMemoryUsageStatistics() {
    PortMemorySizer s;
    if (m_pIPak) { s.Push("Pak"); m_pIPak->GetMemoryStatistics(&s); s.Pop(); }
    if (m_pStreamEngine) { s.Push("Stream"); m_pStreamEngine->GetMemoryStatistics(&s); s.Pop(); }
    if (m_pRenderer) { s.Push("Renderer"); m_pRenderer->GetMemoryUsage(&s); s.Pop(); }
    if (m_pI3DEngine) { s.Push("3D"); m_pI3DEngine->GetMemoryUsage(&s); s.Pop(); }
    if (m_pICryCharManager) { s.Push("Animation"); m_pICryCharManager->GetMemoryUsage(&s); s.Pop(); }
    if (m_pIPhysicalWorld) { s.Push("Physics"); m_pIPhysicalWorld->GetMemoryStatistics(&s); s.Pop(); }
    if (m_pScriptSystem) { s.Push("Script"); m_pScriptSystem->GetMemoryStatistics(&s); s.Pop(); }
    if (m_pEntitySystem) { s.Push("Entities"); m_pEntitySystem->GetMemoryStatistics(&s); s.Pop(); }
    if (m_pGame) { s.Push("Game"); m_pGame->GetMemoryStatistics(&s); s.Pop(); }
    for (auto it=s.totals.begin(); it!=s.totals.end(); ++it)
        if (it->second >= 1024*1024) CryLogAlways("PORTMEM subsystem %s = %llu KiB", it->first.c_str(), (unsigned long long)(it->second/1024));
}
void CSystem::CollectMemStats(CrySizerImpl*, MemStatsPurposeEnum) {}
void CSystem::TickMemStats(MemStatsPurposeEnum) {}
int  CSystem::DumpMMStats(bool) { return 0; }
void CSystem::DebugStats(bool, bool) {}
void CSystem::GetExeSizes(ICrySizer*, MemStatsPurposeEnum) {}
void CSystem::DumpWinHeaps() {}

const char* CSystem::GetUserName()
{
    static char name[128];
    const char* u = getenv("USER");
    if (!u) { struct passwd* pw = getpwuid(getuid()); u = pw ? pw->pw_name : "player"; }
    snprintf(name, sizeof(name), "%s", u);
    return name;
}

void CSystem::Error(const char* format, ...)
{
    char buf[4096];
    va_list args; va_start(args, format); vsnprintf(buf, sizeof(buf), format, args); va_end(args);
    if (m_pLog) m_pLog->LogError("%s", buf);
    fprintf(stderr, "Error: %s\n", buf);
}

void CSystem::LogCallStack() {}

bool CSystem::GetSSFileInfo(const char* inszFileName, char* outszInfo, const DWORD indwBufferSize)
{
    if (outszInfo && indwBufferSize) outszInfo[0] = 0;
    return false;
}

// Loading creates large temporary buffers. Return freed pages at loading
// boundaries, never in the normal frame loop or the audio callback.
extern "C" void PortReleaseUnusedHeap()
{
#if defined(__GLIBC__)
    const char* enabled = getenv("FARCRY_TRIM_HEAP");
    if (!enabled || strcmp(enabled, "0") != 0) malloc_trim(0);
#endif
}
