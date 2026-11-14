#!/usr/bin/env python3
"""Exercise the production mixer and playback methods with helicopter-style fades.
--backend permits checking an earlier SDL2Sound.cpp against the same assertions.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--backend', type=Path, default=Path(__file__).resolve().parents[1]/'platform/SDL2Sound.cpp')
args = parser.parse_args()
source = args.backend.read_text()
def method(signature):
    start = source.index(signature)
    end = source.index('{', start)+1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
methods = '\n'.join(method('void CSDL2Sound::'+name+'(') for name in
                    ['Play','StartPlay','OnDecoded','SetRatio','SetPitch','Mix'])
fixture = r'''
#include <vector>
#include <memory>
#include <string>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <algorithm>
using std::size_t;
using Uint32=unsigned;
static Uint32 SDL_GetTicks(){return 1000;}
static void SDL_LockAudioDevice(int){}
static void SDL_UnlockAudioDevice(int){}
static bool PortSndEnvFlag(const char*,bool){return false;}
const int FLAG_SOUND_STREAM=1;
enum {SOUND_EVENT_ON_PLAY,SOUND_EVENT_ON_LOADED,SOUND_EVENT_ON_LOAD_FAILED};
struct Vec3 {};
static float SndDistance(Vec3,Vec3){return 0;}
struct CSDL2Sound;
struct CSDL2SoundSystem {
 bool IsAudioReady(){return true;}
 int GetDevice(){return 1;}
 int GetDeviceFreq(){return 44100;}
 float GetGroupScale(unsigned){return 1;}
 void AddVoice(CSDL2Sound*){}
 void RetainOneShot(CSDL2Sound*){}
 void Log(const char*, ...){}
 std::shared_ptr<std::vector<short>> FindSamples(const std::string&){return {};}
 bool QueueDecode(CSDL2Sound*){return true;}
 void CacheSamples(const std::string&,const std::shared_ptr<std::vector<short>>& ){}
};
struct CSDL2Sound {
 CSDL2SoundSystem* m_pOwner=nullptr;
 std::shared_ptr<std::vector<short>> m_pcm=std::make_shared<std::vector<short>>();
 bool m_bPlaying=false,m_bPlayPending=false,m_bLoaded=false,m_bLoadFailed=false;
 bool m_bStream=false,m_bLoop=true,m_bFinished=false,m_bQueued=false,m_bLoading=true;
 bool m_bMusic=false,m_bRelative=true;
 Uint32 m_nLastPlayTicks=0,m_nPendingAt=0;
 float m_fPendingVolume=1,m_fPlayVolume=1,m_fRatio=1,m_fPitch=1,m_fFading=1;
 float m_fPos=0,m_fPan=0,m_fMinDist=1,m_fMaxDist=100;
 int m_nVolume=255,m_nChannels=2,m_nFreq=44100,m_nFlags=0;
 unsigned m_nScaleGroup=0;
 Vec3 m_vPos;
 std::string m_szName="rotor.wav";
 std::string SampleKey(){return m_szName;}
 void NotifyEvent(int){}
 int GetLengthMs(){return 1000;}
 bool EnsureLoaded(){return m_bLoaded;}
 void Play(float,bool=true,bool=true);
 START_DECL
 void OnDecoded(const std::shared_ptr<std::vector<short>>&,int,bool);
 void SetRatio(float);
 void SetPitch(int);
 void Mix(int*,int,const Vec3&,float,float);
};
'''
fixture = fixture.replace('START_DECL', 'void StartPlay();' if 'void CSDL2Sound::StartPlay()' in methods else 'void StartPlay(float);')
main = r'''
static void mix(CSDL2Sound& s,int expected,float advance) {
 int out[8]={};float before=s.m_fPos;
 s.Mix(out,4,Vec3{},1,1);
 for(int x:out)assert(x==expected);
 assert(std::fabs(s.m_fPos-before-advance)<.001f);
}
int main(){
 auto pcm=std::make_shared<std::vector<short>>(2000,4000);
 CSDL2SoundSystem owner;
 // HeliCommon.StartRotor: Play(0), then SetSoundRatio ramp. Gain changes,
 // playback speed does not. Fade all the way to zero while advancing time.
 CSDL2Sound rotor;rotor.m_pOwner=&owner;rotor.m_bLoaded=true;rotor.m_pcm=pcm;
 rotor.Play(0);assert(rotor.m_bPlaying);mix(rotor,0,4);
 rotor.SetRatio(.5f);mix(rotor,2000,4);
 rotor.SetRatio(1);mix(rotor,4000,4);
 rotor.SetRatio(0);mix(rotor,0,4);
 // Repeated loop requests preserve cursor; bSetRatio=false preserves gain.
 float before=rotor.m_fPos;rotor.Play(.25f);assert(rotor.m_fPos==before);
 mix(rotor,1000,4);rotor.Play(1,true,false);mix(rotor,1000,4);
 // Pitch remains a separate control from gain.
 rotor.SetPitch(2000);mix(rotor,1000,8);
 rotor.m_fFading=.5f;mix(rotor,500,8);
 // Decoder completion must preserve a fade applied while loading, including
 // a final mute, rather than restoring Play's stale initial volume.
 CSDL2Sound delayed;delayed.m_pOwner=&owner;
 delayed.Play(0);assert(delayed.m_bPlayPending);
 delayed.SetRatio(.5f);delayed.OnDecoded(pcm,2,true);
 assert(delayed.m_bPlaying && !delayed.m_bPlayPending);mix(delayed,2000,4);
 CSDL2Sound muted;muted.m_pOwner=&owner;
 muted.Play(1);muted.SetRatio(0);muted.OnDecoded(pcm,2,true);mix(muted,0,4);
 std::puts("PASS: helicopter fade-in/out, unchanged pitch, repeated Play, delayed decoding and explicit pitch");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    (root/'test.cpp').write_text(fixture+methods+main)
    subprocess.run(['c++','-std=c++11','-O0','-g',str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True,cwd=root)
