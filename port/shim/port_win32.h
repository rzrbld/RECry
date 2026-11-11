// Portable Win32 subset used by the CryEngine Linux/AArch64 port.
// Implemented with POSIX primitives. No rendering or graphics translation here.
#pragma once

#include "LinuxSpecific.h"
#include "Linux64Specific.h"

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <ctype.h>
#include <limits.h>
#include <dlfcn.h>
#include <sched.h>
#include <sys/sysinfo.h>

#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#ifndef _MAX_PATH
#define _MAX_PATH MAX_PATH
#endif

// LinuxSpecific.h already supplies DWORD, LONG, BOOL, HANDLE, OVERLAPPED, etc.
#if !defined(HRESULT_DEFINED_BY_CRY)
typedef int32_t HRESULT;
#endif
typedef int HANDLE_INT;
typedef struct _RECT { long left, top, right, bottom; } RECT, *LPRECT;
#ifndef HMODULE_DEFINED
#define HMODULE_DEFINED
typedef void* HMODULE;
#endif

#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT  258
#define WAIT_FAILED   0xFFFFFFFFu
#define WAIT_ABANDONED 128
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_NO_MORE_FILES 18
#define ERROR_ALREADY_EXISTS 183

#define GENERIC_READ  0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define CREATE_NEW 1
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5
#ifndef FILE_ATTRIBUTE_NORMAL
#define FILE_ATTRIBUTE_NORMAL 0x80
#endif
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_READONLY 0x01
#define FILE_ATTRIBUTE_HIDDEN 0x02
#define FILE_FLAG_OVERLAPPED 0x40000000
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)

#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2

#define CREATE_SUSPENDED 0x00000004
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define THREAD_PRIORITY_NORMAL 0
#define REALTIME_PRIORITY_CLASS 0x00000100
#define MAXIMUM_WAIT_OBJECTS 64

