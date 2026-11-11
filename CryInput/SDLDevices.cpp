#include "StdAfx.h"
#include "SDLDevices.h"
#include "Input.h"
#include <ILog.h>
#include <ISystem.h>
#include <IConsole.h>
#include <IProcess.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
extern ISystem* GetISystem();

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static float PortEnvFloat(const char* szName, float fDefault)
{
    const char* s = getenv(szName);
    if (!s || !*s) return fDefault;
    float v = (float)atof(s);
    return (v > 0.0f) ? v : fDefault;
}

static bool PortEnvFlag(const char* szName, bool bDefault)
{
    const char* s = getenv(szName);
    if (!s || !*s) return bDefault;
    return !(s[0] == '0' && s[1] == 0);
}

static void PortLog(const char* szFormat, ...)
{
    char szBuffer[1024];
    va_list args;
    va_start(args, szFormat);
    vsnprintf(szBuffer, sizeof(szBuffer), szFormat, args);
    va_end(args);
    fprintf(stderr, "[port/input] %s\n", szBuffer);
    if (ISystem* pSystem = GetISystem())
        if (ILog* pLog = pSystem->GetILog())
            pLog->Log("port/input: %s", szBuffer);
}

// ---------------------------------------------------------------------------
// keyboard
// ---------------------------------------------------------------------------
static int SDLScancodeToXKEY(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return XKEY_ESCAPE;
    case SDL_SCANCODE_1: return XKEY_1; case SDL_SCANCODE_2: return XKEY_2;
    case SDL_SCANCODE_3: return XKEY_3; case SDL_SCANCODE_4: return XKEY_4;
    case SDL_SCANCODE_5: return XKEY_5; case SDL_SCANCODE_6: return XKEY_6;
    case SDL_SCANCODE_7: return XKEY_7; case SDL_SCANCODE_8: return XKEY_8;
    case SDL_SCANCODE_9: return XKEY_9; case SDL_SCANCODE_0: return XKEY_0;
    case SDL_SCANCODE_MINUS: return XKEY_MINUS; case SDL_SCANCODE_EQUALS: return XKEY_EQUALS;
    case SDL_SCANCODE_BACKSPACE: return XKEY_BACKSPACE; case SDL_SCANCODE_TAB: return XKEY_TAB;
    case SDL_SCANCODE_Q: return XKEY_Q; case SDL_SCANCODE_W: return XKEY_W;
    case SDL_SCANCODE_E: return XKEY_E; case SDL_SCANCODE_R: return XKEY_R;
    case SDL_SCANCODE_T: return XKEY_T; case SDL_SCANCODE_Y: return XKEY_Y;
    case SDL_SCANCODE_U: return XKEY_U; case SDL_SCANCODE_I: return XKEY_I;
    case SDL_SCANCODE_O: return XKEY_O; case SDL_SCANCODE_P: return XKEY_P;
    case SDL_SCANCODE_LEFTBRACKET: return XKEY_LBRACKET; case SDL_SCANCODE_RIGHTBRACKET: return XKEY_RBRACKET;
    case SDL_SCANCODE_RETURN: return XKEY_RETURN; case SDL_SCANCODE_LCTRL: return XKEY_LCONTROL;
    case SDL_SCANCODE_A: return XKEY_A; case SDL_SCANCODE_S: return XKEY_S;
    case SDL_SCANCODE_D: return XKEY_D; case SDL_SCANCODE_F: return XKEY_F;
    case SDL_SCANCODE_G: return XKEY_G; case SDL_SCANCODE_H: return XKEY_H;
    case SDL_SCANCODE_J: return XKEY_J; case SDL_SCANCODE_K: return XKEY_K;
    case SDL_SCANCODE_L: return XKEY_L; case SDL_SCANCODE_SEMICOLON: return XKEY_SEMICOLON;
    case SDL_SCANCODE_APOSTROPHE: return XKEY_APOSTROPHE; case SDL_SCANCODE_GRAVE: return XKEY_TILDE;
    case SDL_SCANCODE_LSHIFT: return XKEY_LSHIFT; case SDL_SCANCODE_BACKSLASH: return XKEY_BACKSLASH;
    case SDL_SCANCODE_Z: return XKEY_Z; case SDL_SCANCODE_X: return XKEY_X;
    case SDL_SCANCODE_C: return XKEY_C; case SDL_SCANCODE_V: return XKEY_V;
    case SDL_SCANCODE_B: return XKEY_B; case SDL_SCANCODE_N: return XKEY_N;
    case SDL_SCANCODE_M: return XKEY_M; case SDL_SCANCODE_COMMA: return XKEY_COMMA;
    case SDL_SCANCODE_PERIOD: return XKEY_PERIOD; case SDL_SCANCODE_SLASH: return XKEY_SLASH;
    case SDL_SCANCODE_RSHIFT: return XKEY_RSHIFT; case SDL_SCANCODE_KP_MULTIPLY: return XKEY_MULTIPLY;
    case SDL_SCANCODE_LALT: return XKEY_LALT; case SDL_SCANCODE_SPACE: return XKEY_SPACE;
    case SDL_SCANCODE_CAPSLOCK: return XKEY_CAPSLOCK;
    case SDL_SCANCODE_F1: return XKEY_F1; case SDL_SCANCODE_F2: return XKEY_F2;
    case SDL_SCANCODE_F3: return XKEY_F3; case SDL_SCANCODE_F4: return XKEY_F4;
    case SDL_SCANCODE_F5: return XKEY_F5; case SDL_SCANCODE_F6: return XKEY_F6;
    case SDL_SCANCODE_F7: return XKEY_F7; case SDL_SCANCODE_F8: return XKEY_F8;
    case SDL_SCANCODE_F9: return XKEY_F9; case SDL_SCANCODE_F10: return XKEY_F10;
    case SDL_SCANCODE_F11: return XKEY_F11; case SDL_SCANCODE_F12: return XKEY_F12;
    case SDL_SCANCODE_NUMLOCKCLEAR: return XKEY_NUMLOCK; case SDL_SCANCODE_SCROLLLOCK: return XKEY_SCROLLLOCK;
    case SDL_SCANCODE_KP_7: return XKEY_NUMPAD7; case SDL_SCANCODE_KP_8: return XKEY_NUMPAD8;
    case SDL_SCANCODE_KP_9: return XKEY_NUMPAD9; case SDL_SCANCODE_KP_MINUS: return XKEY_SUBTRACT;
    case SDL_SCANCODE_KP_4: return XKEY_NUMPAD4; case SDL_SCANCODE_KP_5: return XKEY_NUMPAD5;
    case SDL_SCANCODE_KP_6: return XKEY_NUMPAD6; case SDL_SCANCODE_KP_PLUS: return XKEY_ADD;
    case SDL_SCANCODE_KP_1: return XKEY_NUMPAD1; case SDL_SCANCODE_KP_2: return XKEY_NUMPAD2;
    case SDL_SCANCODE_KP_3: return XKEY_NUMPAD3; case SDL_SCANCODE_KP_0: return XKEY_NUMPAD0;
    case SDL_SCANCODE_KP_PERIOD: return XKEY_DECIMAL;
    case SDL_SCANCODE_KP_ENTER: return XKEY_NUMPADENTER; case SDL_SCANCODE_RCTRL: return XKEY_RCONTROL;
    case SDL_SCANCODE_KP_DIVIDE: return XKEY_DIVIDE; case SDL_SCANCODE_SYSREQ: return XKEY_PRINT;
    case SDL_SCANCODE_RALT: return XKEY_RALT; case SDL_SCANCODE_HOME: return XKEY_HOME;
    case SDL_SCANCODE_UP: return XKEY_UP; case SDL_SCANCODE_PAGEUP: return XKEY_PAGE_UP;
    case SDL_SCANCODE_LEFT: return XKEY_LEFT; case SDL_SCANCODE_RIGHT: return XKEY_RIGHT;
    case SDL_SCANCODE_END: return XKEY_END; case SDL_SCANCODE_DOWN: return XKEY_DOWN;
    case SDL_SCANCODE_PAGEDOWN: return XKEY_PAGE_DOWN; case SDL_SCANCODE_INSERT: return XKEY_INSERT;
    case SDL_SCANCODE_DELETE: return XKEY_DELETE; case SDL_SCANCODE_LGUI: return XKEY_WIN_LWINDOW;
    case SDL_SCANCODE_RGUI: return XKEY_WIN_RWINDOW; case SDL_SCANCODE_APPLICATION: return XKEY_WIN_APPS;
    default: return XKEY_NULL;
    }
}

