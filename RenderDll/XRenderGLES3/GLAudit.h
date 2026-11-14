/*=============================================================================
  GLAudit.h : GL call audit for the native backend rework (stage 0).

  FARCRY_GL_AUDIT=<file> (or 1 for logs/gl-audit.txt) wraps every entry of
  the engine's GL function table (GLFuncs.h) after FindProcs resolved it:
  each wrapper counts the call per function and per call site (return
  address) and forwards to the resolved function, so the game runs as usual.
  FARCRY_RENDERER=native turns the same wrappers into traps: calls are
  counted but not forwarded (the native backend owns GL; anything the old
  backend still calls shows up in the report instead of being emulated).
  "Zero trapped calls" on a demo is the coverage criterion of each stage.
  The report written at exit lists, per function, calls per frame and the
  call sites as module offsets; devtools/gl_audit_report.py resolves them to
  source lines. It is the measured work list for the native backend: every
  call site listed must end up issuing native state instead of GL calls.
=============================================================================*/
#pragma once

bool GLAudit_Enabled();
bool GLAudit_Trap(int id); // native mode: do not forward (queries and platform calls still forward)
void GLAudit_Hit(int id, void* site);
void GLAudit_Name(int id, const char* name);
void GLAudit_Frame();      // once per presented frame

// Native renderer with a render thread: forwarded calls (queries) made on
// another thread run on the render thread and wait for it.
int GLAudit_Remote(int id);   // 0: call here, 1: run on the render thread, 2: answer the default (glGetError: no error)
void GLAudit_RunRemote(int id, void* site, void (*fn)(void*), void* arg);
template <typename R> struct GLAuditRemote
{
  template <typename F> static R Call(int id, void* site, F& f) { struct C { F* f; R r; } c{ &f, R() }; GLAudit_RunRemote(id, site, [](void* p) { C* c = (C*)p; c->r = (*c->f)(); }, &c); return c.r; }
};
template <> struct GLAuditRemote<void>
{
  template <typename F> static void Call(int id, void* site, F& f) { GLAudit_RunRemote(id, site, [](void* p) { (*(F*)p)(); }, &f); }
};

// Wrapper for one GL table entry, generated from the pointer's own type.
template <int ID, typename F> struct GLAuditW;
template <int ID, typename R, typename... A> struct GLAuditW<ID, R (*)(A...)>
{
  static R (*real)(A...);
  static R fn(A... a)
  {
    GLAudit_Hit(ID, __builtin_return_address(0));
    if (GLAudit_Trap(ID)) return R();
    if (const int m = GLAudit_Remote(ID))
    {
      if (m == 2) return R();
      auto f = [&]() { return real(a...); }; return GLAuditRemote<R>::Call(ID, __builtin_return_address(0), f);
    }
    return real(a...);
  }
};
template <int ID, typename R, typename... A> R (*GLAuditW<ID, R (*)(A...)>::real)(A...) = nullptr;

// Wraps the table entry 'func' in place (no-op for null entries).
#define GLAUDIT_WRAP(func) \
  { enum { kId = __COUNTER__ }; typedef GLAuditW<kId, decltype(func)> W_; \
    if (func) { W_::real = func; func = &W_::fn; GLAudit_Name(kId, #func); } }
