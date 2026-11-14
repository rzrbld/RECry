#!/usr/bin/env python3
"""Compile production sound lifecycle methods against controlled event fixtures.
No game assets or audio device required. --prove-previous-bug also checks that
reverting each cleanup independently makes its corresponding regression fail.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

src = Path(__file__).resolve().parents[2]

def method(path, signature):
    text = (src / path).read_text()
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

methods = [
    method('CryGame/XPlayer.cpp', 'CWeaponClass *CPlayer::DeselectWeapon()'),
    method('port/platform/SDL2Sound.cpp', 'void CSDL2SoundSystem::Silence()'),
    method('port/platform/SDL2Sound.cpp', 'void CSDL2Sound::Stop()'),
    method('port/platform/SDL2Sound.cpp', 'void CSDL2Sound::OnDecoded('),
    method('CryEntitySystem/Entity.cpp', 'void CEntity::PlaySound('),
]
fixture = r'''
#include <cassert>
#include <vector>
#include <map>
#include <list>
#include <memory>
#include <string>
#include <functional>
#include <cstdio>
using std::size_t;
static std::vector<int> events;
const int eNotFiring=0;
struct Entity { void DetachObjectToBone(const char*) { events.push_back(2); } };
struct Game { bool multi=false; bool IsMultiplayer(){return multi;} };
struct CWeaponClass {
 std::string bone="weapon";
 void ScriptOnStopFiring(Entity*) { events.push_back(1); }
 void ScriptOnDeactivate(Entity*) { events.push_back(3); }
 const std::string& GetBindBone(){return bone;}
};
struct CPlayer {
 int m_nSelectedWeaponID=1;
 struct { bool firing=false; int LastFiringType=eNotFiring; } m_stats;
 Entity entity; Entity* m_pEntity=&entity;
 Game game; Game* m_pGame=&game;
 CWeaponClass weapon;
 bool local=false;
 bool IsMyPlayer(){return local;}
 CWeaponClass* GetSelectedWeapon(){return &weapon;}
 void SetWeapon(int id){m_nSelectedWeaponID=id;events.push_back(4);}
 CWeaponClass* DeselectWeapon();
};
static unsigned SDL_GetTicks(){return 1000;}
static void SDL_LockAudioDevice(int){}
static void SDL_UnlockAudioDevice(int){}
enum { SOUND_EVENT_ON_STOP, SOUND_EVENT_ON_LOAD_FAILED, SOUND_EVENT_ON_LOADED };
struct CSDL2Sound;
struct CSDL2SoundSystem {
 std::map<int,CSDL2Sound*> m_mapSounds;
 std::vector<CSDL2Sound*> m_vecVoices;
 bool IsAudioReady(){return true;}
 int GetDevice(){return 1;}
 int GetDeviceFreq(){return 48000;}
 void CacheSamples(const std::string&,const std::shared_ptr<std::vector<short>>& ){}
 void Log(const char*, ...){}
 void RemoveVoice(CSDL2Sound*);
 void Silence();
};
struct CSDL2Sound {
 bool m_bPlayPending=false,m_bPlaying=false,m_bFinished=false;
 bool m_bQueued=true,m_bLoaded=false,m_bLoadFailed=false,m_bLoading=true,m_bLoop=true;
 int m_nChannels=0,m_nFreq=0; unsigned m_nPendingAt=0;
 float m_fPendingVolume=1;
 int refs=1,starts=0,stops=0;
 CSDL2SoundSystem* m_pOwner=nullptr;
 std::string m_szName="loop.wav";
 std::shared_ptr<std::vector<short>> m_pcm;
 std::function<void()> onStop;
 void AddRef(){++refs;}
 void Release(){assert(refs>1);--refs;}
 void NotifyEvent(int e){if(e==SOUND_EVENT_ON_STOP){++stops;if(onStop)onStop();}}
 std::string SampleKey(){return m_szName;}
 void StartPlay(){++starts;m_bPlaying=true;}
 void Stop();
 void OnDecoded(const std::shared_ptr<std::vector<short>>&,int,bool);
};
void CSDL2SoundSystem::RemoveVoice(CSDL2Sound* s) {
 for(auto it=m_vecVoices.begin();it!=m_vecVoices.end();++it)
  if(*it==s){m_vecVoices.erase(it);return;}
}
struct Vec3d {};
struct ISound {
 bool playing=true;int stops=0;
 bool IsPlaying(){return playing;}
 void SetPosition(Vec3d){}
 void Play(float){playing=true;}
 void Stop(){++stops;playing=false;}
 const char* GetName(){return "attached-loop.wav";}
};
struct SoundPtr : std::shared_ptr<ISound> {
 SoundPtr(ISound* p):std::shared_ptr<ISound>(p,[](ISound*){}){}
 operator ISound*() const {return get();}
};
struct SAttachedSound {
 SoundPtr pSound;
 SAttachedSound(ISound* p,Vec3d):pSound(p){}
};
struct Logger {void Log(const char*, ...){}};
'''
fixture += r'''
struct System {Logger log;Logger* GetILog(){return &log;}};
struct CEntity {
 using SoundsList=std::list<SAttachedSound>;
 SoundsList m_lstAttachedSounds;
 bool m_bUpdateSounds=false;
 System system;System* m_pISystem=&system;
 std::string m_name="boat";
 Vec3d GetSoundPos(){return {};}
 void PlaySound(ISound*,float,Vec3d&);
};
'''
main = r'''
int main() {
 // AI death/weapon removal: stop both an active trigger and a released trigger
 // whose last firing state hasn't yet received its stop callback.
 for(int mode=0;mode<3;++mode){
  CPlayer p;p.local=(mode==2);p.m_stats.firing=(mode!=1);
  p.m_stats.LastFiringType=(mode==1?1:0);events.clear();
  assert(p.DeselectWeapon()==&p.weapon);
  std::vector<int> expected=mode==2?std::vector<int>{1,3,2,4}:std::vector<int>{1,2,4};
  assert(events==expected);assert(!p.m_stats.firing && p.m_stats.LastFiringType==eNotFiring);
 }
 {CPlayer p;events.clear();p.DeselectWeapon();assert((events==std::vector<int>{2,4}));}
 {CPlayer p;p.m_nSelectedWeaponID=-1;events.clear();assert(!p.DeselectWeapon());assert(events.empty());}
 // A level reset must cancel undecoded loops, including when Stop callbacks
 // remove another sound from the registry. Snapshot refs must cover callbacks.
 CSDL2SoundSystem sys;CSDL2Sound active,pending;
 active.m_pOwner=pending.m_pOwner=&sys;
 active.m_bPlaying=true;pending.m_bPlayPending=true;
 sys.m_mapSounds[1]=&active;sys.m_mapSounds[2]=&pending;sys.m_vecVoices.push_back(&active);
 active.onStop=[&]{assert(active.refs==2 && pending.refs==2);sys.m_mapSounds.erase(2);};
 sys.Silence();assert(!active.m_bPlaying && active.stops==1);
 assert(!pending.m_bPlayPending);assert(active.refs==1 && pending.refs==1);
 assert(sys.m_vecVoices.empty());
 auto pcm=std::make_shared<std::vector<short>>(16);
 pending.OnDecoded(pcm,2,true);assert(pending.m_bLoaded && pending.starts==0);
 // A live request still starts after decoding, cancellation must not disable it.
 CSDL2Sound live;live.m_bPlayPending=true;live.OnDecoded(pcm,2,true);assert(live.starts==1);
 // Overflow must stop every attached loop before forgetting ownership.
 CEntity e;ISound attached[15],incoming;Vec3d offset;
 for(auto& s:attached)e.m_lstAttachedSounds.emplace_back(&s,offset);
 e.PlaySound(&incoming,1,offset);assert(e.m_lstAttachedSounds.empty());
 for(auto& s:attached)assert(!s.playing && s.stops==1);
 std::puts("PASS: AI stop-before-detach, pending decode cancellation, callback lifetimes, entity overflow cleanup");
}
'''
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--prove-previous-bug', action='store_true')
args = parser.parse_args()
variants = [('fixed', methods)]
if args.prove_previous_bug:
    old = methods.copy()
    a = old[0].index('\t\t\t// AI shooters skip')
    b = old[0].index('\t\t\t//this script event', a)
    old[0] = old[0][:a] + old[0][b:]
    variants.append(('missing-AI-stop', old))
    old = methods.copy()
    old[1] = '''void CSDL2SoundSystem::Silence() {
      std::vector<CSDL2Sound*> voices;voices.swap(m_vecVoices);
      for(size_t i=0;i<voices.size();++i)voices[i]->Stop();
    }'''
    variants.append(('pending-decode-survives-reset', old))
    old = methods.copy()
    a = old[4].index('\t\t// Stop before dropping ownership')
    old[4] = old[4][:a] + '\t\tm_lstAttachedSounds.clear();\n\t}\n}'
    variants.append(('orphan-on-attachment-overflow', old))
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    for name, bodies in variants:
        (root/'test.cpp').write_text(fixture+'\n'+'\n'.join(bodies)+'\n'+main)
        subprocess.run(['c++','-std=c++11','-O0','-g',str(root/'test.cpp'),'-o',str(root/'test')],check=True)
        result = subprocess.run([str(root/'test')],capture_output=True,text=True,cwd=root)
        if name == 'fixed':
            print(result.stdout,end='')
            if result.returncode:
                raise RuntimeError(result.stderr)
        else:
            if result.returncode == 0:
                raise RuntimeError('Regression failed to detect '+name)
            print('PASS: detects '+name)