CXKeyboard::CXKeyboard() { memset(m_cKeysState,0,sizeof(m_cKeysState)); memset(m_cOldKeysState,0,sizeof(m_cOldKeysState)); memset(m_cTempKeys,0,sizeof(m_cTempKeys)); m_modifiers=0; m_lastKey=0; m_pSystem=0; m_pLog=0; m_pInput=0; m_bExclusiveMode=false; }
CXKeyboard::~CXKeyboard() {}
bool CXKeyboard::Init(CInput* pInput, ISystem* pSystem)
{
    m_pInput=pInput; m_pSystem=pSystem; m_pLog=pSystem?pSystem->GetILog():0;
    SDL_InitSubSystem(SDL_INIT_EVENTS);
    return true;
}
void CXKeyboard::ShutDown() {}
void CXKeyboard::BeginFrame() { memcpy(m_cOldKeysState,m_cKeysState,sizeof(m_cKeysState)); }
void CXKeyboard::OnKey(int k, bool down)
{
    if (k <= 0 || k >= XKEY_STATE_COUNT) return;

    bool bChanged = false;
    if (down) { if (!(m_cKeysState[k]&0x80)) { m_lastKey=k; bChanged=true; } m_cKeysState[k]|=0x80; }
    else { if (m_cKeysState[k]&0x80) bChanged=true; m_cKeysState[k]&=~0x80; }

    if (k==XKEY_LSHIFT||k==XKEY_RSHIFT) { if(down) m_modifiers|=XKEY_MOD_LSHIFT; else m_modifiers&=~XKEY_MOD_LSHIFT; }
    if (k==XKEY_LCONTROL||k==XKEY_RCONTROL) { if(down) m_modifiers|=XKEY_MOD_LCONTROL; else m_modifiers&=~XKEY_MOD_LCONTROL; }
    if (k==XKEY_LALT||k==XKEY_RALT) { if(down) m_modifiers|=XKEY_MOD_LALT; else m_modifiers&=~XKEY_MOD_LALT; }

    // Post an SInputEvent so console key binds (SkipCutScene, ESC menu switch)
    // and the UI menu screens receive keyboard input. Before this the Linux
    // SDL devices only updated polled state, so BindCommandToKey never fired.
    if (bChanged && m_pInput)
    {
        SInputEvent event;
        event.key = k;
        event.type = down ? SInputEvent::KEY_PRESS : SInputEvent::KEY_RELEASE;
        event.timestamp = 0;
        event.moidifiers = m_modifiers;
        event.keyname = m_pInput->GetKeyName(event.key,event.moidifiers);
        m_pInput->PostInputEvent(event);
    }
}
void CXKeyboard::SetSyntheticKeyState(int k, bool down)
{
    if (k <= 0 || k >= XKEY_STATE_COUNT) return;
    if (down) m_cKeysState[k]|=0x80; else m_cKeysState[k]&=~0x80;
    if (k==XKEY_LSHIFT||k==XKEY_RSHIFT) { if(down) m_modifiers|=XKEY_MOD_LSHIFT; else m_modifiers&=~XKEY_MOD_LSHIFT; }
    if (k==XKEY_LCONTROL||k==XKEY_RCONTROL) { if(down) m_modifiers|=XKEY_MOD_LCONTROL; else m_modifiers&=~XKEY_MOD_LCONTROL; }
    if (k==XKEY_LALT||k==XKEY_RALT) { if(down) m_modifiers|=XKEY_MOD_LALT; else m_modifiers&=~XKEY_MOD_LALT; }
}
void CXKeyboard::Update() {}
bool CXKeyboard::KeyDown(int k) { return k>0 && k<XKEY_STATE_COUNT && (m_cKeysState[k]&0x80)!=0; }
bool CXKeyboard::KeyPressed(int k) { return k>0 && k<XKEY_STATE_COUNT && (m_cKeysState[k]&0x80) && !(m_cOldKeysState[k]&0x80); }
bool CXKeyboard::KeyReleased(int k) { return k>0 && k<XKEY_STATE_COUNT && !(m_cKeysState[k]&0x80) && (m_cOldKeysState[k]&0x80); }
void CXKeyboard::ClearKey(int k) { if(k>0&&k<XKEY_STATE_COUNT) m_cKeysState[k]=0; }
int CXKeyboard::GetKeyPressedCode() { for(int i=1;i<XKEY_STATE_COUNT;i++) if(KeyPressed(i)) return i; return XKEY_NULL; }
const char* CXKeyboard::GetKeyPressedName() { static char n[16]; int k=GetKeyPressedCode(); snprintf(n,sizeof(n),"key%d",k); return n; }
int CXKeyboard::GetKeyDownCode() { for(int i=1;i<XKEY_STATE_COUNT;i++) if(KeyDown(i)) return i; return XKEY_NULL; }
const char* CXKeyboard::GetKeyDownName() { static char n[16]; int k=GetKeyDownCode(); snprintf(n,sizeof(n),"key%d",k); return n; }
void CXKeyboard::ClearKeyState() { memset(m_cKeysState,0,sizeof(m_cKeysState)); memset(m_cOldKeysState,0,sizeof(m_cOldKeysState)); m_modifiers=0; }
unsigned char CXKeyboard::GetKeyState(int nKey) { return (nKey>0&&nKey<XKEY_STATE_COUNT)?m_cKeysState[nKey]:0; }
unsigned char CXKeyboard::XKEY2ASCII(unsigned short nCode, int modifiers)
{
    bool shift = (modifiers & XKEY_MOD_SHIFT) != 0;
    if (nCode >= XKEY_A && nCode <= XKEY_Z) { char c='a'+(nCode-XKEY_A); return shift?c-32:c; }
    if (nCode >= XKEY_0 && nCode <= XKEY_9) { const char* d="0123456789"; return d[nCode-XKEY_0]; }
    switch(nCode) {
    case XKEY_SPACE: return ' '; case XKEY_RETURN: return '\r'; case XKEY_TAB: return '\t';
    case XKEY_COMMA: return shift?'<':','; case XKEY_PERIOD: return shift?'>':'.';
    case XKEY_MINUS: return shift?'_':'-'; case XKEY_EQUALS: return shift?'+':'=';
    case XKEY_SLASH: return shift?'?':'/'; case XKEY_BACKSLASH: return shift?'|':'\\';
    case XKEY_SEMICOLON: return shift?':':';'; case XKEY_APOSTROPHE: return shift?'"':'\'';
    case XKEY_LBRACKET: return shift?'{':'['; case XKEY_RBRACKET: return shift?'}':']';
    default: return 0;
    }
}
bool CXKeyboard::GetOSKeyName(int nKey, wchar_t* out, int n) { if(out&&n>0){ out[0]=XKEY2ASCII(nKey,m_modifiers); out[1]=0;} return true; }
void CXKeyboard::FeedVirtualKey(int vk, long, bool down) { OnKey(vk, down); }

