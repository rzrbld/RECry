#!/usr/bin/env python3
"""Test production one-shot ownership and voice stealing with emitter lifetimes.
--backend accepts an older backend as a negative control. No assets/device needed.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--backend',type=Path,default=Path(__file__).resolve().parents[1]/'platform/SDL2Sound.cpp')
args=parser.parse_args()
source=args.backend.read_text()
def method(sig):
 start=source.index(sig);end=source.index('{',start)+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
methods=[]
for cls,names in [('CSDL2Sound',['Play','StartPlay','OnDecoded','Stop','Release']),
                  ('CSDL2SoundSystem',['Update','AddVoice','RemoveVoice','RetainOneShot','CollectOneShots'])]:
 for name in names:
  sig=('int ' if name=='Release' else 'void ')+cls+'::'+name+'('
  if sig in source:methods.append(method(sig))
  else:methods.append('void '+cls+'::'+name+'('+('CSDL2Sound*' if name=='RetainOneShot' else '')+') {}')
fixture=r'''
#include <vector>
#include <set>
#include <map>
#include <string>
#include <memory>
#include <functional>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "smartptr.h"
using std::size_t;
using Uint32=unsigned;
static int audioLocks=0;
static void SDL_LockAudioDevice(int){assert(audioLocks==0);++audioLocks;}
static void SDL_UnlockAudioDevice(int){assert(audioLocks==1);--audioLocks;}
static Uint32 ticks=1000;
static Uint32 SDL_GetTicks(){return ticks;}
static bool PortSndEnvFlag(const char*,bool){return false;}
const int FLAG_SOUND_STREAM=1,SDL2SND_MAX_VOICES=2;
enum {SOUND_EVENT_ON_PLAY,SOUND_EVENT_ON_LOADED,SOUND_EVENT_ON_LOAD_FAILED,SOUND_EVENT_ON_STOP};
static std::set<int> destroyed;
struct CSDL2Sound;
struct CSDL2SoundSystem {
 int m_nDevice=1;
 bool decode=false;
 std::map<int,CSDL2Sound*> m_mapSounds;
 std::vector<CSDL2Sound*> m_vecVoices;
 typedef std::set<_smart_ptr<CSDL2Sound>> OneShots;
 OneShots m_oneShots;
 bool IsAudioReady(){return m_nDevice!=0;}
 int GetDevice(){return m_nDevice;}
 int GetDeviceFreq(){return 44100;}
 void Log(const char*, ...){}
 void DrainDecoded(){}
 void GetSoundMemoryUsageInfo(size_t& a,size_t& b){a=b=0;}
 std::shared_ptr<std::vector<short>> FindSamples(const std::string&){return {};}
 bool QueueDecode(CSDL2Sound*){return decode;}
 void CacheSamples(const std::string&,const std::shared_ptr<std::vector<short>>& ){}
 void Update();void RetainOneShot(CSDL2Sound*);void CollectOneShots();
 void AddVoice(CSDL2Sound*);void RemoveVoice(CSDL2Sound*);
};
struct CSDL2Sound {
 CSDL2SoundSystem* m_pOwner;
 int id,m_nRefs=0;
 bool m_bPlaying=false,m_bPlayPending=false,m_bLoaded=true,m_bLoadFailed=false;
 bool m_bStream=false,m_bLoop=false,m_bFinished=false,m_bQueued=false,m_bLoading=false;
 Uint32 m_nLastPlayTicks=0,m_nPendingAt=0;
 float m_fRatio=1,m_fPos=0;
 int m_nChannels=2,m_nFreq=44100,m_nFlags=0;
 std::string m_szName="explosion.wav";
 std::shared_ptr<std::vector<short>> m_pcm;
 std::function<void()> onStop;
 CSDL2Sound(CSDL2SoundSystem& s,int i):m_pOwner(&s),id(i){s.m_mapSounds[i]=this;}
 ~CSDL2Sound(){assert(audioLocks==0);destroyed.insert(id);m_pOwner->RemoveVoice(this);m_pOwner->m_mapSounds.erase(id);}
 void AddRef(){++m_nRefs;}
 int Release();
 bool IsPlaying(){return m_bPlaying||m_bPlayPending;}
 bool IsLoop(){return m_bLoop;}
 int GetId(){return id;}
 const char* GetName(){return m_szName.c_str();}
 bool IsFinished(){return m_bFinished;}
 std::string SampleKey(){return m_szName;}
 void NotifyEvent(int e){assert(audioLocks==0);if(e==SOUND_EVENT_ON_STOP&&onStop)onStop();}
 int GetLengthMs(){return 1000;}
 bool EnsureLoaded(){return m_bLoaded;}
 void SetRatio(float x){m_fRatio=x;}
 void Play(float=1,bool=true,bool=true);void StartPlay();void Stop();
 void OnDecoded(const std::shared_ptr<std::vector<short>>&,int,bool);
};
'''
main=r'''
static CSDL2Sound* trigger(CSDL2SoundSystem& sys,int id,bool loop=false) {
 CSDL2Sound* s=new CSDL2Sound(sys,id);
 { _smart_ptr<CSDL2Sound> emitter=s;s->m_bLoop=loop;s->Play(); }
 return s;
}
static void finish(CSDL2Sound* s){s->m_bPlaying=false;s->m_bFinished=true;}
int main(){
 CSDL2SoundSystem sys;
 auto* blast=trigger(sys,1);
 assert(!destroyed.count(1)); // particle emitter can disappear before the blast tail
 assert(blast->IsPlaying());sys.Update();assert(!destroyed.count(1));
 finish(blast);sys.Update();assert(destroyed.count(1));assert(sys.m_oneShots.empty());
 // An emitter may expire even before its async audio decode finishes.
 sys.decode=true;
 CSDL2Sound* pending=new CSDL2Sound(sys,2);
 {_smart_ptr<CSDL2Sound> emitter=pending;pending->m_bLoaded=false;pending->Play();}
 assert(!destroyed.count(2));assert(pending->m_bPlayPending);
 pending->OnDecoded(std::make_shared<std::vector<short>>(16),2,true);
 assert(pending->IsPlaying());finish(pending);sys.Update();assert(destroyed.count(2));
 // Explicit stops and failed loads must not leave retained samples behind.
 auto* stopped=trigger(sys,3);stopped->Stop();sys.Update();assert(destroyed.count(3));
 CSDL2Sound* failed=new CSDL2Sound(sys,4);
 {_smart_ptr<CSDL2Sound> emitter=failed;failed->m_bLoaded=false;failed->Play();}
 assert(!destroyed.count(4));failed->OnDecoded({},2,false);sys.Update();assert(destroyed.count(4));
 // Loops keep their original owner/stop behavior; they must not acquire an
 // indefinite backend reference when the entity/script releases them.
 trigger(sys,5,true);assert(destroyed.count(5));assert(sys.m_vecVoices.empty());
 // Voice saturation: Stop must not relock the device while already locked,
 // and must not erase a second unrelated voice. Callbacks execute unlocked.
 sys.decode=false;
 auto* a=trigger(sys,6);auto* b=trigger(sys,7);int stopEvents=0;
 a->onStop=[&]{++stopEvents;assert(audioLocks==0);};
 auto* c=trigger(sys,8);
 assert(stopEvents==1);assert(sys.m_vecVoices.size()==2);
 assert(sys.m_vecVoices[0]==b&&sys.m_vecVoices[1]==c);
 sys.Update();assert(destroyed.count(6));
 finish(b);finish(c);sys.Update();assert(sys.m_oneShots.empty());assert(sys.m_mapSounds.empty());
 std::puts("PASS: one-shot tails survive emitter release/async decode, finish/stop/failure reclaim ownership, loops stay owner-bound, voice stealing is unlocked and removes one voice");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp);(root/'test.cpp').write_text(fixture+'\n'.join(methods)+main)
 common=Path(__file__).resolve().parents[2]/'CryCommon'
 subprocess.run(['c++','-std=c++11','-O0','-g','-I'+str(common),str(root/'test.cpp'),'-o',str(root/'test')],check=True)
 subprocess.run([str(root/'test')],check=True,cwd=root)