typedef struct _FILETIME { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME, *LPFILETIME;
typedef struct _SYSTEMTIME {
  WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;

typedef pthread_mutex_t CRITICAL_SECTION;
typedef CRITICAL_SECTION* LPCRITICAL_SECTION;

struct port_object;
typedef struct port_object* EVENT_HANDLE;
typedef struct port_object* THREAD_HANDLE;
typedef struct port_object* PORT_OBJECT;

#ifndef _WIN32_FIND_DATA_DEFINED
#define _WIN32_FIND_DATA_DEFINED
typedef struct _WIN32_FIND_DATAA {
  DWORD dwFileAttributes;
  FILETIME ftCreationTime;
  FILETIME ftLastAccessTime;
  FILETIME ftLastWriteTime;
  DWORD nFileSizeHigh;
  DWORD nFileSizeLow;
  DWORD dwReserved0;
  DWORD dwReserved1;
  char cFileName[MAX_PATH];
  char cAlternateFileName[14];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;
#endif

// ---- case-insensitive path resolution (retail assets use mixed case) ----
inline int port_resolve_path_nocase(const char* in, char* out, size_t outsz)
{
  if (!in || !*in) return -1;
  if (in[0] == '/' ) {
    // absolute: still resolve components case-insensitively
  }
  // Fast path: exact file exists.
  if (access(in, F_OK) == 0) {
    snprintf(out, outsz, "%s", in);
    return 0;
  }
  char cur[PATH_MAX];
  cur[0] = '\0';
  const char* p = in;
  if (*p == '/') { strncat(cur, "/", sizeof(cur)-1); p++; }
  while (*p) {
    while (*p == '/') p++;
    if (!*p) break;
    const char* q = p;
    while (*q && *q != '/') q++;
    char comp[NAME_MAX + 1];
    size_t n = (size_t)(q - p);
    if (n > NAME_MAX) return -1;
    memcpy(comp, p, n); comp[n] = '\0';
    char next[PATH_MAX];
    if (cur[0]) snprintf(next, sizeof(next), "%s/%s", cur, comp);
    else snprintf(next, sizeof(next), "%s", comp);
    struct stat st;
    if (stat(next, &st) == 0) {
      snprintf(cur, sizeof(cur), "%s", next);
    } else if (cur[0] && stat(cur, &st) == 0 && S_ISDIR(st.st_mode)) {
      // search directory entries case-insensitively
      DIR* d = opendir(cur);
      bool found = false;
      if (d) {
        struct dirent* de;
        while ((de = readdir(d))) {
          if (strcasecmp(de->d_name, comp) == 0) {
            snprintf(next, sizeof(next), "%s/%s", cur, de->d_name);
            if (stat(next, &st) == 0) { snprintf(cur, sizeof(cur), "%s", next); found = true; }
            break;
          }
        }
        closedir(d);
      }
      if (!found) return -1;
    } else {
      return -1;
    }
    p = q;
  }
  snprintf(out, outsz, "%s", cur);
  return 0;
}

inline FILE* fopen_nocase(const char* file, const char* mode)
{
  FILE* f = fopen(file, mode);
  if (f) return f;
  char resolved[PATH_MAX];
  if (port_resolve_path_nocase(file, resolved, sizeof(resolved)) == 0)
    return fopen(resolved, mode);
  return NULL;
}

// ---- file handles ------------------------------------------------------
inline HANDLE_INT port_handle_from_fd(int fd) { return fd; }
inline int port_fd(HANDLE_INT h) { return (int)h; }

inline HANDLE_INT CreateFile(const char* name, DWORD access, DWORD share, void*, DWORD creation, DWORD flags, void*)
{
  (void)share; (void)flags;
  char resolved[PATH_MAX];
  if (port_resolve_path_nocase(name, resolved, sizeof(resolved)) != 0 && creation == OPEN_EXISTING)
    return -1;
  int oflags = 0;
  if (access & GENERIC_WRITE) {
    if (access & GENERIC_READ) oflags = O_RDWR; else oflags = O_WRONLY;
  } else oflags = O_RDONLY;
  switch (creation) {
    case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; break;
    case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
    case OPEN_ALWAYS: oflags |= O_CREAT; break;
    case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
    default: break;
  }
  int fd = open(resolved, oflags, 0666);
  return (HANDLE_INT)fd;
}

inline BOOL CloseHandle(HANDLE_INT h) { return close((int)h) == 0 ? TRUE : FALSE; }
inline BOOL ReadFile(HANDLE_INT h, void* buf, DWORD n, DWORD* lpRead, OVERLAPPED* ov)
{
  (void)ov;
  ssize_t r = ::read((int)h, buf, n);
  if (r < 0) { if (lpRead) *lpRead = 0; return FALSE; }
  if (lpRead) *lpRead = (DWORD)r;
  return TRUE;
}
inline BOOL WriteFile(HANDLE_INT h, const void* buf, DWORD n, DWORD* written, OVERLAPPED* ov)
{
  (void)ov;
  ssize_t r = write((int)h, buf, n);
  if (r < 0) { if (written) *written = 0; return FALSE; }
  if (written) *written = (DWORD)r;
  return TRUE;
}
inline DWORD GetFileSize(HANDLE_INT h, DWORD* high)
{
  struct stat st;
  if (fstat((int)h, &st) != 0) return 0xFFFFFFFFu;
  if (high) *high = (DWORD)((unsigned long long)st.st_size >> 32);
  return (DWORD)(st.st_size & 0xFFFFFFFFu);
}
inline DWORD SetFilePointer(HANDLE_INT h, LONG lo, LONG* hi, DWORD method)
{
  int whence = method == FILE_BEGIN ? SEEK_SET : (method == FILE_CURRENT ? SEEK_CUR : SEEK_END);
  off_t off = (off_t)lo;
  if (hi) off = (off_t)(((unsigned long long)(DWORD)lo) | (((unsigned long long)(DWORD)*hi) << 32));
  off_t r = lseek((int)h, off, whence);
  if (r < 0) return 0xFFFFFFFFu;
  if (hi) *hi = (LONG)((unsigned long long)r >> 32);
  return (DWORD)(r & 0xFFFFFFFFu);
}
inline BOOL GetFileTime(HANDLE_INT h, FILETIME*, FILETIME*, FILETIME* last)
{
  struct stat st;
  if (!last || fstat((int)h, &st) != 0) return FALSE;
  unsigned long long t = (unsigned long long)st.st_mtime * 10000000ULL + 116444736000000000ULL;
  last->dwLowDateTime = (DWORD)t; last->dwHighDateTime = (DWORD)(t >> 32);
  return TRUE;
}
inline LONG CompareFileTime(const FILETIME* a, const FILETIME* b)
{
  unsigned long long x = ((unsigned long long)a->dwHighDateTime << 32) | a->dwLowDateTime;
  unsigned long long y = ((unsigned long long)b->dwHighDateTime << 32) | b->dwLowDateTime;
  return x < y ? -1 : (x > y ? 1 : 0);
}
inline DWORD GetFileAttributes(const char* name)
{
  char resolved[PATH_MAX];
  if (port_resolve_path_nocase(name, resolved, sizeof(resolved)) != 0) return INVALID_FILE_ATTRIBUTES;
  struct stat st;
  if (stat(resolved, &st) != 0) return INVALID_FILE_ATTRIBUTES;
  return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
inline BOOL DeleteFile(const char* name) { return unlink(name) == 0 ? TRUE : FALSE; }
inline BOOL MoveFile(const char* a, const char* b) { return rename(a, b) == 0 ? TRUE : FALSE; }
inline BOOL CopyFile(const char* a, const char* b, BOOL failIfExists)
{
  if (failIfExists && access(b, F_OK) == 0) return FALSE;
  FILE* in = fopen(a, "rb"); if (!in) return FALSE;
  FILE* out = fopen(b, "wb"); if (!out) { fclose(in); return FALSE; }
  char buf[65536]; size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
  fclose(in); fclose(out); return TRUE;
}
inline BOOL CreateDirectory(const char* path, void*) { return mkdir(path, 0777) == 0 || errno == EEXIST; }
inline BOOL RemoveDirectory(const char* path) { return rmdir(path) == 0; }
inline DWORD GetCurrentDirectory(DWORD n, char* buf)
{
  if (!buf || n == 0) {
    char* d = getcwd(NULL, 0);
    if (!d) return 0;
    DWORD len = (DWORD)strlen(d) + 1;
    free(d);
    return len;
  }
  if (!getcwd(buf, n)) return 0;
  return (DWORD)strlen(buf);
}
inline BOOL SetCurrentDirectory(const char* path) { return chdir(path) == 0; }
inline DWORD GetModuleFileName(void*, char* buf, DWORD n)
{
  ssize_t r = readlink("/proc/self/exe", buf, n - 1);
  if (r <= 0) { if (n) buf[0] = 0; return 0; }
  buf[r] = 0;
  return (DWORD)r;
}
inline DWORD GetTickCount() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (DWORD)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }
inline void Sleep(DWORD ms) { struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (ms % 1000) * 1000000L; nanosleep(&ts, NULL); }
inline void SleepEx(DWORD ms, BOOL) { Sleep(ms); }

// ---- time --------------------------------------------------------------
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000LL; return TRUE; }
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c)
{
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  c->QuadPart = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
  return TRUE;
}

// ---- critical sections -------------------------------------------------
inline void InitializeCriticalSection(CRITICAL_SECTION* cs)
{
  pthread_mutexattr_t a; pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(cs, &a); pthread_mutexattr_destroy(&a);
}
inline void DeleteCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_destroy(cs); }
inline void EnterCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_lock(cs); }
inline void LeaveCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_unlock(cs); }

