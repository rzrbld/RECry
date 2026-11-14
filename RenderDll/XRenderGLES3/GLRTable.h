/*=============================================================================
  GLRTable.h : the engine's GL function table on the native backend.

  The engine calls GL through a table of function pointers (GLFuncs.h),
  resolved in CGLRenderer::FindProcs. The native backend owns GL on its own
  thread and the engine's renderer code talks to it through GLRBridge.h, so
  after resolution every table entry is replaced:
    - queries (glGet*, glIs*): answered by the backend where it owns the
      state (matrices, viewport, fog, the fixed-function limits the engine
      expects), else asked of the driver on the render thread;
    - platform entries (wgl*): kept (SDL context bookkeeping, swap);
    - everything else: a no-op stub. Engine code that still called GL
      directly would show up here: FARCRY_GL_STUBS=1 lists such calls at exit.
  Resolution itself is unchanged (extension support flags depend on it):
  GLRTableKnown() names the entry points the old fixed-function layer
  provided, so they resolve as before.
=============================================================================*/
#pragma once

bool GLRTableKnown(const char* name);          // entry point the backend covers (resolution only)
void* GLRTableKnownPlaceholder();
void* GLRTableNative(const char* name);        // native query implementation, or null
bool GLRTableIsQuery(const char* name);
void GLRTableName(int id, const char* name);
void GLRTableStubbed(int id);
void GLRTableRunOnRenderThread(void (*fn)(void*), void* arg);

template <int ID, typename F> struct GLRStub;
template <int ID, typename R, typename... A> struct GLRStub<ID, R (*)(A...)>
{
  static R fn(A...) { GLRTableStubbed(ID); return R(); }
};

// Driver query on the render thread (which owns the context).
template <typename R> struct GLRRemote
{
  template <typename F> static R Call(F& f) { struct C { F* f; R r; } c{ &f, R() }; GLRTableRunOnRenderThread([](void* p) { C* c = (C*)p; c->r = (*c->f)(); }, &c); return c.r; }
};
template <> struct GLRRemote<void>
{
  template <typename F> static void Call(F& f) { GLRTableRunOnRenderThread([](void* p) { (*(F*)p)(); }, &f); }
};
template <int ID, typename F> struct GLRQuery;
template <int ID, typename R, typename... A> struct GLRQuery<ID, R (*)(A...)>
{
  static R (*real)(A...);
  static R fn(A... a) { auto f = [&]() { return real(a...); }; return GLRRemote<R>::Call(f); }
};
template <int ID, typename R, typename... A> R (*GLRQuery<ID, R (*)(A...)>::real)(A...) = nullptr;

// Replaces one table entry (see above).
#define GLRTABLE_SET(func) \
  { enum { kId = __COUNTER__ }; const char* n_ = #func; if (n_[0] == 'p') ++n_; GLRTableName(kId, n_); \
    if (!func) {}   /* null entries stay null: the engine tests some for availability */ \
    else if (void* nat = GLRTableNative(n_)) func = (decltype(func))nat; \
    else if (!strncmp(n_, "wgl", 3)) {} \
    else if (GLRTableIsQuery(n_) && func && (void*)func != GLRTableKnownPlaceholder()) { GLRQuery<kId, decltype(func)>::real = func; func = &GLRQuery<kId, decltype(func)>::fn; } \
    else func = &GLRStub<kId, decltype(func)>::fn; }