// ---------------------------------------------------------------------------
// mouse
// ---------------------------------------------------------------------------
CXMouse::CXMouse() { memset(m_Deltas,0,sizeof(m_Deltas)); memset(m_OldDeltas,0,sizeof(m_OldDeltas)); memset(m_Buttons,0,sizeof(m_Buttons)); memset(m_OldButtons,0,sizeof(m_OldButtons)); m_fSensitivity=1.0f; m_fSensitivityScale=1.0f; m_kInertia=0; m_fVScreenX=m_fVScreenY=0; m_exmode=false; m_bPendingDeltas=false; m_PendingDX=m_PendingDY=0; m_pInput=0; }
CXMouse::~CXMouse() {}
bool CXMouse::Init(ISystem* pSystem) { m_pInput=0; return true; }
void CXMouse::Shutdown() { SetExclusive(false); }
void CXMouse::BeginFrame() { memcpy(m_OldButtons,m_Buttons,sizeof(m_Buttons)); memcpy(m_OldDeltas,m_Deltas,sizeof(m_Deltas)); m_Deltas[2]=0; m_Deltas[0]=m_PendingDX; m_Deltas[1]=m_PendingDY; m_PendingDX=m_PendingDY=0; }
void CXMouse::Update(bool) {}
void CXMouse::ClearKeyState() { memset(m_Buttons,0,sizeof(m_Buttons)); memset(m_Deltas,0,sizeof(m_Deltas)); memset(m_OldDeltas,0,sizeof(m_OldDeltas)); m_PendingDX=m_PendingDY=0; }
void CXMouse::OnButton(int b, bool down) { if(b>=0&&b<8) { if(down) m_Buttons[b]|=0x80; else m_Buttons[b]&=~0x80; } }
void CXMouse::OnMove(float dx, float dy) { m_PendingDX+=dx; m_PendingDY+=dy; }
void CXMouse::OnWheel(int dy) { m_Deltas[2]=dy; }
// IInput queries use XKEY codes, not SDL's zero-based button slots.
// Axis keys must participate in action-map hold checks for mouse/stick look.
static bool MouseStateDown(int key, const int* buttons, const float* deltas)
{
    if(key==XKEY_MAXIS_X) return deltas[0]!=0.f;
    if(key==XKEY_MAXIS_Y) return deltas[1]!=0.f;
    if(key==XKEY_MWHEEL_UP) return deltas[2]>0.f;
    if(key==XKEY_MWHEEL_DOWN) return deltas[2]<0.f;
    if(key<XKEY_MOUSE1 || key>XKEY_MOUSE8 || (key & 0xffff)) return false;
    return (buttons[(key>>16)-1] & 0x80)!=0;
}
bool CXMouse::MouseDown(int key) { return MouseStateDown(key,m_Buttons,m_Deltas); }
bool CXMouse::MousePressed(int key) { return MouseStateDown(key,m_Buttons,m_Deltas) && !MouseStateDown(key,m_OldButtons,m_OldDeltas); }
bool CXMouse::MouseReleased(int key) { return !MouseStateDown(key,m_Buttons,m_Deltas) && MouseStateDown(key,m_OldButtons,m_OldDeltas); }
bool CXMouse::MouseDblClick(int) { return false; }
bool CXMouse::GetOSKeyName(int, wchar_t*, int) { return false; }
void CXMouse::SetMouseWheelRotation(int value) { m_Deltas[2]+=value; }
bool CXMouse::SetExclusive(bool value, void*) { m_exmode=value; SDL_SetRelativeMouseMode(value?SDL_TRUE:SDL_FALSE); return true; }
void CXMouse::PostEvent(int key, SInputEvent::EType type, float value, unsigned int)
{
    if (!m_pInput) return;
    SInputEvent event;
    event.key = key;
    event.type = type;
    event.timestamp = (unsigned int)SDL_GetTicks();
    event.moidifiers = m_pInput->GetModifiers();
    event.keyname = m_pInput->GetKeyName(event.key,event.moidifiers);
    event.value = value;
    m_pInput->PostInputEvent(event);
}