// ---- events and threads ------------------------------------------------
struct port_object {
  int kind; // 0 = event, 1 = thread
  bool manual;
  bool signaled;
  pthread_mutex_t m;
  pthread_cond_t c;
  pthread_t t;
  void* (*fn)(void*);
  void* arg;
  bool suspended;
};

static inline void* port_thread_trampoline(void* p)
{
  port_object* o = (port_object*)p;
  pthread_mutex_lock(&o->m);
  while (o->suspended) pthread_cond_wait(&o->c, &o->m);
  pthread_mutex_unlock(&o->m);
  return o->fn(o->arg);
}

inline EVENT_HANDLE CreateEvent(void*, BOOL manual, BOOL initial, const char*)
{
  port_object* o = new port_object();
  memset(o, 0, sizeof(*o));
  o->kind = 0; o->manual = manual != 0; o->signaled = initial != 0;
  pthread_mutex_init(&o->m, NULL); pthread_cond_init(&o->c, NULL);
  return o;
}
inline BOOL SetEvent(EVENT_HANDLE e)
{
  pthread_mutex_lock(&e->m); e->signaled = true; pthread_cond_broadcast(&e->c); pthread_mutex_unlock(&e->m);
  return TRUE;
}
inline BOOL ResetEvent(EVENT_HANDLE e)
{
  pthread_mutex_lock(&e->m); e->signaled = false; pthread_mutex_unlock(&e->m);
  return TRUE;
}
inline THREAD_HANDLE CreateThread(void*, size_t, void* (*fn)(void*), void* arg, DWORD flags, DWORD* id)
{
  port_object* o = new port_object();
  memset(o, 0, sizeof(*o));
  o->kind = 1; o->fn = fn; o->arg = arg; o->suspended = (flags & CREATE_SUSPENDED) != 0;
  pthread_mutex_init(&o->m, NULL); pthread_cond_init(&o->c, NULL);
  pthread_create(&o->t, NULL, port_thread_trampoline, o);
  if (id) *id = (DWORD)(uintptr_t)o->t;
  return o;
}
inline DWORD ResumeThread(THREAD_HANDLE t)
{
  pthread_mutex_lock(&t->m); t->suspended = false; pthread_cond_broadcast(&t->c); pthread_mutex_unlock(&t->m);
  return 0;
}
inline DWORD WaitForSingleObject(PORT_OBJECT o, DWORD ms)
{
  if (o->kind == 1) { pthread_join(o->t, NULL); return WAIT_OBJECT_0; }
  pthread_mutex_lock(&o->m);
  if (!o->signaled) {
    if (ms == INFINITE) { while (!o->signaled) pthread_cond_wait(&o->c, &o->m); }
    else {
      struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      while (!o->signaled) { if (pthread_cond_timedwait(&o->c, &o->m, &ts) == ETIMEDOUT) break; }
    }
  }
  bool got = o->signaled;
  if (got && !o->manual) o->signaled = false;
  pthread_mutex_unlock(&o->m);
  return got ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
inline DWORD WaitForSingleObjectEx(PORT_OBJECT o, DWORD ms, BOOL) { return WaitForSingleObject(o, ms); }
inline BOOL CloseHandle(PORT_OBJECT o)
{
  if (o->kind == 0) { pthread_mutex_destroy(&o->m); pthread_cond_destroy(&o->c); }
  else { pthread_mutex_destroy(&o->m); pthread_cond_destroy(&o->c); }
  delete o; return TRUE;
}
inline DWORD GetCurrentThreadId() { return (DWORD)(uintptr_t)pthread_self(); }
inline DWORD GetCurrentProcessId() { return (DWORD)getpid(); }
inline void ExitThread(DWORD) { pthread_exit(NULL); }
inline void SetThreadPriority(THREAD_HANDLE, int) {}
inline void SetPriorityClass(HANDLE_INT, DWORD) {}
inline DWORD GetPriorityClass(HANDLE_INT) { return 0; }
inline int GetThreadPriority(THREAD_HANDLE) { return 0; }
inline DWORD_PTR SetThreadAffinityMask(THREAD_HANDLE, DWORD_PTR) { return 1; }
inline BOOL GetProcessAffinityMask(HANDLE_INT, DWORD_PTR* p, DWORD_PTR* s) { if (p) *p = 1; if (s) *s = 1; return TRUE; }

// ---- misc --------------------------------------------------------------
inline DWORD GetLastError() { return (DWORD)errno; }
inline void SetLastError(DWORD e) { errno = (int)e; }
inline DWORD GetVersion() { return 0; }
inline HMODULE LoadLibrary(const char*) { return NULL; }
inline void* GetProcAddress(HMODULE, const char*) { return NULL; }
inline BOOL FreeLibrary(HMODULE) { return TRUE; }
inline void OutputDebugString(const char* s) { fprintf(stderr, "%s", s); }
inline int MessageBox(void*, const char* t, const char* c, unsigned) { fprintf(stderr, "%s: %s\n", c, t); return 0; }

inline BOOL SetFileAttributes(const char* name, DWORD) { char r[PATH_MAX]; if (port_resolve_path_nocase(name, r, sizeof(r)) == 0) return chmod(r, 0666) == 0; return FALSE; }
inline void* GetModuleHandle(const char*) { return NULL; }

// ---- MSVC CRT directory helpers used by CSystem::Deltree ----------------
struct __finddata64_t {
  unsigned attrib;
  long long time_create;
  long long time_access;
  long long time_write;
  long long size;
  char name[260];
};
#define _A_SUBDIR 0x10
struct _find_ctx { DIR* d; char pattern[260]; char path[PATH_MAX]; bool first; };
inline bool port_wild_match(const char* pat, const char* name)
{
  while (*pat) {
    if (*pat == '*') {
      pat++;
      if (!*pat) return true;
      for (const char* n = name; *n; ++n) if (port_wild_match(pat, n)) return true;
      return false;
    }
    if (*pat == '?') { if (!*name) return false; }
    else if (tolower((unsigned char)*pat) != tolower((unsigned char)*name)) return false;
    pat++; if (*name) name++;
  }
  return *name == 0;
}
inline intptr_t _findfirst64(const char* spec, __finddata64_t* fd)
{
  _find_ctx* c = new _find_ctx();
  char specCopy[PATH_MAX]; snprintf(specCopy, sizeof(specCopy), "%s", spec);
  char* slash = strrchr(specCopy, '/');
  char* bslash = strrchr(specCopy, '\\');
  if (!slash || (bslash && bslash > slash)) slash = bslash;
  if (slash) { *slash = 0; snprintf(c->pattern, sizeof(c->pattern), "%s", slash + 1); }
  else { snprintf(c->pattern, sizeof(c->pattern), "%s", specCopy); specCopy[0] = 0; }
  char resolved[PATH_MAX];
  if (specCopy[0] && port_resolve_path_nocase(specCopy, resolved, sizeof(resolved)) == 0)
    snprintf(c->path, sizeof(c->path), "%s", resolved);
  else
    snprintf(c->path, sizeof(c->path), "%s", specCopy[0] ? specCopy : ".");
  c->d = opendir(c->path);
  c->first = true;
  if (!c->d) { delete c; return -1; }
  struct dirent* de;
  while ((de = readdir(c->d))) {
    if (!port_wild_match(c->pattern, de->d_name)) continue;
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", c->path, de->d_name);
    struct stat st;
    if (stat(full, &st) != 0) continue;
    snprintf(fd->name, sizeof(fd->name), "%s", de->d_name);
    fd->attrib = S_ISDIR(st.st_mode) ? _A_SUBDIR : 0;
    fd->size = st.st_size;
    c->first = false;
    return (intptr_t)c;
  }
  closedir(c->d); delete c; return -1;
}
inline int _findnext64(intptr_t h, __finddata64_t* fd)
{
  if (h == -1 || h == 0 || !fd) { errno = EINVAL; return -1; }
  _find_ctx* c = (_find_ctx*)h;
  struct dirent* de;
  while ((de = readdir(c->d))) {
    if (!port_wild_match(c->pattern, de->d_name)) continue;
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", c->path, de->d_name);
    struct stat st;
    if (stat(full, &st) != 0) continue;
    snprintf(fd->name, sizeof(fd->name), "%s", de->d_name);
    fd->attrib = S_ISDIR(st.st_mode) ? _A_SUBDIR : 0;
    fd->size = st.st_size;
    return 0;
  }
  return -1;
}
inline int _findclose(intptr_t h) { if (h == -1 || h == 0) { errno = EINVAL; return -1; } _find_ctx* c = (_find_ctx*)h; if (c->d) closedir(c->d); delete c; return 0; }
inline int _findclose64(intptr_t h) { return _findclose(h); }

inline int comparePathNames(const char* a, const char* b, size_t n)
{
  for (size_t i = 0; i < n; ++i) {
    char ca = a[i], cb = b[i];
    if (ca == '\\') ca = '/'; if (cb == '\\') cb = '/';
    int d = tolower((unsigned char)ca) - tolower((unsigned char)cb);
    if (d) return d;
    if (!ca) return 0;
  }
  return 0;
}
inline int memicmp(const void* a, const void* b, size_t n)
{
  const unsigned char* x = (const unsigned char*)a; const unsigned char* y = (const unsigned char*)b;
  for (size_t i = 0; i < n; ++i) { int d = tolower(x[i]) - tolower(y[i]); if (d) return d; }
  return 0;
}

// ---- additional CRT / Win32 helpers -----------------------------------
#ifndef INVALID_FILE_SIZE
#define INVALID_FILE_SIZE 0xFFFFFFFFu
#endif
typedef void* HINTERNET;
inline DWORD timeGetTime() { return GetTickCount(); }
inline char* _fullpath(char* abs, const char* rel, size_t maxlen)
{
  char resolved[PATH_MAX];
  if (!realpath(rel, resolved)) {
    // realpath fails for files that do not exist yet; normalize manually
    if (rel[0] == '/') snprintf(resolved, sizeof(resolved), "%s", rel);
    else { char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof(cwd))) return NULL; snprintf(resolved, sizeof(resolved), "%s/%s", cwd, rel); }
  }
  snprintf(abs, maxlen, "%s", resolved);
  return abs;
}
inline BOOL GetOverlappedResult(HANDLE_INT, OVERLAPPED* ov, DWORD* bytes, BOOL)
{
  if (bytes) *bytes = ov ? ov->dwNumberOfBytesTransfered : 0;
  return TRUE;
}
#ifndef Int32x32To64
#define Int32x32To64(a,b) ((long long)(int)(a) * (long long)(int)(b))
#endif
inline BOOL SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft)
{
  struct tm t; memset(&t, 0, sizeof(t));
  t.tm_year = st->wYear - 1900; t.tm_mon = st->wMonth - 1; t.tm_mday = st->wDay;
  t.tm_hour = st->wHour; t.tm_min = st->wMinute; t.tm_sec = st->wSecond;
  time_t tt = timegm(&t);
  if (tt == (time_t)-1) return FALSE;
  unsigned long long v = (unsigned long long)tt * 10000000ULL + 116444736000000000ULL + st->wMilliseconds * 10000ULL;
  ft->dwLowDateTime = (DWORD)v; ft->dwHighDateTime = (DWORD)(v >> 32);
  return TRUE;
}

