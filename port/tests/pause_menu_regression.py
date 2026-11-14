#!/usr/bin/env python3
"""Check production screen focus transitions and modern controller event routing."""
from pathlib import Path
import argparse, subprocess, tempfile
src=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--ui',type=Path,default=src/'CryGame/UISystem.cpp')
p.add_argument('--input',type=Path,default=src/'CryInput/Input.cpp')
a=p.parse_args()
def method(path,sig):
 s=path.read_text(encoding='latin1');b=s.index(sig);e=s.index('{',b)+1;depth=1
 while depth:
  depth+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[b:e]
methods='\n'.join([method(a.ui,'int CUISystem::ActivateScreen(CUIScreen *'),method(a.ui,'int CUISystem::DeactivateScreen(CUIScreen *'),method(a.ui,'bool CUISystem::IsOnFocusScreen('),method(a.input,'void CInput::PostInputEvent(')])
# Exercise the actual menu routing block independently of gameplay bindings.
input_text=a.input.read_text()
begin=input_text.index('// Menu/pause navigation:')
begin=input_text.index('{',input_text.index('if (bMenu)',begin))
end=begin+1;depth=1
while depth:
 depth+=(input_text[end]=='{')-(input_text[end]=='}');end+=1
methods += '\nvoid CInput::UpdateMenu() '+input_text[begin:end]
code=r'''
#include <vector>
#include <algorithm>
#include <functional>
#include <cmath>
#include <string>
#include <cstdlib>
#include <cassert>
#include <cstdio>
struct CUIScreen;
struct CUIWidget {
 CUIScreen* m_pScreen;
 void OnMouseLeave(){} void OnLostFocus(){}
};
using CUIWidgetItor=std::vector<CUIWidget*>::iterator;
struct CUIScreen {
 bool m_bActive=false;std::vector<CUIWidget*> m_vWidgetList;
 std::function<void()> activate,deactivate;
 void OnActivate(){if(activate)activate();}
 void OnDeactivate(){if(deactivate)deactivate();}
};
using CUIScreenItor=std::vector<CUIScreen*>::iterator;
struct CUISystem {
 std::vector<CUIScreen*> m_vScreenList;
 CUIWidget* m_pFocus=nullptr;CUIWidget* m_pMouseCaptured=nullptr;CUIWidget* m_pMouseOver=nullptr;
 CUIScreen* m_pFocusScreen=nullptr;bool repeat=false;
 int ActivateScreen(CUIScreen*);int DeactivateScreen(CUIScreen*);bool IsOnFocusScreen(CUIWidget*);
 bool IsScreenActive(CUIScreen* s){return s->m_bActive;}
 int SetFocus(CUIWidget* w){m_pFocus=w;if(w)m_pFocusScreen=w->m_pScreen;return 1;}
 int SetFocusScreen(CUIScreen* s){m_pFocusScreen=s;return 1;}
 int FirstTabStop(){for(auto s:m_vScreenList)for(auto w:s->m_vWidgetList)if(IsOnFocusScreen(w)){SetFocus(w);return 1;}return 0;}
 int ResetKeyRepeat(){repeat=false;return 1;}
};
const int XKEY_SPACE=1,XKEY_RETURN=2,XKEY_ESCAPE=3,XKEY_TAB=4,XKEY_LSHIFT=5,XKEY_MOD_LSHIFT=1;
enum {XKEY_GP_A=10,XKEY_GP_B,XKEY_GP_Y,XKEY_GP_START,XKEY_GP_BACK,XKEY_GP_DPAD_UP,XKEY_GP_DPAD_DOWN,XKEY_GP_DPAD_LEFT,XKEY_GP_DPAD_RIGHT};
static bool g_PortFocusLoadTab=false;
static float PortCfgFloat(const char*,float value){return value;}
struct Pad {
 bool pressed[32]={},released[32]={};float lx=0,ly=0,rx=0,ry=0;
 bool KeyPressed(int k){return pressed[k];}bool KeyReleased(int k){return released[k];}
 float GetStickLX(){return lx;}float GetStickLY(){return ly;}float GetStickRX(){return rx;}float GetStickRY(){return ry;}
 void clear(){for(int i=0;i<32;++i)pressed[i]=released[i]=false;lx=ly=rx=ry=0;}
};
struct Mouse {
 float x=400,y=300;int downs=0,ups=0;
 float GetVScreenX(){return x;}float GetVScreenY(){return y;}
 void SetVScreenX(float v){x=v;}void SetVScreenY(float v){y=v;}
 void OnButton(int,bool down){if(down)++downs;else ++ups;}
};
struct Keyboard{int GetModifiers(){return 0;}void SetSyntheticKeyState(int,bool){}};
struct Timer{float GetFrameTime(){return 1.f/30;}};
struct System{Timer timer;Timer* GetITimer(){return &timer;}};
struct SInputEvent{int key;};
struct Console {bool open=false;bool IsOpened(){return open;}};
static bool modern=true;
static bool PadLayoutGptk(){return modern;}
struct CInput {
 bool m_postingenable=true,menu=true;
 Pad m_Gamepad;Mouse m_Mouse;Keyboard m_Keyboard;
 System system;System* m_pSystem=&system;
 std::vector<int> synthetic;
 void PostSyntheticKey(int k,bool,int){synthetic.push_back(k);}
 void UpdateMenu();
 Console console;Console* m_console=&console;
 int delivered=0;std::function<void(const SInputEvent&)> onEvent;
 bool IsMenuOrConsoleActive(){return menu||console.open;}
 void BroadcastEvent(const SInputEvent& e){++delivered;if(onEvent)onEvent(e);}
 void PostInputEvent(const SInputEvent&);
};
'''+methods+r'''
int main(){
 CUISystem ui;CUIScreen pause,options,controls,main;
 CUIWidget checkpoint{&pause},option{&options},control{&controls},mainButton{&main};
 pause.m_vWidgetList={&checkpoint};options.m_vWidgetList={&option};
 controls.m_vWidgetList={&control};main.m_vWidgetList={&mainButton};
 ui.m_vScreenList={&pause,&options,&controls,&main};
 ui.ActivateScreen(&pause);ui.SetFocus(&checkpoint);
 ui.m_pMouseCaptured=ui.m_pMouseOver=&checkpoint;ui.repeat=true;
 // Switching away clears stale keyboard and mouse ownership.
 ui.DeactivateScreen(&pause);assert(ui.m_pFocus!=&checkpoint);
 assert(ui.m_pFocusScreen!=&pause);assert(!ui.m_pMouseCaptured&&!ui.m_pMouseOver&&!ui.repeat);
 assert(!ui.IsOnFocusScreen(&checkpoint));assert(!ui.IsOnFocusScreen(nullptr));
 // An options activation can activate its child controls page; parent return
 // must preserve the child-selected focus.
 options.activate=[&]{ui.ActivateScreen(&controls);};
 ui.ActivateScreen(&options);assert(ui.m_pFocus==&control);assert(ui.m_pFocusScreen==&controls);
 assert(ui.IsOnFocusScreen(&control));assert(!ui.IsOnFocusScreen(&checkpoint));
 // Invalid/stale screen focus cannot route keys to an inactive checkpoint.
 ui.m_pFocusScreen=&pause;assert(!ui.IsOnFocusScreen(&checkpoint));
 ui.m_pFocusScreen=&controls;
 CInput input;int loads=0;
 input.onEvent=[&](const SInputEvent& e){if(e.key==XKEY_SPACE&&ui.IsOnFocusScreen(&checkpoint))++loads;};
 input.PostInputEvent({XKEY_SPACE});assert(input.delivered==0&&loads==0);
 input.PostInputEvent({XKEY_RETURN});assert(input.delivered==1);
 input.PostInputEvent({XKEY_ESCAPE});assert(input.delivered==2);
 // Reopening pause with a valid selected checkpoint still must not let the
 // modern layout's duplicate gameplay Space activate it.
 ui.DeactivateScreen(&controls);ui.DeactivateScreen(&options);ui.ActivateScreen(&pause);
 input.PostInputEvent({XKEY_SPACE});assert(input.delivered==2&&loads==0);
 // Gameplay jump, console typing and classic keyboard menus keep Space.
 input.menu=false;input.PostInputEvent({XKEY_SPACE});assert(input.delivered==3);
 input.console.open=true;input.PostInputEvent({XKEY_SPACE});assert(input.delivered==4);
 input.console.open=false;input.menu=true;modern=false;
 input.PostInputEvent({XKEY_SPACE});assert(input.delivered==5);
 input.m_postingenable=false;input.PostInputEvent({XKEY_RETURN});assert(input.delivered==5);
 // Modern always uses mouse clicks, even before stick motion or after D-pad.
 modern=true;CInput routed;routed.m_Gamepad.pressed[XKEY_GP_DPAD_DOWN]=true;
 routed.m_Gamepad.pressed[XKEY_GP_Y]=true;routed.UpdateMenu();
 assert(routed.synthetic.empty()&&!g_PortFocusLoadTab);
 routed.m_Gamepad.clear();routed.m_Gamepad.pressed[XKEY_GP_A]=true;routed.UpdateMenu();
 assert(routed.m_Mouse.downs==1&&routed.synthetic.empty());
 routed.m_Gamepad.clear();routed.m_Gamepad.released[XKEY_GP_A]=true;routed.UpdateMenu();
 assert(routed.m_Mouse.ups==1);
 routed.m_Gamepad.clear();routed.m_Gamepad.lx=1;routed.UpdateMenu();assert(routed.m_Mouse.x>400);
 routed.m_Gamepad.clear();float x=routed.m_Mouse.x;routed.m_Gamepad.rx=1;routed.UpdateMenu();assert(routed.m_Mouse.x>x);
 // Classic retains intentional D-pad focus navigation and Return activation.
 modern=false;routed.m_Gamepad.clear();routed.m_Gamepad.pressed[XKEY_GP_DPAD_DOWN]=true;
 routed.UpdateMenu();assert(routed.synthetic.size()==2&&routed.synthetic[0]==XKEY_TAB);
 routed.synthetic.clear();routed.m_Gamepad.clear();routed.m_Gamepad.pressed[XKEY_GP_A]=true;
 routed.UpdateMenu();assert(routed.synthetic.size()==2&&routed.synthetic[0]==XKEY_RETURN);
 std::puts("PASS: pause/options nested focus, inactive checkpoint rejection, mouse ownership cleanup and modern duplicate Space suppression");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 r=Path(tmp);(r/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-DLINUX',str(r/'test.cpp'),'-o',str(r/'test')],check=True)
 subprocess.run([str(r/'test')],check=True,cwd=r)