// ---------------------------------------------------------------------------
// gamepad
// ---------------------------------------------------------------------------
CGamepad::CGamepad()
{
    m_LX=m_LY=m_RX=m_RY=0.0f;
    m_fDeadZone = PortEnvFloat("FARCRY_PAD_DEADZONE", 0.22f);
    if (m_fDeadZone < 0.02f) m_fDeadZone = 0.02f;
    if (m_fDeadZone > 0.90f) m_fDeadZone = 0.90f;
    m_bConnected = false;
}
void CGamepad::BeginFrame()
{
    m_OldState = m_State;
}
void CGamepad::ClearState()
{
    m_State.clear();
    m_OldState.clear();
    m_LX=m_LY=m_RX=m_RY=0.0f;
}
void CGamepad::SetButton(int k, bool down)
{
    if (k <= 0 || k == XKEY_NULL) return;
    if (down) m_State[k] |= 0x80;
    else m_State[k] = 0;
}
void CGamepad::SetAxis(int k, float v)
{
    if (k <= 0 || k == XKEY_NULL) return;
    SetButton(k, v > 0.5f);
}
void CGamepad::SetStick(float lx,float ly,float rx,float ry) { m_LX=lx; m_LY=ly; m_RX=rx; m_RY=ry; }
bool CGamepad::KeyDown(int k)
{
    if (k <= 0 || k == XKEY_NULL) return false;
    std::map<int,unsigned char>::const_iterator it = m_State.find(k);
    return (it != m_State.end()) && ((it->second & 0x80) != 0);
}
bool CGamepad::KeyPressed(int k)
{
    if (k <= 0 || k == XKEY_NULL) return false;
    std::map<int,unsigned char>::const_iterator it = m_State.find(k);
    if (it == m_State.end() || !(it->second & 0x80)) return false;
    std::map<int,unsigned char>::const_iterator old = m_OldState.find(k);
    return (old == m_OldState.end()) || !(old->second & 0x80);
}
bool CGamepad::KeyReleased(int k)
{
    if (k <= 0 || k == XKEY_NULL) return false;
    std::map<int,unsigned char>::const_iterator it = m_State.find(k);
    bool bDown = (it != m_State.end()) && ((it->second & 0x80) != 0);
    if (bDown) return false;
    std::map<int,unsigned char>::const_iterator old = m_OldState.find(k);
    return (old != m_OldState.end()) && ((old->second & 0x80) != 0);
}
float CGamepad::GetDeltaX() { return m_RX; }
float CGamepad::GetDeltaY() { return m_RY; }

