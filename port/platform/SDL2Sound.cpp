#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <set>
// SDL2 audio backend for the Linux/AArch64 port.
//
// Replaces the no-op NullSound backend with a real SDL2 audio device:
//   * loads RIFF/WAVE assets through the CryPak VFS (inside fcdata/*.pak),
//   * decodes them once to 16-bit stereo at the device sample rate,
//   * mixes up to MAX_VOICES queued sounds in the SDL audio callback,
//   * implements loop, volume, pitch/ratio, pan, simple distance attenuation,
//   * provides the IMusicSystem interface used by the engine/scripts.
//
// Music/cutscene sounds requested with FLAG_SOUND_STREAM are loaded eagerly so
// IsLoaded()/SOUND_EVENT_ON_LOADED work as the engine expects. OGG streams are
// not decoded by this backend (no Vorbis library is linked); they fail
// gracefully and are logged once, while WAV streams play normally.
//
// If SDL audio cannot be initialised (no device, headless run without the
// dummy driver, FARCRY_SDL_AUDIO=0) the backend degrades to the previous
// silent behaviour instead of failing engine startup.

#include "platform.h"
#include <ISound.h>
#include <ISystem.h>
#include <ICryPak.h>
#include <ILog.h>
#include <IConsole.h>
#include <Cry_Camera.h>
#include <SDL.h>
#include <vector>

// External PCM stream mixed into the game's output: movie audio from the Bink
// player (BinkPort.cpp), which cannot open a second device on most handhelds.
// Stereo S16 at the device rate, a ring buffer guarded by the device lock.
static SDL_AudioDeviceID g_portAudioDevice = 0;
static int g_portAudioFreq = 0;
static std::vector<Sint16> g_extRing;
static size_t g_extRead = 0, g_extCount = 0; // in frames
static bool g_extPaused = false;
int PortAudio_ExternalRate() { return g_portAudioDevice ? g_portAudioFreq : 0; }
int PortAudio_ExternalWrite(const Sint16* lr, int frames)
{
    if (!g_portAudioDevice || frames <= 0) return 0;
    SDL_LockAudioDevice(g_portAudioDevice);
    if (g_extRing.empty()) g_extRing.resize((size_t)g_portAudioFreq * 4 * 2);
    const size_t cap = g_extRing.size() / 2;
    int written = 0;
    for (; written < frames && g_extCount < cap; ++written, ++g_extCount)
    {
        const size_t w = (g_extRead + g_extCount) % cap;
        g_extRing[w * 2] = lr[written * 2]; g_extRing[w * 2 + 1] = lr[written * 2 + 1];
    }
    SDL_UnlockAudioDevice(g_portAudioDevice);
    return written;
}
int PortAudio_ExternalQueued() { return (int)g_extCount; }
void PortAudio_ExternalClear() { if (!g_portAudioDevice) return; SDL_LockAudioDevice(g_portAudioDevice); g_extRead = g_extCount = 0; SDL_UnlockAudioDevice(g_portAudioDevice); }
void PortAudio_ExternalPause(bool p) { g_extPaused = p; }
static long long g_extMixed = 0;   // frames of movie audio mixed so far (diagnostic log)
long long PortAudio_ExternalMixed() { return g_extMixed; }
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <vector>
#include <map>
#include <string>

#define SDL2SND_MAX_VOICES 64
#define SDL2SND_MUSIC_VOLUME 0.7f

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static bool PortSndEnvFlag(const char* szName, bool bDefault)
{
    const char* s = getenv(szName);
    if (!s || !*s) return bDefault;
    return !(s[0] == '0' && s[1] == 0);
}

static void PortSndSetEnvDefault(const char* szName, const char* szValue)
{
    const char* cur = getenv(szName);
    if (!cur || !*cur) SDL_setenv(szName, szValue, 1);
}

