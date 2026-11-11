/* BinkPort.cpp - RAD Bink 1.x API (the part CUIVideoPanel uses) on FFmpeg.
 * See BinkPort.h. Video: libavformat demuxes the .bik, libavcodec decodes
 * Bink video into YUV 4:2:0, converted here to the 32-bit BGRA surface the
 * panel uploads. Audio: the selected track is decoded, converted to stereo
 * S16 with libswresample and mixed into the game's audio output. A worker
 * thread decodes and converts up to three frames ahead, so the main thread
 * only copies finished frames. Timing follows the movie's frame rate on a
 * wall clock (BinkWait), like RAD's player without frame skipping. */
#include "BinkPort.h"
#include <SDL.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}

// Game mixer stream (SDL2Sound.cpp); falls back to an own SDL device.
int PortAudio_ExternalRate();
int PortAudio_ExternalWrite(const Sint16* lr, int frames);
int PortAudio_ExternalQueued();
void PortAudio_ExternalClear();
void PortAudio_ExternalPause(bool p);
long long PortAudio_ExternalMixed();
// The game's menus open the same movie more than once (Ubi.bik, Crytek.bik are
// opened twice each). Only one player may own the shared mixer stream: the
// most recently opened one. Without this, two players fed the stream at once
// and closing either cleared/paused it under the other (movie sound playing,
// cutting out or missing). Game sounds are unaffected (separate mixer path).
static void* g_audioOwner = nullptr;
static std::vector<void*> g_mixerPlayers;   // open players using the mixer, oldest first

namespace
{
  std::string g_error = "";
  U32 g_track = 0;
  U32 g_forceRate = 0, g_forceRateDiv = 1;

  struct SBinkState
  {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    SwrContext* swr = nullptr;
    int vstream = -1, astream = -1;
    AVFrame* frame = nullptr;   // last decoded video frame
    AVFrame* aframe = nullptr;
    AVPacket* pkt = nullptr;
    SDL_AudioDeviceID audio = 0;
    bool mixer = false; // audio goes to the game's mixer
    int audioRate = 0;
    bool soundOn = true, videoOn = true, eof = false;
    float volume = 1.0f;
    long long aDecoded = 0, aWritten = 0, aDropped = 0;   // audio frames (diagnostic log)
    Uint64 startTicks = 0, pausedAt = 0;
    bool paused = false;
    // Decode-ahead worker: converted BGRA frames (Width*Height*4).
    int w = 0, h = 0;
    std::thread worker;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> ready, pool;
    std::vector<uint8_t> current;
    bool stop = false, done = false;
  };

  void ConvertFrame(const AVFrame* f, uint8_t* dst, int w, int h)
  {
    const bool alpha = f->format == AV_PIX_FMT_YUVA420P;
    for (int j = 0; j < h; ++j)
    {
      const uint8_t* Y = f->data[0] + j * f->linesize[0];
      const uint8_t* U = f->data[1] + (j >> 1) * f->linesize[1];
      const uint8_t* V = f->data[2] + (j >> 1) * f->linesize[2];
      const uint8_t* A = alpha ? f->data[3] + j * f->linesize[3] : nullptr;
      uint8_t* d = dst + (size_t)j * w * 4;
      for (int i = 0; i < w; ++i, d += 4)
      {
        // BT.601 limited range -> RGB (fixed point), stored B,G,R,A.
        const int c = (Y[i] - 16) * 298, du = U[i >> 1] - 128, dv = V[i >> 1] - 128;
        int r = (c + 409 * dv + 128) >> 8, g = (c - 100 * du - 208 * dv + 128) >> 8, bl = (c + 516 * du + 128) >> 8;
        d[0] = (uint8_t)(bl < 0 ? 0 : bl > 255 ? 255 : bl);
        d[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
        d[2] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
        d[3] = A ? A[i] : 255;
      }
    }
  }

  // Case-insensitive resolution of a game path ("Languages\\Movies\\x.bik")
  // relative to the working directory (the port's runtime data folder).
  std::string ResolvePath(const char* name)
  {
    std::string p(name);
    for (char& c : p) if (c == '\\') c = '/';
    FILE* f = fopen(p.c_str(), "rb");
    if (f) { fclose(f); return p; }
    std::string cur = p[0] == '/' ? "/" : ".";
    size_t pos = p[0] == '/' ? 1 : 0;
    while (pos <= p.size())
    {
      size_t slash = p.find('/', pos);
      std::string part = p.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
      pos = slash == std::string::npos ? p.size() + 1 : slash + 1;
      if (part.empty() || part == ".") continue;
      DIR* d = opendir(cur.c_str());
      if (!d) return p;
      std::string found;
      while (dirent* e = readdir(d)) if (!strcasecmp(e->d_name, part.c_str())) { found = e->d_name; break; }
      closedir(d);
      if (found.empty()) return p;
      cur = cur == "/" ? "/" + found : cur + "/" + found;
    }
    return cur;
  }

  AVCodecContext* OpenDecoder(AVStream* st)
  {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return nullptr;
    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(c, st->codecpar) < 0 || avcodec_open2(c, codec, nullptr) < 0) { avcodec_free_context(&c); return nullptr; }
    return c;
  }