CJoystick::CJoystick() { m_pLog=0; m_pGamepad=0; m_initialized=false; }
CJoystick::~CJoystick() {}
bool CJoystick::Init(ILog* pLog) { m_pLog=pLog; m_initialized=true; return true; }
void CJoystick::Update() {}
void CJoystick::ShutDown() { m_initialized=false; }
int CJoystick::GetNumButtons() { return 0; }
bool CJoystick::IsButtonPressed(int b) { return m_pGamepad?m_pGamepad->KeyDown(b):false; }
int CJoystick::GetDir() { return 0; }
int CJoystick::GetHatDir() { return 0; }
Vec3 CJoystick::GetAnalog1Dir(unsigned int) const { return m_pGamepad?Vec3(m_pGamepad->GetStickRX(),m_pGamepad->GetStickRY(),0):Vec3(0,0,0); }
Vec3 CJoystick::GetAnalog2Dir(unsigned int) const { return Vec3(0,0,0); }

// ---------------------------------------------------------------------------
// SDL game controller
// ---------------------------------------------------------------------------
static SDL_GameController* g_pad = NULL;
static bool g_bPadLoggedDisconnect = false;

static const char* PadButtonName(int xkey)
{
    switch (xkey)
    {
    case XKEY_GP_A: return "A";
    case XKEY_GP_B: return "B";
    case XKEY_GP_X: return "X";
    case XKEY_GP_Y: return "Y";
    case XKEY_GP_WHITE: return "LB";
    case XKEY_GP_BLACK: return "RB";
    case XKEY_GP_LEFT_TRIGGER: return "LT";
    case XKEY_GP_RIGHT_TRIGGER: return "RT";
    case XKEY_GP_DPAD_UP: return "DPAD_UP";
    case XKEY_GP_DPAD_DOWN: return "DPAD_DOWN";
    case XKEY_GP_DPAD_LEFT: return "DPAD_LEFT";
    case XKEY_GP_DPAD_RIGHT: return "DPAD_RIGHT";
    case XKEY_GP_START: return "START";
    case XKEY_GP_BACK: return "BACK";
    case XKEY_GP_LEFT_THUMB: return "L3";
    case XKEY_GP_RIGHT_THUMB: return "R3";
    case XKEY_GP_STHUMBLUP: return "LSTICK_UP";
    case XKEY_GP_STHUMBLDOWN: return "LSTICK_DOWN";
    case XKEY_GP_STHUMBLLEFT: return "LSTICK_LEFT";
    case XKEY_GP_STHUMBLRIGHT: return "LSTICK_RIGHT";
    default: return "?";
    }
}

// Rank a joystick as the game's pad. Under PortMaster, gptokeyb2 -x exposes a
// virtual Xbox 360 controller (045e:028e) and grabs the physical device, so
// that virtual pad is the one carrying input. FARCRY_PAD_NAME overrides.
static int PadPreference(int index)
{
    if (!SDL_IsGameController(index)) return -1;
    const char* want = getenv("FARCRY_PAD_NAME");
    const char* name = SDL_GameControllerNameForIndex(index);
    if (want && *want) return (name && strstr(name, want)) ? 3 : 0;
    if (SDL_JoystickGetDeviceVendor(index) == 0x045e && SDL_JoystickGetDeviceProduct(index) == 0x028e) return 2;
    return 1;
}

static int g_padPreference = -1;

static void OpenPad(CGamepad& gp)
{
    int n = SDL_NumJoysticks();
    int best = -1, bestPref = g_pad ? g_padPreference : 0;
    for (int i = 0; i < n; ++i) {
        int pref = PadPreference(i);
        if (pref > bestPref) { best = i; bestPref = pref; }
    }
    if (best < 0) return;
    SDL_GameController* pad = SDL_GameControllerOpen(best);
    if (!pad) return;
    if (g_pad) { PortLog("switching to preferred game controller"); SDL_GameControllerClose(g_pad); }
    g_pad = pad;
    g_padPreference = bestPref;
    const char* szName = SDL_GameControllerName(g_pad);
    PortLog("game controller opened: %s (joystick %d, %04x:%04x)", szName ? szName : "unknown", best,
            SDL_JoystickGetDeviceVendor(best), SDL_JoystickGetDeviceProduct(best));
    gp.SetConnected(true);
    gp.ClearState();
    g_bPadLoggedDisconnect = false;
}

static void ClosePad(CGamepad& gp)
{
    if (!g_pad) return;
    PortLog("game controller disconnected");
    SDL_GameControllerClose(g_pad);
    g_pad = NULL;
    g_padPreference = -1;
    gp.SetConnected(false);
    gp.ClearState();
    g_bPadLoggedDisconnect = true;
}

