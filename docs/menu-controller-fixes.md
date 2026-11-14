# Pause menu and Modern controller fixes

After pausing gameplay, navigating Options/Controls or Main Menu could activate
the previously selected checkpoint. Inactive screens could retain keyboard focus,
and Modern's gptokeyb Space key (A/jump) duplicated the native menu click path.

Screen activation now chooses focus in the opening page while respecting nested
options pages. Deactivation clears its keyboard focus, mouse capture and hover;
input dispatch rejects inactive screens. Modern menu routing stays in pointer
mode and suppresses duplicate gameplay Space broadcasts, while preserving raw
key state for rebinding. Classic's explicit D-pad focus navigation remains.

Modern shooter is preselected for a fresh launcher configuration and is the
PortMaster launch-script default. Explicit saved Classic is respected, including
the required fallback on firmware without gptokeyb2. Modern LB + right stick
left/right emits T for CryVision, replacing alternate lean combinations;
D-pad left/right still lean. CryVision follows normal equipment/battery rules.

Validation:

- Desktop and Bootlin GCC 8.4/glibc 2.30 ARM game builds passed.
- `python3 port/tests/pause_menu_regression.py` compiles production focus,
  activation/deactivation, event filtering and controller menu routing with
  fixtures. Earlier UI and input behaviors independently fail the assertions.
- Host Bunker pause -> Options -> Controls -> Main Menu smoke exited normally,
  focused the expected pages and did not load a save. Controls capture checked.
- Desktop/ARM launcher builds and shell syntax checks passed. Isolated launcher
  runs saved Modern for fresh settings and retained an explicit Classic choice;
  the 640x480 controls-page capture displays the CryVision shortcut.
- Installed on H700; user confirmed the menu issue is fixed. CryVision's T binding
  was checked in the original action maps. Separate user confirmation of the
  physical CryVision combination remains pending.