#ifndef ERROR_NO_SYSTEM_RESOURCES
#define ERROR_NO_SYSTEM_RESOURCES 1450
#endif
inline BOOL CancelIo(HANDLE_INT) { return TRUE; }

struct _finddata_t {
  unsigned attrib;
  long time_create;
  long time_access;
  long time_write;
  long size;
  char name[260];
};
inline intptr_t _findfirst(const char* spec, struct _finddata_t* fd)
{
  __finddata64_t d;
  intptr_t h = _findfirst64(spec, &d);
  if (h == -1) return h;
  fd->attrib=d.attrib; fd->time_create=(long)d.time_create; fd->time_access=(long)d.time_access;
  fd->time_write=(long)d.time_write; fd->size=(long)d.size; snprintf(fd->name,sizeof(fd->name),"%s",d.name);
  return h;
}
inline int _findnext(intptr_t h, struct _finddata_t* fd)
{
  __finddata64_t d;
  int r = _findnext64(h, &d);
  if (r == 0) {
    fd->attrib=d.attrib; fd->time_create=(long)d.time_create; fd->time_access=(long)d.time_access;
    fd->time_write=(long)d.time_write; fd->size=(long)d.size; snprintf(fd->name,sizeof(fd->name),"%s",d.name);
  }
  return r;
}