bool PortGamepadIsConnected() { return g_pad != NULL; }

// --------------- virtual controller test harness ---------------------------
#if SDL_VERSION_ATLEAST(2,0,14)
static SDL_Joystick* g_virtualPad = NULL;
static int g_virtualPadButton = 0;
static int g_virtualPadAxis = 0;
static Uint32 g_virtualPadStart = 0;
static int g_virtualPadStage = -1;

void PortInitVirtualGamepad()
{
    if (!PortEnvFlag("FARCRY_TEST_VIRTUAL_PAD", false)) return;
    if (g_virtualPad) return;
    int nButtons = SDL_CONTROLLER_BUTTON_MAX;
    if (nButtons < 15) nButtons = 15;
    int idx = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, nButtons, 0);
    if (idx < 0) {
        PortLog("virtual game controller attach failed: %s", SDL_GetError());
        return;
    }
    g_virtualPad = SDL_JoystickOpen(idx);
    if (!g_virtualPad) {
        PortLog("virtual game controller open failed: %s", SDL_GetError());
        return;
    }
    g_virtualPadStart = SDL_GetTicks();
    g_virtualPadStage = -1;
    PortLog("virtual game controller attached (FARCRY_TEST_VIRTUAL_PAD=1)");
}

static void VirtualPadSetButton(int button, bool down)
{
    if (!g_virtualPad) return;
    SDL_JoystickSetVirtualButton(g_virtualPad, button, down ? 1 : 0);
    if (g_virtualPadButton != button || down) {
        PortLog("virtual pad: button %d %s", button, down ? "DOWN" : "UP");
        g_virtualPadButton = button;
    }
}
static void VirtualPadSetAxis(int axis, Sint16 value)
{
    if (!g_virtualPad) return;
    SDL_JoystickSetVirtualAxis(g_virtualPad, axis, value);
    if (g_virtualPadAxis != axis || value != 0) {
        PortLog("virtual pad: axis %d = %d", axis, (int)value);
        g_virtualPadAxis = axis;
    }
}

