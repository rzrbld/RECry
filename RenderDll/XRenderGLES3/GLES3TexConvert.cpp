/*=============================================================================
  GLES3TexConvert.cpp : background DXT -> ETC2 texture conversion.
  See GLES3TexConvert.h.
=============================================================================*/
#include "GLES3TexConvert.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <string>

#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include "ProcessRGB.hpp"

enum
{
  DXT1_RGB = 0x83F0, DXT1_RGBA = 0x83F1, DXT3 = 0x83F2, DXT5 = 0x83F3,
  ETC2_RGB8 = 0x9274, ETC2_RGBA8_EAC = 0x9278,
};

struct SJob
{
  uint32_t tex, gen, target, dxt;
  int level, w, h;
  std::vector<uint8_t> data;
};

static std::vector<std::thread> s_workers;
static std::mutex s_mutex;
static std::condition_variable s_cv;
static std::deque<SJob> s_jobs;
static std::deque<STexConvResult> s_done;
static std::atomic<bool> s_quit(false);
static std::atomic<size_t> s_inFlight(0);
static std::string s_cacheDir;
static void (*s_log)(const char*) = NULL;
static std::atomic<unsigned> s_converted(0), s_cacheHits(0);

static void Log(const char* fmt, ...)
{
  if (!s_log) return;
  char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  s_log(b);
}

bool TexConv_IsDXT(uint32_t f) { return f >= DXT1_RGB && f <= DXT5; }
uint32_t TexConv_Etc2Format(uint32_t dxt) { return dxt == DXT1_RGB ? ETC2_RGB8 : ETC2_RGBA8_EAC; }
size_t TexConv_Etc2Size(uint32_t etc2, int w, int h)
{
  const size_t blocks = (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4);
  return blocks * (etc2 == ETC2_RGB8 ? 8 : 16);
}

static uint64_t Hash64(const uint8_t* p, size_t n, uint64_t h)
{
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
  return h;
}