  void QueueAudio(SBinkState* s, AVFrame* f)
  {
    if ((!s->audio && !s->mixer) || !s->swr) return;
    const int outSamples = swr_get_out_samples(s->swr, f->nb_samples);
    if (outSamples <= 0) return;
    static thread_local std::string buf;
    buf.resize((size_t)outSamples * 4);
    uint8_t* out[1] = { (uint8_t*)&buf[0] };
    const int got = swr_convert(s->swr, out, outSamples, (const uint8_t**)f->extended_data, f->nb_samples);
    if (got > 0) s->aDecoded += got;
    if (got <= 0 || !s->soundOn) return;
    if (s->volume < 0.999f)
    {
      int16_t* p = (int16_t*)&buf[0];
      for (int i = 0; i < got * 2; ++i) p[i] = (int16_t)(p[i] * s->volume);
    }
    if (s->mixer) { if (g_audioOwner == s) { int w = PortAudio_ExternalWrite((const Sint16*)buf.data(), got); s->aWritten += w; s->aDropped += got - w; } else s->aDropped += got; }
    else SDL_QueueAudio(s->audio, buf.data(), (Uint32)got * 4);
  }

  // Read packets until one video frame is decoded (audio packets met on the
  // way are decoded and queued). Returns false at the end of the movie.
  bool DecodeVideoFrame(SBinkState* s)
  {
    for (;;)
    {
      int r = avcodec_receive_frame(s->vdec, s->frame);
      if (r == 0) return true;
      if (r != AVERROR(EAGAIN) || s->eof) return false;
      if (av_read_frame(s->fmt, s->pkt) < 0) { s->eof = true; avcodec_send_packet(s->vdec, nullptr); continue; }
      if (s->pkt->stream_index == s->vstream) avcodec_send_packet(s->vdec, s->pkt);
      else if (s->pkt->stream_index == s->astream && s->adec)
      {
        if (avcodec_send_packet(s->adec, s->pkt) == 0)
          while (avcodec_receive_frame(s->adec, s->aframe) == 0) QueueAudio(s, s->aframe);
      }
      av_packet_unref(s->pkt);
    }
  }

  void Worker(SBinkState* s)
  {
    for (;;)
    {
      std::vector<uint8_t> buf;
      {
        std::unique_lock<std::mutex> lk(s->m);
        s->cv.wait(lk, [s] { return s->stop || s->ready.size() < 3; });
        if (s->stop) return;
        if (!s->pool.empty()) { buf = std::move(s->pool.front()); s->pool.pop_front(); }
      }
      if (!DecodeVideoFrame(s))
      {
        std::lock_guard<std::mutex> lk(s->m);
        s->done = true; s->cv.notify_all(); return;
      }
      const int fw = s->frame->width < s->w ? s->frame->width : s->w, fh = s->frame->height < s->h ? s->frame->height : s->h;
      buf.resize((size_t)s->w * s->h * 4);
      ConvertFrame(s->frame, buf.data(), fw, fh);
      std::lock_guard<std::mutex> lk(s->m);
      s->ready.push_back(std::move(buf)); s->cv.notify_all();
    }
  }

  SBinkState* St(HBINK b) { return b ? (SBinkState*)b->priv : nullptr; }
}