void PortUpdateVirtualGamepadTest(CGamepad& gamepad)
{
    if (!g_virtualPad) return;
    Uint32 t = SDL_GetTicks() - g_virtualPadStart;
    // Let tests wait for level load/player binding before applying movement.
    const char* delayText = getenv("FARCRY_VIRTUAL_PAD_DELAY_MS");
    Uint32 delay = delayText ? (Uint32)atoi(delayText) : 0;
    if (t < delay) return;
    t -= delay;
    int stage = (int)(t / 1500);
    // Advance at most one stage per polled frame so a slow frame (or a busy
    // machine) can never skip a button/axis step of the test sequence.
    if (stage > g_virtualPadStage + 1) stage = g_virtualPadStage + 1;
    if (stage == g_virtualPadStage) return;
    PortLog("virtual pad: stage %d", stage);

    // release the previous stage controls
    VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
    VirtualPadSetButton(SDL_CONTROLLER_BUTTON_B, false);
    VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_UP, false);
    VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
    VirtualPadSetButton(SDL_CONTROLLER_BUTTON_X, false);
    VirtualPadSetButton(SDL_CONTROLLER_BUTTON_START, false);
    VirtualPadSetAxis(SDL_CONTROLLER_AXIS_LEFTY, 0);
    VirtualPadSetAxis(SDL_CONTROLLER_AXIS_RIGHTX, 0);
    VirtualPadSetAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0);
    g_virtualPadStage = stage;

    const bool bMenuOnly = getenv("FARCRY_TEST_VIRTUAL_PAD_MENU") != NULL;
    const bool bMenuStart = getenv("FARCRY_TEST_VIRTUAL_PAD_MENU_START") != NULL;
    const bool bMenuStartPause = getenv("FARCRY_TEST_VIRTUAL_PAD_MENU_START_PAUSE") != NULL;
    const bool bMenuStartPlay = getenv("FARCRY_TEST_VIRTUAL_PAD_MENU_START_PLAY") != NULL;
    const bool bMenuLoad = getenv("FARCRY_TEST_VIRTUAL_PAD_MENU_LOAD") != NULL;
    const bool bMenuLoadBack = getenv("FARCRY_TEST_VIRTUAL_PAD_MENU_LOAD_BACK") != NULL;
    if (bMenuLoadBack)
    {
        if (stage == 0) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (stage == 1) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (stage == 2) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_B, true);
        else if (stage == 3) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_B, false);
        else if (stage == 4) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
        else if (stage == 6) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (stage == 7) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (stage == 9) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (stage == 10) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (stage >= 14) { PortLog("virtual pad load-back test finished; disconnecting"); SDL_JoystickClose(g_virtualPad); g_virtualPad = NULL; g_virtualPadStage = 0; }
        return;
    }
    if (bMenuLoad)
    {
        if (stage == 0) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (stage == 1) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (stage == 2) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_Y, true);
        else if (stage == 3) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_Y, false);
        else if (stage == 4) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (stage == 5) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (stage == 7) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (stage == 8) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (stage == 10) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (stage == 11) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (stage >= 14) { PortLog("virtual pad load test finished; disconnecting"); SDL_JoystickClose(g_virtualPad); g_virtualPad = NULL; g_virtualPadStage = 0; }
        return;
    }
    if (bMenuStart || bMenuStartPause || bMenuStartPlay)
    {
        if (stage == 0) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (stage == 1) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (stage == 2) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (stage == 3) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (stage == 4) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (stage == 5) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (bMenuStart && stage == 6) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (bMenuStart && stage == 7) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (bMenuStart && stage == 8) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true);
        else if (bMenuStart && stage == 9) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else if (bMenuStartPause && stage == 11) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_START, true);
        else if (bMenuStartPause && stage == 12) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_START, false);
        else if (bMenuStartPause && stage == 16) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_START, true);
        else if (bMenuStartPause && stage == 17) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_START, false);
        else if (bMenuStartPlay && stage == 11) VirtualPadSetAxis(SDL_CONTROLLER_AXIS_LEFTY, -32768);
        else if (bMenuStartPlay && stage == 12) VirtualPadSetAxis(SDL_CONTROLLER_AXIS_RIGHTX, 32767);
        else if (bMenuStartPlay && stage == 13) VirtualPadSetAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
        else if (stage >= (bMenuStartPause ? 18 : (bMenuStartPlay ? 15 : 6))) { PortLog("virtual pad start test finished; disconnecting"); SDL_JoystickClose(g_virtualPad); g_virtualPad = NULL; g_virtualPadStage = 0; }
        return;
    }
    switch (stage)
    {
    case 0: VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true); break;
    case 1:
        if (bMenuStart) {
            VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
            if (stage >= 0) { /* release only */ }
            break;
        }
        if (bMenuOnly) VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false);
        else VirtualPadSetButton(SDL_CONTROLLER_BUTTON_B, true);
        break;
    case 2:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true); break; }
        VirtualPadSetAxis(SDL_CONTROLLER_AXIS_LEFTY, -32768); break;  // forward
    case 3:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true); break; }
        VirtualPadSetAxis(SDL_CONTROLLER_AXIS_RIGHTX, 32767); break;  // look right
    case 4:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true); break; }
        VirtualPadSetAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767); break; // fire
    case 5:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true); break; }
        VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_UP, true); break;
    case 6:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true); break; }
        VirtualPadSetButton(SDL_CONTROLLER_BUTTON_X, true); break;
    case 7:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true); break; }
        VirtualPadSetButton(SDL_CONTROLLER_BUTTON_START, true); break;
    case 8:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true); break; }
        VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false); break;
    case 9:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true); break; }
        PortLog("virtual pad test finished; disconnecting");
        SDL_JoystickClose(g_virtualPad);
        g_virtualPad = NULL;
        g_virtualPadStage = 0;
        break;
    case 10:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true); break; }
        break;
    case 11:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false); break; }
        break;
    case 12:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, true); break; }
        break;
    case 13:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true); break; }
        break;
    case 14:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false); break; }
        break;
    case 15:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, true); break; }
        break;
    case 16:
        if (bMenuOnly) { VirtualPadSetButton(SDL_CONTROLLER_BUTTON_A, false); break; }
        break;
    case 17:
        break;
    case 18:
        if (bMenuOnly) { PortLog("virtual pad menu test finished; disconnecting"); SDL_JoystickClose(g_virtualPad); g_virtualPad = NULL; g_virtualPadStage = 0; }
        break;
    default: break;
    }
}
#else
void PortInitVirtualGamepad() {}
void PortUpdateVirtualGamepadTest(CGamepad&) {}
static void VirtualPadSetButton(int, bool) {}
static void VirtualPadSetAxis(int, Sint16) {}
#endif