static float SndDistance(const Vec3& a, const Vec3& b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

// ---------------------------------------------------------------------------
// WAV loading / conversion
// ---------------------------------------------------------------------------
static unsigned SndReadU32(const unsigned char* p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned short SndReadU16(const unsigned char* p)
{
    return (unsigned short)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static bool SndReadFile(ISystem* pSystem, const char* szFile, std::vector<unsigned char>& data)
{
    if (!pSystem || !szFile || !szFile[0]) return false;
    ICryPak* pPak = pSystem->GetIPak();
    if (!pPak) return false;
    FILE* f = pPak->FOpen(szFile, "rb", ICryPak::FOPEN_HINT_QUIET);
    if (!f) return false;
    pPak->FSeek(f, 0, SEEK_END);
    long nSize = pPak->FTell(f);
    pPak->FSeek(f, 0, SEEK_SET);
    if (nSize <= 0) { pPak->FClose(f); return false; }
    data.resize((size_t)nSize);
    size_t nRead = pPak->FRead(&data[0], 1, (size_t)nSize, f);
    pPak->FClose(f);
    if (nRead != (size_t)nSize) { data.clear(); return false; }
    return true;
}

extern "C" {
int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);
}

// Decode Ogg Vorbis through the vendored public-domain stb_vorbis.
static bool SndDecodeOgg(const unsigned char* data, size_t size, int outFreq,
                         std::vector<short>& outPcm, int channels = 2)
{
    if (!data || size < 4 || size > 0x7fffffff || outFreq <= 0 || (channels != 1 && channels != 2)) return false;
    int srcChannels = 0, srcRate = 0;
    short* decoded = NULL;
    int frames = stb_vorbis_decode_memory(data, (int)size, &srcChannels, &srcRate, &decoded);
    if (frames <= 0 || !decoded || srcChannels <= 0 || srcRate <= 0) {
        if (decoded) free(decoded);
        return false;
    }
    size_t bytes = (size_t)frames * (size_t)srcChannels * sizeof(short);
    if (bytes > 0x7fffffffU) { free(decoded); return false; }
    SDL_AudioCVT cvt;
    int result = SDL_BuildAudioCVT(&cvt, AUDIO_S16SYS, (Uint8)srcChannels, srcRate,
                                   AUDIO_S16SYS, (Uint8)channels, outFreq);
    if (result < 0 || cvt.len_mult < 1 || bytes > 0x7fffffffU / (unsigned)cvt.len_mult) {
        free(decoded); return false;
    }
    cvt.len = (int)bytes;
    cvt.buf = (Uint8*)SDL_malloc(bytes * (size_t)cvt.len_mult);
    if (!cvt.buf) { free(decoded); return false; }
    memcpy(cvt.buf, decoded, bytes);
    free(decoded);
    if (result > 0 && SDL_ConvertAudio(&cvt) != 0) { SDL_free(cvt.buf); return false; }
    int length = result > 0 ? cvt.len_cvt : cvt.len;
    outPcm.resize((size_t)length / sizeof(short));
    if (length) memcpy(&outPcm[0], cvt.buf, outPcm.size() * sizeof(short));
    SDL_free(cvt.buf);
    return !outPcm.empty();
}

// Decode retail PCM and Microsoft/IMA ADPCM through SDL's bounded RIFF reader.
// Keep device conversion outside the audio callback and outside the mixer lock.
static bool SndDecodeWav(const unsigned char* data, size_t size, int outFreq,
                         std::vector<short>& outPcm, int channels = 2)
{
    if (!data || size < 12 || size > 0x7fffffff || outFreq <= 0 || (channels != 1 && channels != 2)) return false;
    SDL_RWops* rw = SDL_RWFromConstMem(data, (int)size);
    if (!rw) return false;
    SDL_AudioSpec spec;
    Uint8* decoded = NULL;
    Uint32 bytes = 0;
    if (!SDL_LoadWAV_RW(rw, 1, &spec, &decoded, &bytes)) return false;
    SDL_AudioCVT cvt;
    int result = SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                                   AUDIO_S16SYS, (Uint8)channels, outFreq);
    if (result < 0 || !bytes || cvt.len_mult < 1 ||
        bytes > 0x7fffffffU / (unsigned)cvt.len_mult) {
        SDL_FreeWAV(decoded); return false;
    }
    cvt.len = (int)bytes;
    cvt.buf = (Uint8*)SDL_malloc((size_t)bytes * cvt.len_mult);
    if (!cvt.buf) { SDL_FreeWAV(decoded); return false; }
    memcpy(cvt.buf, decoded, bytes);
    SDL_FreeWAV(decoded);
    if (result > 0 && SDL_ConvertAudio(&cvt) != 0) {
        SDL_free(cvt.buf); return false;
    }
    int length = result > 0 ? cvt.len_cvt : cvt.len;
    outPcm.resize((size_t)length / sizeof(short));
    if (length) memcpy(&outPcm[0], cvt.buf, outPcm.size() * sizeof(short));
    SDL_free(cvt.buf);
    return !outPcm.empty();
}

// ---------------------------------------------------------------------------
// forward declarations
// ---------------------------------------------------------------------------
class CSDL2Sound;
class CSDL2MusicSystem;
class CSDL2SoundSystem;

// ---------------------------------------------------------------------------
// ISound implementation
// ---------------------------------------------------------------------------
class CSDL2Sound : public ISound
{
public:
    CSDL2Sound(CSDL2SoundSystem* pOwner, const char* szName, int nFlags, int nId);
    virtual ~CSDL2Sound();

    // ISound
    void AddEventListener(ISoundEventListener* pListener);
    void RemoveEventListener(ISoundEventListener* pListener);
    // A play waiting for the background decoder counts as playing: entities
    // drop non-playing sounds from the list they stop later, and scripts stop
    // engine loops only when they report playing (else the loop starts after
    // decoding and is never stopped).
    bool IsPlaying() { return m_bPlaying || m_bPlayPending; }
    bool IsPlayingVirtual() { return m_bPlaying || m_bPlayPending; }
    bool IsLoading() { return m_bLoading; }
    bool IsLoaded() { return m_bLoaded; }
    void Play(float fVolumeScale = 1.0f, bool bForceActiveState = true, bool bSetRatio = true);
    void PlayFadeUnderwater(float fVolumeScale = 1.0f, bool bForceActiveState = true, bool bSetRatio = true) { Play(fVolumeScale, bForceActiveState, bSetRatio); }
    void Stop();
    const char* GetName() { return m_szName.c_str(); }
    const int GetId() { return m_nId; }
    void SetLoopMode(bool bLoop) { m_bLoop = bLoop; }
    bool Preload();
    unsigned int GetCurrentSamplePos(bool bMilliSeconds = false);
    void SetCurrentSamplePos(unsigned int nPos, bool bMilliSeconds);
    void SetPitching(float fPitching) { m_fPitching = fPitching; }
    void SetRatio(float fRatio);
    int GetFrequency() { return m_nFreq; }
    void SetPitch(int nPitch);
    void SetPan(int nPan);
    void SetMinMaxDistance(float fMinDist, float fMaxDist);
    void SetConeAngles(float, float) {}
    void AddToScaleGroup(int nGroup);
    void RemoveFromScaleGroup(int nGroup);
    void SetScaleGroup(unsigned int nGroupBits) { m_nScaleGroup = nGroupBits; }
    void SetVolume(int nVolume);
    int GetVolume() { return m_nVolume; }
    void SetPosition(const Vec3& pos) { m_vPos = pos; }
    const bool GetPosition(Vec3& pos) { pos = m_vPos; return true; }
    void SetVelocity(const Vec3& vel) { m_vVel = vel; }
    Vec3 GetVelocity() { return m_vVel; }
    void SetDirection(const Vec3& dir) { m_vDir = dir; }
    Vec3 GetDirection() { return m_vDir; }
    void SetLoopPoints(const int, const int) {}
    bool IsRelative() const { return m_bRelative; }
    int AddRef() { return ++m_nRefs; }
    int Release();
    void SetSoundProperties(float fFading) { m_fFading = fFading; }
    void FXEnable(int) {}
    void FXSetParamEQ(float, float, float) {}
    int GetLengthMs();
    int GetLength();
    void SetSoundPriority(unsigned char nPriority) { m_nPriority = nPriority; }

    // backend internals
    bool EnsureLoaded();
    // Background decoding (see CSDL2SoundSystem::QueueDecode): the decoded
    // samples arrive in Update(); a Play() made before that starts then.
    void StartPlay(float fVolumeScale);
    void OnDecoded(const std::shared_ptr<std::vector<short> >& pcm, int channels, bool ok);
    std::string SampleKey() const { return m_szName + (m_nChannels == 1 ? "#mono" : "#stereo"); }
    int Channels() const { return m_nChannels; }
    bool m_bQueued = false, m_bPlayPending = false;
    float m_fPendingVolume = 1.0f;
    Uint32 m_nPendingAt = 0;
    const std::vector<short>* Samples() const { return m_pcm.get(); }
    void Mix(int* pOut, int nFrames, const Vec3& vListenerPos, float fMaster, float fMusic);
    bool IsFinished() const { return m_bFinished; }
    void ResetFinished() { m_bFinished = false; }
    bool IsMusic() const { return m_bMusic; }
    void DetachOwner() { m_pOwner = NULL; m_bPlaying = false; }
    unsigned int GetScaleGroupMask() const { return m_nScaleGroup; }
    float GetFading() const { return m_fFading; }

    void NotifyEvent(ESoundCallbackEvent event);

private:
    CSDL2SoundSystem* m_pOwner;
    std::string m_szName;
    int m_nFlags;
    int m_nId;
    int m_nRefs;
    int m_nVolume;
    int m_nPriority;
    bool m_bLoaded;
    bool m_bLoading;
    bool m_bLoadFailed;
    bool m_bPlaying;
    bool m_bFinished;
    bool m_bLoop;
    bool m_bRelative;
    bool m_bMusic;
    bool m_bStream;
    float m_fPlayVolume;
    float m_fPitch;      // 1.0 = normal
    float m_fPitching;
    float m_fRatio;
    float m_fPan;        // -1..1
    float m_fMinDist;
    float m_fMaxDist;
    float m_fFading;
    float m_fPos;        // position in output frames (double precision kept as float ok for short sounds)
    unsigned int m_nScaleGroup;
    int m_nFreq;
    int m_nChannels;
    Vec3 m_vPos, m_vVel, m_vDir;
    std::shared_ptr<std::vector<short> > m_pcm;
    std::vector<ISoundEventListener*> m_listeners;
};

// ---------------------------------------------------------------------------
// Sound system
// ---------------------------------------------------------------------------
class CSDL2SoundSystem : public ISoundSystem
{
public:
    CSDL2SoundSystem(ISystem* pSystem);
    virtual ~CSDL2SoundSystem();

    // ISoundSystem
    void Release() { delete this; }
    void Update();
    IMusicSystem* CreateMusicSystem();
    ISound* LoadSound(const char* szFile, int nFlags);
    void SetMasterVolume(unsigned char nVol) { m_nMasterVolume = nVol; }
    void SetMasterVolumeScale(float fScale, bool) { m_fMasterScale = fScale; }
    ISound* GetSound(int nSoundId);
    void PlaySound(int nSoundId);
    void SetListener(const CCamera& cCam, const Vec3& vVel);
    void RecomputeSoundOcclusion(bool, bool, bool) {}
    bool IsEAX(int) { return false; }
    bool SetEaxListenerEnvironment(int, CS_REVERB_PROPERTIES*, int) { return false; }
    bool GetCurrentEaxEnvironment(int& nPreset, CS_REVERB_PROPERTIES&) { nPreset = 0; return false; }
    bool SetGroupScale(int nGroup, float fScale);
    void Silence();
    void Pause(bool bPause, bool) { m_bPaused = bPause; }
    void Mute(bool bMute) { if (bMute != m_bMuted) fprintf(stderr, "[port/audio] game sound %s\n", bMute ? "muted (loading)" : "unmuted"); m_bMuted = bMute; }
    void GetSoundMemoryUsageInfo(size_t& nCurrentMemory, size_t& nMaxMemory);
    int GetUsedVoices();
    float GetCPUUsage() { return 0.0f; }
    float GetMusicVolume() { return SDL2SND_MUSIC_VOLUME; }
    void CalcDirectionalAttenuation(Vec3&, Vec3&, float) {}
    float GetDirectionalAttenuationMaxScale() { return 1.0f; }
    bool UsingDirectionalAttenuation() { return false; }
    void GetMemoryUsage(class ICrySizer*) {}
    IVisArea* GetListenerArea() { return NULL; }
    Vec3 GetListenerPos() { return m_vListenerPos; }
    bool DebuggingSound() { return false; }
    int SetMinSoundPriority(int nPriority) { int nPrev = m_nMinPriority; m_nMinPriority = nPriority; return nPrev; }
    void LockResources() {}
    void UnlockResources() {}

    // backend internals
    static void SDLCALL SDLCallback(void* pUserData, Uint8* pStream, int nLen);
    void AudioCallback(Sint16* pStream, int nLenBytes);
    SDL_AudioDeviceID GetDevice() const { return m_nDevice; }
    void OnSoundReleased(CSDL2Sound* pSound);
    std::shared_ptr<std::vector<short> > FindSamples(const std::string& name);
    void CacheSamples(const std::string& name, const std::shared_ptr<std::vector<short> >& samples);
    void AddVoice(CSDL2Sound* pSound);
    void RemoveVoice(CSDL2Sound* pSound);
    float GetGroupScale(unsigned int nMask) const;
    int GetDeviceFreq() const { return m_nDeviceFreq; }
    bool IsAudioReady() const { return m_nDevice != 0; }
    ISystem* GetSystem() const { return m_pSystem; }
    void Log(const char* szFormat, ...);

private:
    ISystem* m_pSystem;
    SDL_AudioDeviceID m_nDevice;
    int m_nDeviceFreq;
    unsigned char m_nMasterVolume;
    float m_fMasterScale;
    bool m_bPaused;
    bool m_bMuted;
    int m_nNextId;
    int m_nMinPriority;
    float m_groupScale[32];
    Vec3 m_vListenerPos, m_vListenerVel;
    std::vector<CSDL2Sound*> m_vecVoices;
    std::map<int, CSDL2Sound*> m_mapSounds;
    std::vector<int> m_vecScratch;
    std::map<std::string, std::weak_ptr<std::vector<short> > > m_sampleCache;
    size_t m_nLoadedBytes;
    bool m_bLoggedOgg;
public:
    // Background decoder: reading a sound from the paks and decoding it took
    // 10-50 ms on the H700 (ambient loops up to a second), on the main thread
    // at the sound's first play - a frame hitch for every new sound in a fight.
    // FARCRY_SOUND_SYNC=1: the old synchronous loading.
    bool QueueDecode(CSDL2Sound* pSound);
private:
    struct SDecodeJob { int id; std::string name, key; int channels; };
    struct SDecodeResult { int id; std::string key; std::shared_ptr<std::vector<short> > pcm; int channels; bool ok; Uint32 ms; std::string name; };
    void DecodeWorker();
    void DrainDecoded();
    std::thread m_decodeThread;
    std::mutex m_decodeMx;
    std::condition_variable m_decodeCv;
    std::deque<SDecodeJob> m_decodeJobs;
    std::vector<SDecodeResult> m_decodeDone;
    bool m_decodeQuit = false;
};

// ---------------------------------------------------------------------------
// Sound system implementation
// ---------------------------------------------------------------------------
CSDL2SoundSystem::CSDL2SoundSystem(ISystem* pSystem)
    : m_pSystem(pSystem), m_nDevice(0), m_nDeviceFreq(44100), m_nMasterVolume(255),
      m_fMasterScale(1.0f), m_bPaused(false), m_bMuted(false), m_nNextId(1),
      m_nMinPriority(0), m_nLoadedBytes(0), m_bLoggedOgg(false)
{
    for (int i = 0; i < 32; ++i) m_groupScale[i] = 1.0f;
    m_vListenerPos = Vec3(0,0,0);

    // The sound cvars Crytek's sound system registers (SoundSystemCommon.cpp).
    // Game scripts read them: the cutscene player sets the movie volume from
    // s_SFXVolume and silences movies when s_SoundEnable is missing or 0
    // (intro videos had no sound). Same names, defaults and flags as the original.
    if (IConsole* pConsole = pSystem ? pSystem->GetIConsole() : NULL)
    {
        if (!pConsole->GetCVar("s_SoundEnable"))
            pConsole->CreateVariable("s_SoundEnable", "1", VF_DUMPTODISK, "Toggles sound on and off.\nUsage: s_SoundEnable [0/1]\nDefault is 1 (on).");
        if (!pConsole->GetCVar("s_SFXVolume"))
            pConsole->CreateVariable("s_SFXVolume", "1", VF_DUMPTODISK, "Sets the percentile volume of the sound effects.\nUsage: s_SFXVolume 0.5\nDefault is 1, which is full volume.");
        if (!pConsole->GetCVar("s_MusicVolume"))
            pConsole->CreateVariable("s_MusicVolume", "0.5", VF_DUMPTODISK, "Sets the music volume from 0 to 1 in the games options.\nUsage: s_MusicVolume 0.5\nDefault is 0.5.");
    }

    if (!PortSndEnvFlag("FARCRY_SDL_AUDIO", true))
    {
        Log("FARCRY_SDL_AUDIO=0, using silent fallback");
        return;
    }

    const char* szDriver = getenv("FARCRY_SDL_AUDIO_DRIVER");
    if (szDriver && *szDriver)
        SDL_setenv("SDL_AUDIODRIVER", szDriver, 1);
#ifdef FARCRY_AUDIO_DUMMY_DRIVER
    PortSndSetEnvDefault("SDL_AUDIODRIVER", "dummy");
#endif

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        Log("SDL audio init failed (%s), using silent fallback", SDL_GetError());
        return;
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    memset(&have, 0, sizeof(have));
    int outputRate = 44100;
    if (const char* rate = getenv("FARCRY_AUDIO_RATE")) {
        int requested = atoi(rate);
        if (requested == 22050 || requested == 44100 || requested == 48000) outputRate = requested;
    }
    want.freq = outputRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = outputRate <= 22050 ? 512 : 1024;
    want.callback = &CSDL2SoundSystem::SDLCallback;
    want.userdata = this;

    m_nDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (m_nDevice == 0)
    {
        Log("SDL_OpenAudioDevice failed (%s), using silent fallback", SDL_GetError());
        return;
    }
    m_nDeviceFreq = have.freq > 0 ? have.freq : 44100;
    g_portAudioDevice = m_nDevice; g_portAudioFreq = m_nDeviceFreq;
    m_vecScratch.resize((size_t)have.samples * 2 + 64);
    SDL_PauseAudioDevice(m_nDevice, 0);
    Log("SDL2 audio device opened: %d Hz, %d channels, %d samples (driver: %s)",
        have.freq, have.channels, have.samples,
        SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");

    // Env-gated self test: decode and queue a WAV from the paks and load a
    // stream (OGG) so the smoke test can verify the backend without gameplay.
    if (PortSndEnvFlag("FARCRY_SDL_AUDIO_SMOKE", false))
    {
        ISound* pWav = LoadSound("Sounds/ai/get_down.wav", FLAG_SOUND_2D);
        if (pWav)
        {
            bool bOk = pWav->Preload();
            Log("smoke: WAV %s, loaded=%d, length=%d ms",
                bOk ? "decoded" : "FAILED", pWav->IsLoaded() ? 1 : 0, pWav->GetLengthMs());
            pWav->SetVolume(200);
            pWav->Play();
            Log("smoke: WAV queued, used voices=%d", GetUsedVoices());
            pWav->Release();
        }
        ISound* pStream = LoadSound("Music/menu/MenuMusic_shortversion.ogg",
                                    FLAG_SOUND_STREAM | FLAG_SOUND_MUSIC | FLAG_SOUND_LOOP);
        if (pStream)
        {
            Log("smoke: OGG stream loaded=%d", pStream->IsLoaded() ? 1 : 0);
            pStream->Release();
        }
    }
}

CSDL2SoundSystem::~CSDL2SoundSystem()
{
    if (m_decodeThread.joinable())
    {
        { std::lock_guard<std::mutex> lk(m_decodeMx); m_decodeQuit = true; m_decodeJobs.clear(); }
        m_decodeCv.notify_all();
        m_decodeThread.join();
    }
    if (m_nDevice)
    {
        SDL_PauseAudioDevice(m_nDevice, 1);
        SDL_CloseAudioDevice(m_nDevice);
        m_nDevice = 0;
    }
    // If the engine still holds ISound objects when the system is released,
    // detach them so their destructors do not touch a dead back-pointer.
    for (std::map<int, CSDL2Sound*>::iterator it = m_mapSounds.begin(); it != m_mapSounds.end(); ++it)
        if (it->second) it->second->DetachOwner();
    m_mapSounds.clear();
    m_vecVoices.clear();
}

void CSDL2SoundSystem::SDLCallback(void* pUserData, Uint8* pStream, int nLen)
{
    CSDL2SoundSystem* pThis = (CSDL2SoundSystem*)pUserData;
    if (pThis) pThis->AudioCallback((Sint16*)pStream, nLen);
}

void CSDL2SoundSystem::Log(const char* szFormat, ...)
{
    char szBuffer[1024];
    va_list args;
    va_start(args, szFormat);
    vsnprintf(szBuffer, sizeof(szBuffer), szFormat, args);
    va_end(args);
    fprintf(stderr, "[port/audio] %s\n", szBuffer);
    if (m_pSystem)
        if (ILog* pLog = m_pSystem->GetILog())
            pLog->Log("port/audio: %s", szBuffer);
}

void CSDL2SoundSystem::AudioCallback(Sint16* pStream, int nLenBytes)
{
    const int nFrames = nLenBytes / 4; // 16-bit stereo
    if ((int)m_vecScratch.size() < nFrames * 2)
        m_vecScratch.resize((size_t)nFrames * 2);
    int* pMix = &m_vecScratch[0];
    memset(pMix, 0, sizeof(int) * (size_t)nFrames * 2);

    if (!m_bPaused && !m_bMuted)
    {
        float fMaster = (float)m_nMasterVolume / 255.0f * m_fMasterScale;
        for (size_t i = 0; i < m_vecVoices.size(); ++i)
        {
            CSDL2Sound* pSound = m_vecVoices[i];
            if (pSound->IsPlaying())
                pSound->Mix(pMix, nFrames, m_vListenerPos, fMaster, SDL2SND_MUSIC_VOLUME);
        }
    }

    // Movie audio is not affected by Mute(): the game mutes its sounds while it
    // loads, and the intro videos play during the menu load (their sound cut
    // out or never started, depending on timing). Pause/volume still apply.
    if (g_extCount && !g_extPaused)
    {
        const size_t cap = g_extRing.size() / 2;
        const int n = (int)(g_extCount < (size_t)nFrames ? g_extCount : (size_t)nFrames);
        for (int f = 0; f < n; ++f)
        {
            const size_t r = (g_extRead + f) % cap;
            pMix[f * 2] += g_extRing[r * 2]; pMix[f * 2 + 1] += g_extRing[r * 2 + 1];
        }
        g_extRead = (g_extRead + n) % cap; g_extCount -= n; g_extMixed += n;
    }

    for (int i = 0; i < nFrames * 2; ++i)
    {
        int v = pMix[i];
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        pStream[i] = (Sint16)v;
    }
}

void CSDL2SoundSystem::Update()
{
    DrainDecoded();
    if (getenv("FARCRY_MEMORY_TRACE")) {
        static Uint32 last = 0;
        if (SDL_GetTicks() - last > 2000) {
            last = SDL_GetTicks(); size_t current, peak;
            GetSoundMemoryUsageInfo(current, peak);
            Log("memory PCM=%u KiB peak=%u KiB sounds=%u", (unsigned)(current/1024), (unsigned)(peak/1024), (unsigned)m_mapSounds.size());
        }
    }
    if (!m_nDevice) return;
    // Remove finished voices under the audio lock, but deliver the STOP events
    // after unlocking: listeners (scripts/HUD/subtitles) may do non-trivial work
    // and must never block the audio callback thread.
    std::vector<CSDL2Sound*> finished;
    SDL_LockAudioDevice(m_nDevice);
    for (size_t i = 0; i < m_vecVoices.size(); )
    {
        CSDL2Sound* pSound = m_vecVoices[i];
        if (pSound->IsFinished() || !pSound->IsPlaying())
        {
            m_vecVoices.erase(m_vecVoices.begin() + (long)i);
            finished.push_back(pSound);
            continue;
        }
        ++i;
    }
    SDL_UnlockAudioDevice(m_nDevice);
    for (size_t i = 0; i < finished.size(); ++i)
        finished[i]->NotifyEvent(SOUND_EVENT_ON_STOP);
}

ISound* CSDL2SoundSystem::LoadSound(const char* szFile, int nFlags)
{
    if (!szFile || !szFile[0]) return NULL;
    if (nFlags & FLAG_SOUND_3D) nFlags |= FLAG_SOUND_RADIUS;
    CSDL2Sound* pSound = new CSDL2Sound(this, szFile, nFlags, m_nNextId++);
    m_mapSounds[pSound->GetId()] = pSound;
    if (nFlags & FLAG_SOUND_STREAM)
        pSound->EnsureLoaded();
    return pSound;
}

ISound* CSDL2SoundSystem::GetSound(int nSoundId)
{
    std::map<int, CSDL2Sound*>::iterator it = m_mapSounds.find(nSoundId);
    return (it == m_mapSounds.end()) ? NULL : it->second;
}

void CSDL2SoundSystem::PlaySound(int nSoundId)
{
    ISound* pSound = GetSound(nSoundId);
    if (pSound) pSound->Play();
}

void CSDL2SoundSystem::SetListener(const CCamera& cCam, const Vec3& vVel)
{
    m_vListenerPos = cCam.GetPos();
    m_vListenerVel = vVel;
}

bool CSDL2SoundSystem::SetGroupScale(int nGroup, float fScale)
{
    if (nGroup < 0 || nGroup >= 32) return false;
    m_groupScale[nGroup] = fScale;
    return true;
}

float CSDL2SoundSystem::GetGroupScale(unsigned int nMask) const
{
    float fScale = 1.0f;
    for (int i = 0; i < 32; ++i)
        if (nMask & (1u << i))
            fScale *= m_groupScale[i];
    return fScale;
}

void CSDL2SoundSystem::Silence()
{
    if (!m_nDevice) return;
    std::vector<CSDL2Sound*> voices;
    SDL_LockAudioDevice(m_nDevice);
    voices.swap(m_vecVoices);
    SDL_UnlockAudioDevice(m_nDevice);
    for (size_t i = 0; i < voices.size(); ++i)
        voices[i]->Stop();
}

static std::string SampleKey(const std::string& name)
{
    std::string key = name;
    for (size_t i = 0; i < key.size(); ++i)
        key[i] = key[i] == '\\' ? '/' : (char)tolower((unsigned char)key[i]);
    return key;
}
std::shared_ptr<std::vector<short> > CSDL2SoundSystem::FindSamples(const std::string& name)
{
    return m_sampleCache[SampleKey(name)].lock();
}
void CSDL2SoundSystem::CacheSamples(const std::string& name, const std::shared_ptr<std::vector<short> >& samples)
{
    // Weak ownership: unused samples disappear with the last sound object.
    m_sampleCache[SampleKey(name)] = samples;
}
void CSDL2SoundSystem::GetSoundMemoryUsageInfo(size_t& current, size_t& peak)
{
    std::set<const std::vector<short>*> unique;
    current = 0;
    for (std::map<int, CSDL2Sound*>::iterator it = m_mapSounds.begin(); it != m_mapSounds.end(); ++it) {
        const std::vector<short>* samples = it->second->Samples();
        if (samples && unique.insert(samples).second) current += samples->capacity() * sizeof(short);
    }
    if (current > m_nLoadedBytes) m_nLoadedBytes = current;
    peak = m_nLoadedBytes;
}

int CSDL2SoundSystem::GetUsedVoices()
{
    int nUsed = 0;
    for (size_t i = 0; i < m_vecVoices.size(); ++i)
        if (m_vecVoices[i]->IsPlaying()) ++nUsed;
    return nUsed;
}

void CSDL2SoundSystem::AddVoice(CSDL2Sound* pSound)
{
    if (!pSound) return;
    bool bFound = false;
    for (size_t i = 0; i < m_vecVoices.size(); ++i)
        if (m_vecVoices[i] == pSound) { bFound = true; break; }
    if (!bFound)
    {
        if (m_vecVoices.size() >= SDL2SND_MAX_VOICES)
        {
            // steal the oldest voice
            m_vecVoices[0]->Stop();
            m_vecVoices.erase(m_vecVoices.begin());
        }
        m_vecVoices.push_back(pSound);
    }
}

void CSDL2SoundSystem::RemoveVoice(CSDL2Sound* pSound)
{
    for (size_t i = 0; i < m_vecVoices.size(); ++i)
    {
        if (m_vecVoices[i] == pSound)
        {
            m_vecVoices.erase(m_vecVoices.begin() + (long)i);
            return;
        }
    }
}

bool CSDL2SoundSystem::QueueDecode(CSDL2Sound* pSound)
{
    static const bool s_sync = PortSndEnvFlag("FARCRY_SOUND_SYNC", false);
    if (s_sync || !pSound || !m_pSystem) return false;
    if (pSound->m_bQueued) return true;
    if (FindSamples(pSound->SampleKey())) return false;   // cached: loads instantly
    if (!m_decodeThread.joinable())
        m_decodeThread = std::thread(&CSDL2SoundSystem::DecodeWorker, this);
    pSound->m_bQueued = true;
    {
        std::lock_guard<std::mutex> lk(m_decodeMx);
        m_decodeJobs.push_back(SDecodeJob{ pSound->GetId(), pSound->GetName(), pSound->SampleKey(), pSound->Channels() });
    }
    m_decodeCv.notify_one();
    return true;
}

void CSDL2SoundSystem::DecodeWorker()
{
    for (;;)
    {
        SDecodeJob job;
        {
            std::unique_lock<std::mutex> lk(m_decodeMx);
            m_decodeCv.wait(lk, [this] { return m_decodeQuit || !m_decodeJobs.empty(); });
            if (m_decodeQuit) return;
            job = m_decodeJobs.front(); m_decodeJobs.pop_front();
        }
        SDecodeResult res; res.id = job.id; res.key = job.key; res.name = job.name; res.channels = job.channels; res.ok = false;
        const Uint32 t0 = SDL_GetTicks();
        std::vector<unsigned char> data;
        if (SndReadFile(m_pSystem, job.name.c_str(), data) && !data.empty())
        {
            res.pcm.reset(new std::vector<short>);
            res.ok = SndDecodeWav(&data[0], data.size(), m_nDeviceFreq, *res.pcm, res.channels) ||
                     SndDecodeOgg(&data[0], data.size(), m_nDeviceFreq, *res.pcm, res.channels);
        }
        res.ms = SDL_GetTicks() - t0;
        std::lock_guard<std::mutex> lk(m_decodeMx);
        m_decodeDone.push_back(res);
    }
}

void CSDL2SoundSystem::DrainDecoded()
{
    std::vector<SDecodeResult> done;
    { std::lock_guard<std::mutex> lk(m_decodeMx); done.swap(m_decodeDone); }
    static const bool s_debug = PortSndEnvFlag("FARCRY_SDL_AUDIO_DEBUG", false);
    for (size_t i = 0; i < done.size(); ++i)
    {
        const SDecodeResult& r = done[i];
        if (s_debug) Log("decoded in background '%s': %s in %u ms", r.name.c_str(), r.ok ? "ok" : "FAILED", (unsigned)r.ms);
        std::map<int, CSDL2Sound*>::iterator it = m_mapSounds.find(r.id);
        if (it == m_mapSounds.end() || !it->second) continue;   // released meanwhile
        it->second->OnDecoded(r.pcm, r.channels, r.ok);
    }
}

void CSDL2Sound::OnDecoded(const std::shared_ptr<std::vector<short> >& pcm, int channels, bool ok)
{
    m_bQueued = false;
    if (m_bLoaded) { m_bPlayPending = false; return; }   // loaded synchronously meanwhile
    if (!ok || !pcm)
    {
        m_bLoadFailed = true; m_bLoading = false; m_bPlayPending = false;
        if (m_pOwner) m_pOwner->Log("unsupported, corrupt or missing audio: %s", m_szName.c_str());
        NotifyEvent(SOUND_EVENT_ON_LOAD_FAILED);
        return;
    }
    m_pcm = pcm;
    m_nChannels = channels;
    if (m_pOwner) { m_pOwner->CacheSamples(SampleKey(), m_pcm); m_nFreq = m_pOwner->GetDeviceFreq(); }
    m_bLoaded = true; m_bLoading = false;
    NotifyEvent(SOUND_EVENT_ON_LOADED);
    // A play requested while decoding starts now, unless it would be too late
    // to belong to its event (a shot or impact from well over half a second ago).
    if (m_bPlayPending)
    {
        m_bPlayPending = false;
        if (m_bLoop || SDL_GetTicks() - m_nPendingAt < 700) StartPlay(m_fPendingVolume);
    }
}

void CSDL2SoundSystem::OnSoundReleased(CSDL2Sound* pSound)
{
    RemoveVoice(pSound);
    for (std::map<int, CSDL2Sound*>::iterator it = m_mapSounds.begin(); it != m_mapSounds.end(); ++it)
    {
        if (it->second == pSound) { m_mapSounds.erase(it); break; }
    }
}

// ---------------------------------------------------------------------------
// ISound implementation
// ---------------------------------------------------------------------------
// Match CSound: the caller (Lua userdata or _smart_ptr) acquires the first reference.
CSDL2Sound::CSDL2Sound(CSDL2SoundSystem* pOwner, const char* szName, int nFlags, int nId)
    : m_pOwner(pOwner), m_szName(szName ? szName : ""), m_nFlags(nFlags), m_nId(nId), m_nRefs(0),
      m_nVolume(255), m_nPriority(128), m_bLoaded(false), m_bLoading(false), m_bLoadFailed(false),
      m_bPlaying(false), m_bFinished(false), m_bLoop((nFlags & FLAG_SOUND_LOOP) != 0),
      m_bRelative((nFlags & FLAG_SOUND_RELATIVE) != 0),
      m_bMusic((nFlags & FLAG_SOUND_MUSIC) != 0),
      m_bStream((nFlags & FLAG_SOUND_STREAM) != 0),
      m_fPlayVolume(1.0f), m_fPitch(1.0f), m_fPitching(0.0f), m_fRatio(1.0f), m_fPan(0.0f),
      m_fMinDist(1.0f), m_fMaxDist(10000.0f), m_fFading(1.0f), m_fPos(0.0f), m_nScaleGroup(0),
      m_nFreq(22050), m_nChannels((nFlags & FLAG_SOUND_3D) && !(nFlags & FLAG_SOUND_STEREO) && PortSndEnvFlag("FARCRY_AUDIO_MONO_3D", false) ? 1 : 2), m_pcm(new std::vector<short>)
{
    if (nFlags & FLAG_SOUND_UNSCALABLE)
        m_nScaleGroup = 0;
    else
        m_nScaleGroup = (1u << SOUNDSCALE_SCALEABLE);
    if (!m_bMusic)
        m_nScaleGroup |= (1u << SOUNDSCALE_MASTER);
    m_vPos = Vec3(0,0,0);
    m_vVel = Vec3(0,0,0);
    m_vDir = Vec3(0,0,0);
}

CSDL2Sound::~CSDL2Sound()
{
    if (m_pOwner)
    {
        if (m_pOwner->IsAudioReady())
            SDL_LockAudioDevice(m_pOwner->GetDevice());
        m_bPlaying = false;
        m_pOwner->OnSoundReleased(this);
        if (m_pOwner->IsAudioReady())
            SDL_UnlockAudioDevice(m_pOwner->GetDevice());
    }
}

void CSDL2Sound::AddEventListener(ISoundEventListener* pListener)
{
    for (size_t i = 0; i < m_listeners.size(); ++i)
        if (m_listeners[i] == pListener) return;
    m_listeners.push_back(pListener);
}

void CSDL2Sound::RemoveEventListener(ISoundEventListener* pListener)
{
    for (size_t i = 0; i < m_listeners.size(); ++i)
        if (m_listeners[i] == pListener) { m_listeners.erase(m_listeners.begin() + (long)i); return; }
}

void CSDL2Sound::NotifyEvent(ESoundCallbackEvent event)
{
    for (size_t i = 0; i < m_listeners.size(); ++i)
        m_listeners[i]->OnSoundEvent(event, this);
}

int CSDL2Sound::Release()
{
    int nRefs = --m_nRefs;
    if (nRefs <= 0) { delete this; return 0; }
    return nRefs;
}

bool CSDL2Sound::EnsureLoaded()
{
    if (m_bLoaded) return true;
    if (m_bLoadFailed) return false;
    if (!m_pOwner || !m_pOwner->GetSystem()) { m_bLoadFailed = true; return false; }

    const std::string sampleKey = m_szName + (m_nChannels == 1 ? "#mono" : "#stereo");
    std::shared_ptr<std::vector<short> > cached = m_pOwner->FindSamples(sampleKey);
    if (cached) {
        m_pcm = cached;
        m_nFreq = m_pOwner->GetDeviceFreq();
        m_bLoaded = true;
        NotifyEvent(SOUND_EVENT_ON_LOADED);
        return true;
    }
    m_bLoading = true;
    const Uint32 nLoadStart = SDL_GetTicks();
    const bool bDebugLoad = PortSndEnvFlag("FARCRY_SDL_AUDIO_DEBUG", false);
    std::vector<unsigned char> data;
    if (!SndReadFile(m_pOwner->GetSystem(), m_szName.c_str(), data))
    {
        m_bLoading = false;
        m_bLoadFailed = true;
        m_pOwner->Log("sound not found in paks: %s", m_szName.c_str());
        NotifyEvent(SOUND_EVENT_ON_LOAD_FAILED);
        return false;
    }

    bool bDecoded = SndDecodeWav(&data[0], data.size(), m_pOwner->GetDeviceFreq(), (*m_pcm), m_nChannels);
    if (!bDecoded)
        bDecoded = SndDecodeOgg(&data[0], data.size(), m_pOwner->GetDeviceFreq(), (*m_pcm), m_nChannels);
    if (!bDecoded)
    {
        m_bLoading = false;
        m_bLoadFailed = true;
        if (!m_pOwner->IsAudioReady() || !PortSndEnvFlag("FARCRY_SDL_AUDIO_QUIET", false))
            m_pOwner->Log("unsupported or corrupt audio (PCM/ADPCM WAV and Ogg Vorbis supported): %s", m_szName.c_str());
        if (bDebugLoad)
            m_pOwner->Log("load '%s': %u bytes, decode FAILED in %u ms",
                m_szName.c_str(), (unsigned)data.size(), (unsigned)(SDL_GetTicks() - nLoadStart));
        NotifyEvent(SOUND_EVENT_ON_LOAD_FAILED);
        return false;
    }

    m_pOwner->CacheSamples(sampleKey, m_pcm);
    m_nFreq = m_pOwner->GetDeviceFreq();
    m_bLoaded = true;
    m_bLoading = false;
    if (bDebugLoad)
        m_pOwner->Log("load '%s': %u bytes -> %d ms of audio in %u ms",
            m_szName.c_str(), (unsigned)data.size(), GetLengthMs(),
            (unsigned)(SDL_GetTicks() - nLoadStart));
    NotifyEvent(SOUND_EVENT_ON_LOADED);
    return true;
}

bool CSDL2Sound::Preload()
{
    return EnsureLoaded();
}

void CSDL2Sound::Play(float fVolumeScale, bool, bool)
{
    if (!m_bLoaded && !m_bLoadFailed && !m_bStream && m_pOwner)
    {
        // Not decoded yet: already cached by another sound object, else start
        // it when the background decoder delivers (see OnDecoded).
        std::shared_ptr<std::vector<short> > cached = m_pOwner->FindSamples(SampleKey());
        if (cached) EnsureLoaded();
        else if (m_pOwner->QueueDecode(this))
        {
            m_bPlayPending = true;
            m_fPendingVolume = fVolumeScale;
            m_nPendingAt = SDL_GetTicks();
            return;
        }
    }
    if (!EnsureLoaded()) return;
    StartPlay(fVolumeScale);
}

void CSDL2Sound::StartPlay(float fVolumeScale)
{
    m_fPlayVolume = (fVolumeScale > 0.0f) ? fVolumeScale : 0.0f;
    m_fPos = 0.0f;
    m_bFinished = false;
    m_bPlaying = true;
    if (m_pOwner && PortSndEnvFlag("FARCRY_SDL_AUDIO_DEBUG", false))
        m_pOwner->Log("play '%s': stream=%d length=%d ms", m_szName.c_str(), (m_nFlags & FLAG_SOUND_STREAM) ? 1 : 0, GetLengthMs());
    if (m_pOwner && m_pOwner->IsAudioReady())
    {
        SDL_LockAudioDevice(m_pOwner->GetDevice());
        m_pOwner->AddVoice(this);
        SDL_UnlockAudioDevice(m_pOwner->GetDevice());
    }
    NotifyEvent(SOUND_EVENT_ON_PLAY);
}

void CSDL2Sound::Stop()
{
    m_bPlayPending = false;
    if (!m_bPlaying) return;
    m_bPlaying = false;
    m_bFinished = true;
    if (m_pOwner && m_pOwner->IsAudioReady())
    {
        SDL_LockAudioDevice(m_pOwner->GetDevice());
        m_pOwner->RemoveVoice(this);
        SDL_UnlockAudioDevice(m_pOwner->GetDevice());
    }
    NotifyEvent(SOUND_EVENT_ON_STOP);
}

void CSDL2Sound::SetRatio(float fRatio)
{
    m_fRatio = (fRatio > 0.01f) ? fRatio : 0.01f;
}

void CSDL2Sound::SetPitch(int nPitch)
{
    m_fPitch = (nPitch > 0) ? ((float)nPitch / 1000.0f) : 1.0f;
}

void CSDL2Sound::SetPan(int nPan)
{
    if (nPan < -100) nPan = -100;
    if (nPan > 100) nPan = 100;
    m_fPan = (float)nPan / 100.0f;
}

void CSDL2Sound::SetMinMaxDistance(float fMinDist, float fMaxDist)
{
    m_fMinDist = fMinDist;
    m_fMaxDist = fMaxDist;
}

void CSDL2Sound::AddToScaleGroup(int nGroup)
{
    if (nGroup >= 0 && nGroup < 32) m_nScaleGroup |= (1u << nGroup);
}

void CSDL2Sound::RemoveFromScaleGroup(int nGroup)
{
    if (nGroup >= 0 && nGroup < 32) m_nScaleGroup &= ~(1u << nGroup);
}

void CSDL2Sound::SetVolume(int nVolume)
{
    if (nVolume < 0) nVolume = 0;
    if (nVolume > 255) nVolume = 255;
    m_nVolume = nVolume;
}

int CSDL2Sound::GetLengthMs()
{
    if (!(*m_pcm).empty() && m_nFreq > 0)
        return (int)((size_t)((*m_pcm).size() / m_nChannels) * 1000 / (size_t)m_nFreq);
    return 0;
}

int CSDL2Sound::GetLength()
{
    return (int)((*m_pcm).size() * sizeof(short));
}

unsigned int CSDL2Sound::GetCurrentSamplePos(bool bMilliSeconds)
{
    if (bMilliSeconds)
        return (unsigned int)((double)m_fPos * 1000.0 / (double)(m_nFreq > 0 ? m_nFreq : 1));
    return (unsigned int)(m_fPos * 4.0f); // 16-bit stereo frame in bytes
}

void CSDL2Sound::SetCurrentSamplePos(unsigned int nPos, bool bMilliSeconds)
{
    double pos = bMilliSeconds
        ? ((double)nPos / 1000.0 * (double)(m_nFreq > 0 ? m_nFreq : 1))
        : ((double)nPos / 4.0);
    if (pos < 0.0) pos = 0.0;
    m_fPos = (float)pos;
}

void CSDL2Sound::Mix(int* pOut, int nFrames, const Vec3& vListenerPos, float fMaster, float fMusic)
{
    if (!m_bPlaying || (*m_pcm).empty()) return;

    const int nTotalFrames = (int)((*m_pcm).size() / m_nChannels);
    if (nTotalFrames <= 1) return;

    // per-frame volume (constant during this callback)
    float fVol = (float)m_nVolume / 255.0f * m_fPlayVolume * m_fFading;
    if (m_bMusic) fVol *= fMusic;
    if (m_pOwner) fVol *= m_pOwner->GetGroupScale(m_nScaleGroup);
    fVol *= fMaster;

    if (!m_bRelative && m_pOwner)
    {
        float fDist = SndDistance(m_vPos, vListenerPos);
        if (fDist > m_fMaxDist) fVol = 0.0f;
        else if (fDist > m_fMinDist && m_fMaxDist > m_fMinDist)
            fVol *= 1.0f - (fDist - m_fMinDist) / (m_fMaxDist - m_fMinDist);
    }

    if (fVol <= 0.0001f)
    {
        // still advance playback so looping/ambient sounds stay in sync
        m_fPos += (float)nFrames * (m_fRatio * m_fPitch);
        if (m_fPos >= (float)nTotalFrames)
        {
            if (m_bLoop) m_fPos = fmodf(m_fPos, (float)nTotalFrames);
            else { m_bPlaying = false; m_bFinished = true; }
        }
        return;
    }

    const float fGainL = fVol * (m_fPan > 0.0f ? (1.0f - m_fPan) : 1.0f);
    const float fGainR = fVol * (m_fPan < 0.0f ? (1.0f + m_fPan) : 1.0f);

    const double fStep = (double)m_fRatio * (double)m_fPitch;
    double fPos = (double)m_fPos;

    for (int i = 0; i < nFrames; ++i)
    {
        if (fPos >= (double)nTotalFrames)
        {
            if (m_bLoop) fPos = fmod(fPos, (double)nTotalFrames);
            else { m_bPlaying = false; m_bFinished = true; break; }
        }
        int i0 = (int)fPos;
        if (i0 < 0) i0 = 0;
        if (i0 >= nTotalFrames) i0 = nTotalFrames - 1;
        int i1 = (i0 + 1 < nTotalFrames) ? i0 + 1 : i0;
        float frac = (float)(fPos - (double)i0);

        float s0L = (float)(*m_pcm)[(size_t)i0 * m_nChannels];
        float s1L = (float)(*m_pcm)[(size_t)i1 * m_nChannels];
        float s0R = (float)(*m_pcm)[(size_t)i0 * m_nChannels + (m_nChannels > 1 ? 1 : 0)];
        float s1R = (float)(*m_pcm)[(size_t)i1 * m_nChannels + (m_nChannels > 1 ? 1 : 0)];

        pOut[i * 2 + 0] += (int)((s0L + (s1L - s0L) * frac) * fGainL);
        pOut[i * 2 + 1] += (int)((s0R + (s1R - s0R) * frac) * fGainR);
        fPos += fStep;
    }
    m_fPos = (float)fPos;
}

// ---------------------------------------------------------------------------
// IMusicSystem (interface-only; the game's menu music is played through
// ISound/FLAG_SOUND_STREAM, but scripts also use this object for theme/mood
// handling and pausing).
// ---------------------------------------------------------------------------
class CEmptyStringIt : public IStringItVec
{
public:
    CEmptyStringIt() : m_nRefs(1) {}
    bool IsEnd() { return true; }
    const char* Next() { return NULL; }
    void MoveFirst() {}
    void AddRef() { ++m_nRefs; }
    void Release() { if (--m_nRefs <= 0) delete this; }
private:
    int m_nRefs;
};

class CSDL2MusicSystem : public IMusicSystem
{
public:
    CSDL2MusicSystem(ISystem* pSystem)
        : m_pSystem(pSystem), m_pSink(NULL), m_pData(NULL), m_bPaused(false), m_bEnabled(true)
    {
        memset(&m_status, 0, sizeof(m_status));
        m_status.bPlaying = false;
    }
    ~CSDL2MusicSystem()
    {
        if (m_pData && m_pSink) m_pSink->ReleaseData(m_pData);
    }

    void Release() { delete this; }
    ISystem* GetSystem() { return m_pSystem; }
    int GetBytesPerSample() { return 2; }
    IMusicSystemSink* SetSink(IMusicSystemSink* pSink) { IMusicSystemSink* pOld = m_pSink; m_pSink = pSink; return pOld; }
    bool SetData(SMusicData* pData, bool) { m_pData = pData; return true; }
    void Unload()
    {
        if (m_pData && m_pSink) m_pSink->ReleaseData(m_pData);
        m_pData = NULL;
        m_status.bPlaying = false;
    }
    void Pause(bool bPause) { m_bPaused = bPause; }
    void EnableEventProcessing(bool bEnable) { m_bEnabled = bEnable; }
    bool ResetThemeOverride() { m_sTheme = ""; return true; }
    bool SetTheme(const char* pszTheme, bool) { m_sTheme = pszTheme ? pszTheme : ""; m_status.sTheme = m_sTheme.c_str(); return true; }
    const char* GetTheme() { return m_sTheme.c_str(); }
    bool SetMood(const char* pszMood) { m_sMood = pszMood ? pszMood : ""; m_status.sMood = m_sMood.c_str(); return true; }
    bool SetDefaultMood(const char* pszMood) { m_sDefaultMood = pszMood ? pszMood : ""; return true; }
    const char* GetMood() { return m_sMood.c_str(); }
    IStringItVec* GetThemes() { return new CEmptyStringIt; }
    IStringItVec* GetMoods(const char*) { return new CEmptyStringIt; }
    bool AddMusicMoodEvent(const char*, float) { return true; }
    void Update() {}
    SMusicSystemStatus* GetStatus() { return &m_status; }
    void GetMemoryUsage(ICrySizer*) {}
    bool LoadMusicDataFromLUA(IScriptSystem*, const char*) { return false; }
    bool StreamOGG() { return false; }
    void LogMsg(const char* pszFormat, ...)
    {
        char szBuffer[1024];
        va_list args;
        va_start(args, pszFormat);
        vsnprintf(szBuffer, sizeof(szBuffer), pszFormat, args);
        va_end(args);
        if (m_pSystem) if (ILog* pLog = m_pSystem->GetILog()) pLog->Log("port/audio/music: %s", szBuffer);
    }
    bool LoadFromXML(const char*, bool) { return false; }
    void UpdateTheme(SMusicTheme*) {}
    void UpdateMood(SMusicMood*) {}
    void UpdatePattern(SPatternDef*) {}
    void RenamePattern(const char*, const char*) {}
    void PlayPattern(const char*, bool) {}
    void DeletePattern(const char*) {}
    void Silence() { m_status.bPlaying = false; }
private:
    ISystem* m_pSystem;
    IMusicSystemSink* m_pSink;
    SMusicData* m_pData;
    bool m_bPaused;
    bool m_bEnabled;
    SMusicSystemStatus m_status;
    std::string m_sTheme, m_sMood, m_sDefaultMood;
};

IMusicSystem* CSDL2SoundSystem::CreateMusicSystem()
{
    return new CSDL2MusicSystem(m_pSystem);
}

// ---------------------------------------------------------------------------
// factory
// ---------------------------------------------------------------------------
extern "C" ISoundSystem* CreateSoundSystem(ISystem* pSystem, void*)
{
    return new CSDL2SoundSystem(pSystem);
}