HBINK BinkOpen(const char* name, U32 flags)
{
  if (!name || (flags & BINKFROMMEMORY)) { g_error = "BinkOpen: memory movies are not supported"; return nullptr; }
  const std::string path = ResolvePath(name);
  SBinkState* s = new SBinkState();
  if (avformat_open_input(&s->fmt, path.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(s->fmt, nullptr) < 0)
  {
    g_error = "BinkOpen: cannot open " + path;
    if (s->fmt) avformat_close_input(&s->fmt);
    delete s; return nullptr;
  }
  int audioIndex = 0;
  for (unsigned i = 0; i < s->fmt->nb_streams; ++i)
  {
    const AVCodecParameters* p = s->fmt->streams[i]->codecpar;
    if (p->codec_type == AVMEDIA_TYPE_VIDEO && s->vstream < 0) s->vstream = (int)i;
    else if (p->codec_type == AVMEDIA_TYPE_AUDIO)
    {
      if (s->astream < 0 || audioIndex == (int)g_track) s->astream = (int)i;
      ++audioIndex;
    }
  }
  if (s->vstream < 0 || !(s->vdec = OpenDecoder(s->fmt->streams[s->vstream])))
  {
    g_error = "BinkOpen: no Bink video in " + path;
    avformat_close_input(&s->fmt); delete s; return nullptr;
  }
  s->frame = av_frame_alloc(); s->aframe = av_frame_alloc(); s->pkt = av_packet_alloc();
  if (s->astream >= 0 && (s->adec = OpenDecoder(s->fmt->streams[s->astream])))
  {
    SDL_AudioSpec want; SDL_zero(want);
    want.freq = s->adec->sample_rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    if (PortAudio_ExternalRate() > 0) { s->mixer = true; want.freq = PortAudio_ExternalRate(); g_audioOwner = s; g_mixerPlayers.push_back(s); PortAudio_ExternalClear(); PortAudio_ExternalPause(false); }
    else if (SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
      s->audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (s->audio || s->mixer)
    {
      AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
      if (swr_alloc_set_opts2(&s->swr, &out, AV_SAMPLE_FMT_S16, want.freq, &s->adec->ch_layout, s->adec->sample_fmt, s->adec->sample_rate, 0, nullptr) < 0 || swr_init(s->swr) < 0)
        swr_free(&s->swr);
      if (s->audio) SDL_PauseAudioDevice(s->audio, 0);
    }
    else
      fprintf(stderr, "PORT bink: no audio device for %s (%s)\n", path.c_str(), SDL_GetError());
  }
  AVStream* vs = s->fmt->streams[s->vstream];
  BINK* b = new BINK();
  b->Width = (U32)s->vdec->width; b->Height = (U32)s->vdec->height;
  b->Frames = vs->nb_frames > 0 ? (U32)vs->nb_frames : (U32)(vs->duration > 0 ? vs->duration : 0);
  b->FrameNum = 1; b->LastFrameNum = 0;
  AVRational fr = vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate;
  b->FrameRate = g_forceRate ? g_forceRate : (U32)(fr.num ? fr.num : 30);
  b->FrameRateDiv = g_forceRate ? g_forceRateDiv : (U32)(fr.den ? fr.den : 1);
  b->priv = s;
  s->w = (int)b->Width; s->h = (int)b->Height;
  s->worker = std::thread(Worker, s);
  s->startTicks = SDL_GetPerformanceCounter();
  fprintf(stderr, "PORT bink: open %p %s %ux%u %u frames %.2f fps audio=%d\n", (void*)s, path.c_str(), b->Width, b->Height, b->Frames,
          (double)b->FrameRate / b->FrameRateDiv, s->mixer ? 2 : (s->audio != 0));
  return b;
}

void BinkClose(HBINK b)
{
  SBinkState* s = St(b);
  if (!s) return;
  fprintf(stderr, "PORT bink: close %p (frame %u, sound owner %s) audio: dec=%d swr=%d decoded=%lld written=%lld dropped=%lld soundOn=%d vol=%.2f queued=%d mixed=%lld\n",
          (void*)s, b->FrameNum, g_audioOwner == s ? "yes" : "no", s->adec != nullptr, s->swr != nullptr, s->aDecoded, s->aWritten, s->aDropped,
          (int)s->soundOn, s->volume, PortAudio_ExternalQueued(), PortAudio_ExternalMixed());
  { std::lock_guard<std::mutex> lk(s->m); s->stop = true; }
  s->cv.notify_all();
  if (s->worker.joinable()) s->worker.join();
  if (s->audio) SDL_CloseAudioDevice(s->audio);
  if (s->mixer)
  {
    for (size_t i = 0; i < g_mixerPlayers.size(); ++i) if (g_mixerPlayers[i] == s) { g_mixerPlayers.erase(g_mixerPlayers.begin() + i); break; }
    if (g_audioOwner == s)
    {   // the newest player still open takes over the stream
      PortAudio_ExternalClear(); PortAudio_ExternalPause(false);
      g_audioOwner = g_mixerPlayers.empty() ? nullptr : g_mixerPlayers.back();
    }
  }
  swr_free(&s->swr);
  avcodec_free_context(&s->vdec); avcodec_free_context(&s->adec);
  av_frame_free(&s->frame); av_frame_free(&s->aframe); av_packet_free(&s->pkt);
  avformat_close_input(&s->fmt);
  delete s; delete b;
}

S32 BinkWait(HBINK b)
{
  SBinkState* s = St(b);
  if (!s || s->paused) return 1;
  const double t = (double)(SDL_GetPerformanceCounter() - s->startTicks) / SDL_GetPerformanceFrequency();
  const double due = (double)(b->FrameNum - 1) * b->FrameRateDiv / b->FrameRate;
  return t < due ? 1 : 0;
}

S32 BinkDoFrame(HBINK b)
{
  SBinkState* s = St(b);
  if (!s) return 0;
  std::unique_lock<std::mutex> lk(s->m);
  s->cv.wait(lk, [s] { return !s->ready.empty() || s->done; });
  if (s->ready.empty()) { b->FrameNum = b->Frames; return 0; }
  if (!s->current.empty()) s->pool.push_back(std::move(s->current));
  s->current = std::move(s->ready.front()); s->ready.pop_front();
  s->cv.notify_all();
  b->LastFrameNum = b->FrameNum;
  return 0;
}

void BinkNextFrame(HBINK b)
{
  if (b && b->FrameNum < b->Frames) ++b->FrameNum;
}

S32 BinkCopyToBuffer(HBINK b, void* dest, S32 pitch, U32 height, U32 x, U32 y, U32 flags)
{
  SBinkState* s = St(b);
  if (!s || !dest || s->current.empty() || !s->videoOn) return 0;
  const int w = s->w, h = s->h < (int)height ? s->h : (int)height;
  for (int j = 0; j < h; ++j)
    memcpy((uint8_t*)dest + (size_t)(j + y) * pitch + (size_t)x * 4, s->current.data() + (size_t)j * w * 4, (size_t)w * 4);
  // Diagnostic: FARCRY_BINK_DUMP=<dir> writes every 30th converted frame.
  if (const char* dir = getenv("FARCRY_BINK_DUMP"))
    if ((b->FrameNum % 30) == 1)
    {
      char path[512]; snprintf(path, sizeof(path), "%s/bink-%04u.ppm", dir, b->FrameNum);
      if (FILE* fp = fopen(path, "wb"))
      {
        fprintf(fp, "P6\n%d %d\n255\n", w, h);
        for (int j = 0; j < h; ++j)
          for (int i = 0; i < w; ++i) { const uint8_t* px = (const uint8_t*)dest + (size_t)(j + y) * pitch + (size_t)(x + i) * 4; fputc(px[2], fp); fputc(px[1], fp); fputc(px[0], fp); }
        fclose(fp);
      }
    }
  return 0;
}

S32 BinkPause(HBINK b, S32 pause)
{
  SBinkState* s = St(b);
  if (!s) return 0;
  if (pause && !s->paused) { s->paused = true; s->pausedAt = SDL_GetPerformanceCounter(); if (s->audio) SDL_PauseAudioDevice(s->audio, 1); if (s->mixer && g_audioOwner == s) PortAudio_ExternalPause(true); }
  else if (!pause && s->paused) { s->paused = false; s->startTicks += SDL_GetPerformanceCounter() - s->pausedAt; if (s->audio) SDL_PauseAudioDevice(s->audio, 0); if (s->mixer && g_audioOwner == s) PortAudio_ExternalPause(false); }
  return s->paused;
}

char* BinkGetError(void) { return (char*)g_error.c_str(); }
void BinkSetSoundTrack(U32 total, unsigned long* tracks) { g_track = (total && tracks) ? tracks[0] : 0; }
S32 BinkSoundUseDirectSound(void*) { return 1; }
S32 BinkSoundUseWaveOut(void) { return 1; }
void* BinkLogoAddress(void) { return nullptr; }
void BinkSetVolume(HBINK b, U32, S32 volume) { if (SBinkState* s = St(b)) s->volume = volume <= 0 ? 0.0f : (volume >= 32768 ? 1.0f : volume / 32768.0f); }
void BinkSetPan(HBINK, U32, S32) {}
void BinkSetFrameRate(U32 rate, U32 div) { g_forceRate = rate; g_forceRateDiv = div ? div : 1; }
S32 BinkSetVideoOnOff(HBINK b, S32 on) { if (SBinkState* s = St(b)) s->videoOn = on != 0; return on; }
S32 BinkSetSoundOnOff(HBINK b, S32 on)
{
  if (SBinkState* s = St(b)) { s->soundOn = on != 0; if (!on && s->audio) SDL_ClearQueuedAudio(s->audio); if (!on && s->mixer && g_audioOwner == s) PortAudio_ExternalClear(); }
  return on;
}