static void PollPad(CGamepad& gp)
{
    if (!g_pad) return;

    const float fDeadZone = gp.GetDeadZone();
    const float fTriggerThreshold = PortEnvFloat("FARCRY_PAD_TRIGGER_THRESHOLD", 0.35f);

    struct SBtn { int xkey; SDL_GameControllerButton sdl; };
    static const SBtn s_buttons[] = {
        { XKEY_GP_A, SDL_CONTROLLER_BUTTON_A },
        { XKEY_GP_B, SDL_CONTROLLER_BUTTON_B },
        { XKEY_GP_X, SDL_CONTROLLER_BUTTON_X },
        { XKEY_GP_Y, SDL_CONTROLLER_BUTTON_Y },
        { XKEY_GP_WHITE, SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
        { XKEY_GP_BLACK, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
        { XKEY_GP_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_UP },
        { XKEY_GP_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN },
        { XKEY_GP_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT },
        { XKEY_GP_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
        { XKEY_GP_START, SDL_CONTROLLER_BUTTON_START },
        { XKEY_GP_BACK, SDL_CONTROLLER_BUTTON_BACK },
        { XKEY_GP_LEFT_THUMB, SDL_CONTROLLER_BUTTON_LEFTSTICK },
        { XKEY_GP_RIGHT_THUMB, SDL_CONTROLLER_BUTTON_RIGHTSTICK },
    };
    for (unsigned i = 0; i < sizeof(s_buttons)/sizeof(s_buttons[0]); ++i)
        gp.SetButton(s_buttons[i].xkey, SDL_GameControllerGetButton(g_pad, s_buttons[i].sdl) != 0);

    const float fLT = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
    const float fRT = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
    gp.SetButton(XKEY_GP_LEFT_TRIGGER, fLT > fTriggerThreshold);
    gp.SetButton(XKEY_GP_RIGHT_TRIGGER, fRT > fTriggerThreshold);

    float lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
    float ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
    float rx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
    float ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;

    // radial deadzone, preserving direction and upper-half magnitude
    float lmag = sqrtf(lx*lx + ly*ly);
    if (lmag < fDeadZone) { lx = ly = 0.0f; }
    else { float s = (lmag - fDeadZone) / (1.0f - fDeadZone); lx = lx/lmag*s; ly = ly/lmag*s; }
    float rmag = sqrtf(rx*rx + ry*ry);
    if (rmag < fDeadZone) { rx = ry = 0.0f; }
    else { float s = (rmag - fDeadZone) / (1.0f - fDeadZone); rx = rx/rmag*s; ry = ry/rmag*s; }

    gp.SetStick(lx, ly, rx, ry);
    if (PortEnvFlag("FARCRY_PAD_DEBUG", false))
    {
        static unsigned s_n = 0;
        if ((s_n++ % 30) == 0)
            PortLog("pad axes raw L(%d,%d) R(%d,%d) -> L(%.2f,%.2f) R(%.2f,%.2f)",
                    SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX), SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY),
                    SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX), SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY),
                    lx, ly, rx, ry);
    }

    // The XBox oriented engine binds movement to the virtual XKEY_GP_STHUMB*
    // direction keys; keep them in sync with the analog stick.
    const float fDirThreshold = 0.35f;
    gp.SetButton(XKEY_GP_STHUMBLUP,    ly < -fDirThreshold);
    gp.SetButton(XKEY_GP_STHUMBLDOWN,  ly >  fDirThreshold);
    gp.SetButton(XKEY_GP_STHUMBLLEFT,  lx < -fDirThreshold);
    gp.SetButton(XKEY_GP_STHUMBLRIGHT, lx >  fDirThreshold);
}

void PortPollSDLEvents(CXKeyboard& kb, CXMouse& ms, CGamepad& gp, CJoystick& joy)
{
    static bool focused = true;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                focused = false; kb.ClearKeyState(); ms.ClearKeyState(); gp.ClearState();
            } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                focused = true;
            }
            break;
        case SDL_QUIT:
            if (ISystem* sys = GetISystem()) sys->Quit();
            break;
        case SDL_KEYDOWN:
            if (!e.key.repeat) kb.OnKey(SDLScancodeToXKEY(e.key.keysym.scancode), true);
            break;
        case SDL_KEYUP:
            kb.OnKey(SDLScancodeToXKEY(e.key.keysym.scancode), false);
            break;
        case SDL_MOUSEMOTION:
        {
            ms.OnMove((float)e.motion.xrel, (float)e.motion.yrel);
            // The menus work in Far Cry's virtual 800x600 screen; SDL reports window pixels.
            int ww = 800, wh = 600;
            if (SDL_Window* w = SDL_GetWindowFromID(e.motion.windowID)) SDL_GetWindowSize(w, &ww, &wh);
            ms.SetVScreenX(ww > 0 ? e.motion.x * 800.0f / ww : (float)e.motion.x);
            ms.SetVScreenY(wh > 0 ? e.motion.y * 600.0f / wh : (float)e.motion.y);
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button >= 1 && e.button.button <= 8) {
                const int slot = e.button.button==SDL_BUTTON_RIGHT ? 1 : e.button.button==SDL_BUTTON_MIDDLE ? 2 : e.button.button-1;
                ms.OnButton(slot, true);
                ms.PostEvent(XKEY_MOUSE1 + (slot << 16), SInputEvent::KEY_PRESS);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button >= 1 && e.button.button <= 8) {
                const int slot = e.button.button==SDL_BUTTON_RIGHT ? 1 : e.button.button==SDL_BUTTON_MIDDLE ? 2 : e.button.button-1;
                ms.OnButton(slot, false);
                ms.PostEvent(XKEY_MOUSE1 + (slot << 16), SInputEvent::KEY_RELEASE);
            }
            break;
        case SDL_MOUSEWHEEL:
            ms.OnWheel(e.wheel.y);
            if (e.wheel.y > 0) ms.PostEvent(XKEY_MWHEEL_UP, SInputEvent::KEY_PRESS);
            else if (e.wheel.y < 0) ms.PostEvent(XKEY_MWHEEL_DOWN, SInputEvent::KEY_PRESS);
            break;
        case SDL_CONTROLLERDEVICEADDED:
            OpenPad(gp); // also upgrades to a newly attached preferred pad
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad) {
                SDL_Joystick* pJoy = SDL_GameControllerGetJoystick(g_pad);
                if (!pJoy || SDL_JoystickInstanceID(pJoy) == e.cdevice.which)
                    ClosePad(gp);
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            if (PortEnvFlag("FARCRY_PAD_DEBUG", false)) {
                // find xkey for logging
                PortLog("controller button %d down", (int)e.cbutton.button);
            }
            break;
        default: break;
        }
    }

    PortUpdateVirtualGamepadTest(gp);

    if (!g_pad) OpenPad(gp);

    if (g_pad && focused) PollPad(gp);
    else if (PortEnvFlag("FARCRY_PAD_DEBUG", false) && !g_bPadLoggedDisconnect)
    {
        PortLog("no game controller detected");
        g_bPadLoggedDisconnect = true;
    }

    (void)joy;
}