// Decode DXT into a BGRA image padded to whole blocks (etcpak's input order).
static void DecodeToBGRA(uint32_t fmt, int w, int h, const uint8_t* src, std::vector<uint32_t>& img, int& pw, int& ph)
{
  pw = (w + 3) & ~3; ph = (h + 3) & ~3;
  img.assign((size_t)pw * ph, 0);
  const int bs = (fmt == DXT1_RGB || fmt == DXT1_RGBA) ? 8 : 16;
  for (int by = 0; by < ph / 4; ++by)
    for (int bx = 0; bx < pw / 4; ++bx, src += bs)
    {
      uint8_t* dst = (uint8_t*)&img[(size_t)by * 4 * pw + bx * 4];
      const int pitch = pw * 4;
      if (fmt == DXT3) bcdec_bc2(src, dst, pitch);
      else if (fmt == DXT5) bcdec_bc3(src, dst, pitch);
      else bcdec_bc1(src, dst, pitch);
    }
  for (size_t i = 0; i < img.size(); ++i)
  {
    uint32_t c = img[i];  // bytes R,G,B,A -> B,G,R,A
    img[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
    if (fmt == DXT1_RGB) img[i] |= 0xFF000000u;
  }
}

static void Encode(uint32_t etc2, const std::vector<uint32_t>& img, int pw, int ph, std::vector<uint8_t>& out)
{
  const uint32_t blocks = (uint32_t)((pw / 4) * (ph / 4));
  out.resize(blocks * (etc2 == ETC2_RGB8 ? 8 : 16));
  if (etc2 == ETC2_RGB8) CompressEtc2Rgb(img.data(), (uint64_t*)out.data(), blocks, (size_t)pw, true);
  else CompressEtc2Rgba(img.data(), (uint64_t*)out.data(), blocks, (size_t)pw, true);
}

static uint64_t CacheKey(uint32_t dxt, int w, int h, const void* data, size_t size)
{
  uint64_t key = Hash64((const uint8_t*)data, size, 1469598103934665603ULL);
  const uint32_t meta[3] = { dxt, (uint32_t)w, (uint32_t)h };
  return Hash64((const uint8_t*)meta, sizeof(meta), key);
}

static std::string CachePath(uint64_t key)
{
  if (s_cacheDir.empty()) return std::string();
  char b[64]; snprintf(b, sizeof(b), "/%02x/%016llx.etc2", (unsigned)(key & 0xFF), (unsigned long long)key);
  return s_cacheDir + b;
}

static bool ReadCache(const std::string& path, size_t size, std::vector<uint8_t>& out)
{
  if (path.empty()) return false;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out.resize(size);
  const bool ok = fread(out.data(), 1, size, f) == size && fgetc(f) == EOF;
  fclose(f);
  return ok;
}

static void WriteCache(const std::string& path, const std::vector<uint8_t>& data)
{
  if (path.empty()) return;
  std::string dir = path.substr(0, path.find_last_of('/'));
  mkdir(s_cacheDir.c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return;
  const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  fclose(f);
  if (ok) rename(tmp.c_str(), path.c_str()); else remove(tmp.c_str());
}

static void Worker()
{
  std::vector<uint32_t> img;
  for (;;)
  {
    SJob j;
    {
      std::unique_lock<std::mutex> lk(s_mutex);
      s_cv.wait(lk, [] { return s_quit || !s_jobs.empty(); });
      if (s_quit) return;
      j = std::move(s_jobs.front());
      s_jobs.pop_front();
    }
    STexConvResult r;
    r.tex = j.tex; r.gen = j.gen; r.target = j.target; r.level = j.level; r.w = j.w; r.h = j.h;
    r.glFormat = TexConv_Etc2Format(j.dxt);
    const size_t etcSize = TexConv_Etc2Size(r.glFormat, j.w, j.h);
    const std::string path = CachePath(CacheKey(j.dxt, j.w, j.h, j.data.data(), j.data.size()));
    if (ReadCache(path, etcSize, r.data))
      ++s_cacheHits;
    else
    {
      int pw, ph;
      DecodeToBGRA(j.dxt, j.w, j.h, j.data.data(), img, pw, ph);
      Encode(r.glFormat, img, pw, ph, r.data);
      WriteCache(path, r.data);
      ++s_converted;
    }
    {
      std::lock_guard<std::mutex> lk(s_mutex);
      s_done.push_back(std::move(r));
    }
  }
}

bool TexConv_Start(int threads, const char* cacheDir, void (*log)(const char*))
{
  if (!s_workers.empty()) return true;
  s_log = log;
  s_cacheDir = cacheDir ? cacheDir : "";
  s_quit = false;
  if (threads < 1) threads = 1;
  for (int i = 0; i < threads; ++i) s_workers.emplace_back(Worker);
  Log("GLES3TexConvert: %d worker thread(s), cache %s", threads, s_cacheDir.empty() ? "disabled" : s_cacheDir.c_str());
  return true;
}

void TexConv_Stop()
{
  if (s_workers.empty()) return;
  { std::lock_guard<std::mutex> lk(s_mutex); s_quit = true; }
  s_cv.notify_all();
  for (auto& t : s_workers) t.join();
  s_workers.clear();
  s_jobs.clear(); s_done.clear(); s_inFlight = 0;
  Log("GLES3TexConvert: stopped (%u converted, %u from cache)", s_converted.load(), s_cacheHits.load());
}

bool TexConv_Running() { return !s_workers.empty(); }

void TexConv_Placeholder(uint32_t dxt, int w, int h, const void* data, std::vector<uint8_t>& out)
{
  const uint32_t etc2 = TexConv_Etc2Format(dxt);
  out.resize(TexConv_Etc2Size(etc2, w, h));
  // Average colour from up to 16 evenly spaced blocks.
  const int bs = (dxt == DXT1_RGB || dxt == DXT1_RGBA) ? 8 : 16;
  const size_t nBlocks = (size_t)((w + 3) / 4) * ((h + 3) / 4);
  unsigned sum[4] = {0, 0, 0, 0}, n = 0;
  if (data)
    for (size_t i = 0; i < 16 && i < nBlocks; ++i)
    {
      uint8_t px[16 * 4];
      const uint8_t* b = (const uint8_t*)data + (i * nBlocks / (nBlocks < 16 ? nBlocks : 16)) * bs;
      if (dxt == DXT3) bcdec_bc2(b, px, 16); else if (dxt == DXT5) bcdec_bc3(b, px, 16); else bcdec_bc1(b, px, 16);
      for (int p = 0; p < 16; ++p) { for (int c = 0; c < 4; ++c) sum[c] += px[p*4 + c]; ++n; }
    }
  uint8_t avg[4] = { 128, 128, 128, 255 };
  if (n) for (int c = 0; c < 4; ++c) avg[c] = (uint8_t)(sum[c] / n);
  if (dxt == DXT1_RGB) avg[3] = 255;
  uint32_t px[16];
  for (int i = 0; i < 16; ++i) px[i] = (uint32_t)avg[2] | ((uint32_t)avg[1] << 8) | ((uint32_t)avg[0] << 16) | ((uint32_t)avg[3] << 24);
  uint8_t block[16];
  if (etc2 == ETC2_RGB8) CompressEtc2Rgb(px, (uint64_t*)block, 1, 4, false);
  else CompressEtc2Rgba(px, (uint64_t*)block, 1, 4, false);
  const size_t bsz = etc2 == ETC2_RGB8 ? 8 : 16;
  for (size_t o = 0; o < out.size(); o += bsz) memcpy(&out[o], block, bsz);
}

bool TexConv_TryCached(uint32_t dxt, int w, int h, const void* data, size_t size, std::vector<uint8_t>& out)
{
  if (s_cacheDir.empty()) return false;
  const size_t etcSize = TexConv_Etc2Size(TexConv_Etc2Format(dxt), w, h);
  if (!ReadCache(CachePath(CacheKey(dxt, w, h, data, size)), etcSize, out)) return false;
  ++s_cacheHits;
  return true;
}

void TexConv_Enqueue(uint32_t tex, uint32_t gen, uint32_t target, int level, uint32_t dxt, int w, int h, const void* data, size_t size)
{
  SJob j;
  j.tex = tex; j.gen = gen; j.target = target; j.dxt = dxt; j.level = level; j.w = w; j.h = h;
  j.data.assign((const uint8_t*)data, (const uint8_t*)data + size);
  {
    std::lock_guard<std::mutex> lk(s_mutex);
    s_jobs.push_back(std::move(j));
    ++s_inFlight;
  }
  s_cv.notify_one();
}

bool TexConv_Pop(STexConvResult& out)
{
  std::lock_guard<std::mutex> lk(s_mutex);
  if (s_done.empty()) return false;
  out = std::move(s_done.front());
  s_done.pop_front();
  --s_inFlight;
  return true;
}

size_t TexConv_Pending() { return s_inFlight; }
