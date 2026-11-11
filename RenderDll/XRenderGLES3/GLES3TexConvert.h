/*=============================================================================
  GLES3TexConvert.h : background DXT -> ETC2 texture conversion.

  GLES drivers without S3TC (e.g. Mali) cannot sample the game's DXT
  textures. Each DXT upload is replaced immediately by a placeholder of the
  same size in the matching ETC2 format (flat average colour), and the real
  conversion runs on worker threads: DXT is decoded (bcdec) and re-encoded
  to ETC2 (etcpak), keeping GPU memory at DXT size. Results are cached on
  disk, so later runs only read them. The render thread uploads finished
  levels with a per-frame budget.
=============================================================================*/
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>

struct STexConvResult
{
  uint32_t tex, gen;      // GL texture name and its generation at enqueue time
  uint32_t target;        // GL_TEXTURE_2D
  int level, w, h;
  uint32_t glFormat;      // ETC2 format of data
  std::vector<uint8_t> data;
};

// Start the workers (threads >= 1). cacheDir may be NULL (no disk cache).
bool TexConv_Start(int threads, const char* cacheDir, void (*log)(const char*));
void TexConv_Stop();
bool TexConv_Running();

bool TexConv_IsDXT(uint32_t glFormat);
uint32_t TexConv_Etc2Format(uint32_t dxtFormat);
size_t TexConv_Etc2Size(uint32_t etc2Format, int w, int h);

// Flat ETC2 placeholder of the right size, coloured with the texture's
// average colour (sampled from the DXT blocks).
void TexConv_Placeholder(uint32_t dxtFormat, int w, int h, const void* dxt, std::vector<uint8_t>& out);

// Cache lookup on the calling thread: fills out with ETC2 data when this exact
// DXT level was converted before. Lets warm caches skip the placeholder.
bool TexConv_TryCached(uint32_t dxtFormat, int w, int h, const void* dxt, size_t size, std::vector<uint8_t>& out);

// Queue one mip level (the DXT data is copied).
void TexConv_Enqueue(uint32_t tex, uint32_t gen, uint32_t target, int level, uint32_t dxtFormat, int w, int h, const void* dxt, size_t size);

// Non-blocking: take one finished level.
bool TexConv_Pop(STexConvResult& out);
size_t TexConv_Pending();
