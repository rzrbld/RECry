// SDL2 keyboard/mouse/gamepad devices for the Linux/AArch64 port.
#pragma once

#include <IInput.h>
#include <SDL.h>
#include <map>

struct ILog;
class CInput;

#ifndef _XBOX
struct IGamepad
{
    virtual void ShutDown() = 0;
    virtual bool KeyDown(int p_numButton) = 0;
    virtual bool KeyPressed(int p_numButton) = 0;
    virtual bool KeyReleased(int p_numButton) = 0;
    virtual float GetDeltaX() = 0;
    virtual float GetDeltaY() = 0;
};
#endif

class CXKeyboard : public IKeyboard
{
public:
    CXKeyboard();
    virtual ~CXKeyboard();
    bool Init(CInput* pInput, ISystem* pSystem);
    bool Init(ILog* pLog) { return true; }
    void ShutDown();
    void SetKey(int, int) {}
    void SetPrevKey(int, int) {}
    bool KeyDown(int k);
    bool KeyPressed(int k);
    bool KeyReleased(int k);
    void ClearKey(int k);
    int GetKeyPressedCode();
    const char* GetKeyPressedName();
    int GetKeyDownCode();
    const char* GetKeyDownName();
    void SetExclusive(bool, void*) {}
    void WaitForKey() {}
    void ClearKeyState();
    unsigned char GetKeyState(int nKey);
    void Update();
    int GetModifiers() const { return m_modifiers; }
    unsigned short DIK2XKEY(unsigned char cCode) { return cCode; }
    unsigned char XKEY2DIK(unsigned short nCode) { return (unsigned char)nCode; }
    unsigned char XKEY2ASCII(unsigned short nCode, int modifiers);
    bool GetOSKeyName(int nKey, wchar_t* szwKeyName, int iBufSize);
    void FeedVirtualKey(int nVirtualKey, long lParam, bool bDown);
    void BeginFrame();
    void OnKey(int xkey, bool down);
    //! Update polled state without posting an SInputEvent (used for synthetic
    //! modifier state while injecting gamepad menu navigation keys).
    void SetSyntheticKeyState(int xkey, bool down);
private:
    enum { XKEY_STATE_COUNT = 1024 };
    unsigned char m_cKeysState[XKEY_STATE_COUNT];
    unsigned char m_cOldKeysState[XKEY_STATE_COUNT];
    unsigned char m_cTempKeys[XKEY_STATE_COUNT];
    int m_modifiers;
    int m_lastKey;
    ISystem* m_pSystem;
    ILog* m_pLog;
    CInput* m_pInput;
    bool m_bExclusiveMode;
};

class CXMouse : public IMouse
{
public:
    CXMouse();
    virtual ~CXMouse();
    bool Init(ISystem* pSystem);
    void Shutdown();
    void Update(bool bPrevFocus);
    void BeginFrame();
    void ClearKeyState();
    bool MouseDown(int b);
    bool MousePressed(int b);
    bool MouseDblClick(int b);
    bool MouseReleased(int b);
    bool GetOSKeyName(int nKey, wchar_t* szwKeyName, int iBufSize);
    void SetMouseWheelRotation(int value);
    bool SetExclusive(bool value, void* hwnd = 0);
    float GetDeltaX() { return m_Deltas[0] * (m_fSensitivity * m_fSensitivityScale); }
    float GetDeltaY() { return m_Deltas[1] * (m_fSensitivity * m_fSensitivityScale); }
    float GetDeltaZ() { return m_Deltas[2]; }
    void SetInertia(float k) { m_kInertia = k; }
    void SetVScreenX(float f) { m_fVScreenX = f; }
    void SetVScreenY(float f) { m_fVScreenY = f; }
    float GetVScreenX() { return m_fVScreenX; }
    float GetVScreenY() { return m_fVScreenY; }
    void SetSensitvity(float f) { m_fSensitivity = f; }
    float GetSensitvity() { return m_fSensitivity; }
    void SetSensitvityScale(float f) { m_fSensitivityScale = f; }
    float GetSensitvityScale() { return m_fSensitivityScale; }
    bool Acquire() { return true; }
    bool UnAcquire() { return true; }
    void PostEvent(int key, SInputEvent::EType type, float value = 0, unsigned int timestamp = 0);
    void OnButton(int b, bool down);
    void OnMove(float dx, float dy);
    void OnWheel(int dy);
    CInput* m_pInput;
private:
    float m_Deltas[5];
    float m_OldDeltas[5];
    int m_Buttons[8];
    int m_OldButtons[8];
    float m_fSensitivity;
    float m_fSensitivityScale;
    float m_kInertia;
    float m_fVScreenX, m_fVScreenY;
    bool m_exmode;
    bool m_bPendingDeltas;
    float m_PendingDX, m_PendingDY;
};

// SDL2 gamepad. The XKEY_GP_* codes are large (0x01000000..0x24000000), so the
// button state is kept in a sparse map keyed by the code instead of a fixed
// array sized XKEY_NULL+1 (XKEY_NULL is 0 and would overflow).
class CGamepad : public IGamepad
{
public:
    CGamepad();
    void ShutDown() {}
    bool KeyDown(int b);
    bool KeyPressed(int b);
    bool KeyReleased(int b);
    float GetDeltaX();
    float GetDeltaY();
    void BeginFrame();
    void SetButton(int xkey, bool down);
    void SetAxis(int xkey, float v);
    void SetStick(float lx, float ly, float rx, float ry);
    void ClearState();

    //! True when an SDL game controller is currently open.
    bool IsConnected() const { return m_bConnected; }
    void SetConnected(bool bConnected) { m_bConnected = bConnected; }

    // Analog sticks, normalised to [-1,1]. Y is positive down (SDL convention).
    float GetStickLX() const { return m_LX; }
    float GetStickLY() const { return m_LY; }
    float GetStickRX() const { return m_RX; }
    float GetStickRY() const { return m_RY; }
    float GetDeadZone() const { return m_fDeadZone; }
    void SetDeadZone(float fDeadZone) { m_fDeadZone = fDeadZone; }

private:
    std::map<int, unsigned char> m_State;
    std::map<int, unsigned char> m_OldState;
    float m_LX, m_LY, m_RX, m_RY;
    float m_fDeadZone;
    bool m_bConnected;
};

class CJoystick
{
public:
    CJoystick();
    ~CJoystick();
    bool Init(ILog* pLog);
    void Update();
    void ShutDown();
    int GetNumButtons();
    bool IsButtonPressed(int buttonnum);
    int GetDir();
    int GetHatDir();
    Vec3 GetAnalog1Dir(unsigned int joystickID) const;
    Vec3 GetAnalog2Dir(unsigned int joystickID) const;
    void SetGamepad(CGamepad* p) { m_pGamepad = p; }
private:
    ILog* m_pLog;
    CGamepad* m_pGamepad;
    bool m_initialized;
};

// Polls SDL events and feeds the devices. Called from CInput::Update.
void PortPollSDLEvents(CXKeyboard& keyboard, CXMouse& mouse, CGamepad& gamepad, CJoystick& joystick);

// Attach an SDL virtual game controller when FARCRY_TEST_VIRTUAL_PAD is set
// (used by port/scripts/test_controller.sh to exercise the full input path
// without real hardware). No-op otherwise.
void PortInitVirtualGamepad();

// Drive the scripted button/axis sequence for the virtual controller test.
// Called once per frame from PortPollSDLEvents.
void PortUpdateVirtualGamepadTest(CGamepad& gamepad);

// Returns true if a game controller device is open.
bool PortGamepadIsConnected();