inline void adaptFilenameToLinux(std::string& s)
{
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\') s[i] = '/';
    else s[i] = (char)tolower((unsigned char)s[i]);
  }
}
inline bool getFilenameNoCase(const char* path, std::string& out)
{
  char resolved[PATH_MAX];
  if (port_resolve_path_nocase(path, resolved, sizeof(resolved)) != 0) return false;
  out = resolved;
  return true;
}

inline BOOL ReadFileEx(HANDLE_INT h, void* buf, DWORD n, OVERLAPPED* ov, LPOVERLAPPED_COMPLETION_ROUTINE cb)
{
  if (ov) lseek((int)h, (off_t)(((unsigned long long)ov->OffsetHigh << 32) | ov->Offset), SEEK_SET);
  DWORD got = 0;
  if (!ReadFile(h, buf, n, &got, ov)) return FALSE;
  if (ov) ov->dwNumberOfBytesTransfered = got;
  if (cb) cb(0, got, ov);
  return TRUE;
}

inline void replaceDoublePathFilename(char* szFileName)
{
  if (!szFileName) return;
  char* r = szFileName; char* w = szFileName; char prev = 0;
  while (*r) {
    char c = *r++;
    if (c == '\\') c = '/';
    if (c == '/' && prev == '/') continue;
    *w++ = c; prev = c;
  }
  *w = 0;
}

