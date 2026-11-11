/* BinkPort.h - the subset of RAD's Bink 1.x API that CUIVideoPanel uses,
 * implemented in BinkPort.cpp on FFmpeg's open Bink decoder (libavformat /
 * libavcodec / libswresample, LGPL, linked dynamically) and SDL audio.
 * RAD's SDK ships only a Windows binary, so the engine's video panel compiled
 * without it on Linux and the campaign cutscenes were skipped.
 * The leading BINK fields match RAD's layout; the panel reads them directly. */
#pragma once
#include <stdint.h>

typedef uint32_t U32;
typedef int32_t S32;

typedef struct BINK
{
  U32 Width;        // frame width
  U32 Height;       // frame height
  U32 Frames;       // number of frames (1 based)
  U32 FrameNum;     // frame to be displayed next (1 based)
  U32 LastFrameNum; // last frame decompressed (1 based)
  U32 FrameRate;    // frame rate numerator
  U32 FrameRateDiv; // frame rate denominator
  void* priv;       // port: decoder state
} BINK, *HBINK;

#define BINKSNDTRACK   0x00004000L
#define BINKNOSKIP     0x00080000L
#define BINKFROMMEMORY 0x04000000L
#define BINKCOPYALL    0x80000000L
#define BINKSURFACE32  3

HBINK BinkOpen(const char* name, U32 flags);
void BinkClose(HBINK bink);
S32 BinkWait(HBINK bink);
S32 BinkDoFrame(HBINK bink);
void BinkNextFrame(HBINK bink);
S32 BinkCopyToBuffer(HBINK bink, void* dest, S32 destpitch, U32 destheight, U32 destx, U32 desty, U32 flags);
S32 BinkPause(HBINK bink, S32 pause);
char* BinkGetError(void);
void BinkSetSoundTrack(U32 total_tracks, unsigned long* tracks); // DWORD in the panel
S32 BinkSoundUseDirectSound(void* dd);
S32 BinkSoundUseWaveOut(void);
void* BinkLogoAddress(void);
void BinkSetVolume(HBINK bink, U32 trackid, S32 volume);
void BinkSetPan(HBINK bink, U32 trackid, S32 pan);
void BinkSetFrameRate(U32 forcerate, U32 forceratediv);
S32 BinkSetVideoOnOff(HBINK bink, S32 onoff);
S32 BinkSetSoundOnOff(HBINK bink, S32 onoff);
