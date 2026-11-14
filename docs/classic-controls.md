# Classic controller layout — 2026-11-14

Classic now uses `controls/classic.gptk` through gptokeyb2, with the engine's
existing keyboard/mouse emulation mode and SDL stick handling. This replaces
Classic's previous native button bindings when gptokeyb2 and the preset exist.
Classic is now the new-installation default. Existing saved choices are preserved;
the Modern preset is unchanged.

The final user-provided table supersedes the earlier reWASD reference. No tap,
long-press, inverted aiming or paddle mappings from that reference are included.

- A jump; B hold crouch; X reload; Y use / vehicles.
- RT fire; LT aim; RB grenade; L3 hold sprint; R3 flashlight.
- D-pad up binoculars, down CryVision, left/right previous/next weapon.
- Back pause; Start objectives/scoreboard.
- LB+B prone; LB+X fire mode; LB+down grenade type.
- LB+left mouse wheel up; LB+right mouse wheel down.
- Both sticks move the menu pointer; A clicks; B goes back.

Wheel directions are literal and were explicitly accepted after clarification:
Far Cry's stock binocular wheel-up binding zooms in, wheel-down zooms out.

Classic exports FARCRY_PAD_LB_LOOK=1 so the engine keeps right-stick aiming
available while LB is held. Modern exports 0, retaining LB+stick CryVision and
its look suppression. The legacy native fallback remains available if the
preset or gptokeyb2 is missing, with an explicit log message.

Validation: native and aarch64 engine builds passed; ARM launcher built;
native launcher C syntax passed (host SDL2_ttf runtime unavailable); production
mouse and pause-menu regressions passed. Fresh-install Classic selection and explicit Modern override were checked.
Bootstrap selection checked with
Classic, Modern, unknown preset, missing preset and old-gptokeyb fallbacks.
The H700's installed gptokeyb2 parsed/dumped all preset mappings successfully.
The updated launcher ran on H700 and captured the Classic help page at 640x480;
its text/layout were inspected. Full physical button/gameplay verification is
left for the user. No performance or rendering changes are part of this work.

Installed on 192.168.50.160 while game and launcher were idle. All four uploads
were hash-verified before activation. Existing launcher settings, game configs
and saves were not changed. The user can select Classic in launcher Controls.

Engine SHA256: 774209d1fc44f9ba8fb678677ab596654b58e82dcc5223c20da584254f039cb2
Launcher SHA256: 11482a75e08e3ab7fd5eca087ef5f94c3d9530f291a2073e5e37008fec443440
Classic preset SHA256: 3fb38e74a7ac63dcaa754904b151cf816bd792e4cefa86452031cf15e98e6e94
Bootstrap SHA256: 9a8c6212aa864aa6b599d66e9077a38778092ea75853e4ac72fc7383f4d40fe1

Build/deployment artifacts and the verified screenshot are in
/home/rzrbld/repos/REcry/work/dist/classic-controls-20261114/.