inline char* _ltoa(long v, char* buf, int radix) { (void)radix; sprintf(buf, "%ld", v); return buf; }
inline char* _itoa(int v, char* buf, int radix) { (void)radix; sprintf(buf, "%d", v); return buf; }
inline char* ltoa(long v, char* buf, int radix) { return _ltoa(v, buf, radix); }
inline char* itoa(int v, char* buf, int radix) { return _itoa(v, buf, radix); }

inline int IsBadReadPtr(const void*, size_t) { return 0; }
inline int IsBadWritePtr(const void*, size_t) { return 0; }
inline char* strlwr(char* s) { for (char* p=s; *p; ++p) *p=(char)tolower((unsigned char)*p); return s; }
inline char* strupr(char* s) { for (char* p=s; *p; ++p) *p=(char)toupper((unsigned char)*p); return s; }
inline void _splitpath(const char* path, char* drive, char* dir, char* fname, char* ext)
{
  if (drive) drive[0]=0;
  const char* slash = strrchr(path, '/');
  const char* dot = strrchr(path, '.');
  // NOTE: the Windows _splitpath contract has no buffer-size argument and the
  // engine has call sites with small extension buffers (e.g. char extn[16] in
  // RenderDll/Common/Shaders/ShaderScript.cpp).  Do not snprintf with a fixed
  // 260-byte bound here: glibc's _FORTIFY_SOURCE aborts when the destination
  // object is smaller.  Copy just the actual extension string.
  if (ext) {
    if (dot && (!slash || dot > slash)) {
      size_t nExt = strlen(dot);
      memcpy(ext, dot, nExt + 1);
    } else ext[0]=0;
  }
  if (fname) {
    const char* start = slash ? slash + 1 : path;
    size_t n = (dot && dot >= start) ? (size_t)(dot - start) : strlen(start);
    memcpy(fname, start, n); fname[n]=0;
  }
  if (dir) { if (slash) { size_t n=(size_t)(slash-path)+1; memcpy(dir,path,n); dir[n]=0; } else dir[0]=0; }
}
inline void _makepath(char* path, const char* drive, const char* dir, const char* fname, const char* ext)
{
  path[0]=0;
  if (drive) strcat(path, drive);
  if (dir) strcat(path, dir);
  if (fname) strcat(path, fname);
  if (ext) strcat(path, ext);
}

