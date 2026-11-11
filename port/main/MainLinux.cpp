// Far Cry Linux entry point. Loads the engine as a single native binary.
#include "platform.h"
#include <ISystem.h>
#include <IGame.h>
#include <ILog.h>
#include <IConsole.h>
#include <IInput.h>
#include <stdio.h>
#include <string.h>
#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define FARCRY_HAVE_BACKTRACE 1
#  endif
#endif
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>

// Opt-in crash backtrace (FARCRY_CRASH_HANDLER=1) for cross/device debugging.
static void FarCryCrashHandler(int sig)
{
#ifdef FARCRY_HAVE_BACKTRACE
    void* bt[64];
    int n = backtrace(bt, 64);
    const char* msg = "\n=== FARCRY CRASH BACKTRACE ===\n";
    write(2, msg, strlen(msg));
    backtrace_symbols_fd(bt, n, 2);
#else
    const char* msg = "\n=== FARCRY CRASH (no backtrace support on this libc) ===\n";
    write(2, msg, strlen(msg));
#endif
    _exit(128 + sig);
}

extern "C" void PortReleaseUnusedHeap();
extern "C" ISystem* CreateSystemInterface(SSystemInitParams& initParams);

int main(int argc, char** argv)
{
    if (getenv("FARCRY_CRASH_HANDLER")) {
        signal(SIGSEGV, FarCryCrashHandler);
        signal(SIGABRT, FarCryCrashHandler);
        signal(SIGBUS, FarCryCrashHandler);
    }
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (n > 0) {
        exe[n] = 0;
        char* slash = strrchr(exe, '/');
        if (slash) { *slash = 0; setenv("MODULE_PATH", exe, 1); }
    }
    const char* dataDir = getenv("FARCRY_DATA");
    if (dataDir && *dataDir) { if (chdir(dataDir) != 0) { fprintf(stderr, "error: cannot chdir to %s\n", dataDir); return 3; } }
    else if (n > 0) { char* slash = strrchr(exe, '/'); if (slash) *slash = 0; chdir(exe); }

    char cmd[2048]; cmd[0] = 0;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) strncat(cmd, " ", sizeof(cmd)-strlen(cmd)-1);
        strncat(cmd, argv[i], sizeof(cmd)-strlen(cmd)-1);
    }

    SSystemInitParams sip;
    sip.sLogFileName = "log.txt";
    strncpy(sip.szSystemCmdLine, cmd, sizeof(sip.szSystemCmdLine)-1);
    sip.bDedicatedServer = false;

    ISystem* sys = CreateSystemInterface(sip);
    if (!sys) { fprintf(stderr, "CreateSystemInterface failed\n"); return 1; }
    sys->GetILog()->EnableVerbosity(true);

    SGameInitParams gip;
    gip.sGameDLL = "crygame.so";
    strncpy(gip.szGameCmdLine, cmd, sizeof(gip.szGameCmdLine)-1);
    if (!sys->CreateGame(gip)) { fprintf(stderr, "CreateGame failed\n"); sys->Release(); return 2; }

    sys->GetIInput()->ClearKeyState();
    if (getenv("FARCRY_NOCULL")) {
        sys->GetIConsole()->ExecuteString("e_terrain_occlusion_culling 0");
        sys->GetIConsole()->ExecuteString("e_cbuffer 0");
        sys->GetIConsole()->ExecuteString("e_hw_occlusion_culling_objects 0");
        sys->GetIConsole()->ExecuteString("e_occlusion_volumes 0");
    }
    if (getenv("FARCRY_LOAD_CHECKPOINT_ONLY")) {
        sys->GetIConsole()->ExecuteString("load_lastcheckpoint");
    }
    const char* mapName = getenv("FARCRY_MAP");
    if (mapName && *mapName) {
        char cmdMap[256];
        snprintf(cmdMap, sizeof(cmdMap), "map %s", mapName);
        fprintf(stderr, "FARCRY_MAP: executing '%s'\n", cmdMap);
        sys->GetIConsole()->ExecuteString(cmdMap);
    }
    if (getenv("FARCRY_LOAD_CHECKPOINT")) {
        sys->GetIConsole()->ExecuteString("load_lastcheckpoint");
    }
    if (getenv("FARCRY_SKIP_CUTSCENE")) {
        sys->GetIConsole()->ExecuteString("SkipCutScene");
    }
    if (getenv("FARCRY_TEST_CONSOLE")) {
        sys->GetIConsole()->ShowConsole(true);
        sys->GetIConsole()->PrintLine("GLES console font test 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    }
    PortReleaseUnusedHeap();
    IGame* game = sys->GetIGame();
    bool bRelaunch = false;
    game->Run(bRelaunch);

    sys->Release();
    return 0;
}