inline void NotifySystemOnQuit() {}
inline int _fstat64(int fd, struct stat* st) { return fstat(fd, st); }

inline void RemoveCRLF(char* s) { if (!s) return; size_t n=strlen(s); while (n && (s[n-1]=='\n' || s[n-1]=='\r')) s[--n]=0; }

inline DWORD GetCurrentTime() { return GetTickCount(); }
inline void GetLocalTime(SYSTEMTIME* st)
{
  time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
  st->wYear = lt.tm_year + 1900; st->wMonth = lt.tm_mon + 1; st->wDayOfWeek = lt.tm_wday;
  st->wDay = lt.tm_mday; st->wHour = lt.tm_hour; st->wMinute = lt.tm_min; st->wSecond = lt.tm_sec;
  st->wMilliseconds = 0;
}
inline BOOL MakeSureDirectoryPathExists(const char* path)
{
  if (!path || strlen(path) >= PATH_MAX) { errno = EINVAL; return FALSE; }
  char tmp[PATH_MAX]; strcpy(tmp, path);
  for (char* p = tmp; *p; ++p) if (*p == '\\') *p = '/';
  // Windows API accepts a filename: create only separator-terminated parents.
  for (char* p = tmp + 1; *p; ++p) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return FALSE;
      struct stat st;
      if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) return FALSE;
      *p = '/';
    }
  }
  return TRUE;
}

#pragma pack(push,1)
typedef struct tagBITMAPFILEHEADER { WORD bfType; DWORD bfSize; WORD bfReserved1; WORD bfReserved2; DWORD bfOffBits; } BITMAPFILEHEADER;
typedef struct tagBITMAPINFOHEADER { DWORD biSize; LONG biWidth; LONG biHeight; WORD biPlanes; WORD biBitCount; DWORD biCompression; DWORD biSizeImage; LONG biXPelsPerMeter; LONG biYPelsPerMeter; DWORD biClrUsed; DWORD biClrImportant; } BITMAPINFOHEADER;
#pragma pack(pop)

#ifndef WINGDIAPI
#define WINGDIAPI
#endif
#ifndef APIENTRY
#define APIENTRY
#endif
typedef void* HDC;
typedef void* HGLRC;

#ifndef CONST
#define CONST const
#endif
#ifndef VOID
#define VOID void
#endif
typedef void (*PROC)();
typedef void* HPBUFFERARB;
typedef void* HPBUFFEREXT;
#define DECLARE_HANDLE(name) typedef void* name
typedef struct tagPIXELFORMATDESCRIPTOR {
  WORD nSize, nVersion; DWORD dwFlags; BYTE iPixelType, cColorBits, cRedBits, cRedShift,
  cGreenBits, cGreenShift, cBlueBits, cBlueShift, cAlphaBits, cAlphaShift, cAccumBits,
  cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits, cDepthBits, cStencilBits,
  cAuxBuffers, iLayerType, bReserved; DWORD dwLayerMask, dwVisibleMask, dwDamageMask;
} PIXELFORMATDESCRIPTOR, *LPPIXELFORMATDESCRIPTOR;

typedef void* HINSTANCE;
#define _stdcall
#define __stdcall
#define ERROR_INVALID_PIXEL_FORMAT 2000
typedef struct _DEVMODEA {
  char dmDeviceName[32]; WORD dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; DWORD dmFields;
  short dmOrientation, dmPaperSize, dmPaperLength, dmPaperWidth, dmScale, dmCopies, dmDefaultSource, dmPrintQuality;
  short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate; char dmFormName[32];
  WORD dmLogPixels; DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
  DWORD dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
} DEVMODE, *LPDEVMODE;
inline BOOL SetFileTime(HANDLE_INT h, const FILETIME*, const FILETIME*, const FILETIME* t)
{
  if (!t) return FALSE;
  unsigned long long v = ((unsigned long long)t->dwHighDateTime << 32) | t->dwLowDateTime;
  struct timespec ts[2]; ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT;
  ts[1].tv_sec = (time_t)((v - 116444736000000000ULL) / 10000000ULL); ts[1].tv_nsec = 0;
  return futimens((int)h, ts) == 0;
}

#define ERROR_INVALID_DATA 13
inline BOOL SetDeviceGammaRamp(HDC, void*) { return TRUE; }
inline void* GetDesktopWindow() { return NULL; }
inline char* _strtime(char* buf) { time_t t=time(NULL); struct tm lt; localtime_r(&t,&lt); sprintf(buf,"%02d:%02d:%02d",lt.tm_hour,lt.tm_min,lt.tm_sec); return buf; }
inline char* _strdate(char* buf) { time_t t=time(NULL); struct tm lt; localtime_r(&t,&lt); sprintf(buf,"%02d/%02d/%02d",lt.tm_mon+1,lt.tm_mday,(lt.tm_year+1900)%100); return buf; }

#ifndef _declspec
#define _declspec(x)
#endif
inline HDC GetDC(void*) { return NULL; }
inline int ReleaseDC(void*, HDC) { return 0; }
inline BOOL SwapBuffers(HDC) { return TRUE; }

#ifndef TEXT
#define TEXT(x) x
#endif

#define BITSPIXEL 12
inline BOOL GetWindowRect(void*, RECT* r) { if (r) { r->left=r->top=0; r->right=640; r->bottom=480; } return TRUE; }
inline int GetDeviceCaps(HDC, int) { return 32; }

#ifndef ZeroMemory
#define ZeroMemory(p,n) memset((p),0,(n))
#endif
inline BOOL EnumDisplaySettings(const char*, DWORD, DEVMODE*) { return FALSE; }

inline int compareTextFileStrings(const char* a, const char* b) { return strcasecmp(a,b); }

#include <string>
inline void RemoveCRLF(std::string& s) { while (!s.empty() && (s.back()=='\n' || s.back()=='\r')) s.pop_back(); }

#ifndef VK_ESCAPE
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SELECT 0x29
#define VK_PRINT 0x2A
#define VK_EXECUTE 0x2B
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HELP 0x2F
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F13 0x7C
#define VK_F14 0x7D
#define VK_F15 0x7E
#define VK_F16 0x7F
#define VK_F17 0x80
#define VK_F18 0x81
#define VK_F19 0x82
#define VK_F20 0x83
#define VK_F21 0x84
#define VK_F22 0x85
#define VK_F23 0x86
#define VK_F24 0x87
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_OEM_102 0xE2
#define VK_KANA 0x15
#define VK_CONVERT 0x1C
#define VK_NONCONVERT 0x1D
#define VK_JUNJA 0x17
#define VK_FINAL 0x18
#define VK_KANJI 0x19
#endif

#ifndef WSAGetLastError
#define WSAGetLastError() (int)errno
#endif

#ifndef closesocket
#define closesocket close
#endif
