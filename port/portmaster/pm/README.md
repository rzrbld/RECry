## Notes

RECry: Far Cry (2004) on its original CryEngine, built for Linux and OpenGL ES 3 from the
Far Cry 1 source SDK. Rendering, sound and input go through SDL2 and PortMaster; the story
cutscenes (Bink) play through a small bundled FFmpeg build.

You need your own copy of Far Cry from GOG.com. No game files are included.

Thanks to Crytek for Far Cry and for the engine source, and to Ubisoft. Thanks to the
PortMaster team for gptokeyb, innoextract builds and the runtime this port relies on.

## Installation

1. Copy `RECry.sh` into your Ports folder and the `recry` folder into `ports`.
2. Copy both files of the GOG installer into `ports/recry/installer/`:
   - `setup_far_cry_1.40_hotfix_(29940).exe`
   - `setup_far_cry_1.40_hotfix_(29940)-1.bin`
3. Start **RECry**. The first start unpacks the game into `ports/recry/gamedata/`
   (about 3.5 GB; about 35 minutes on an H700). Afterwards the launcher offers to delete
   the installer.

Alternatively copy an installed Far Cry (the `FCData`, `Levels`, `Shaders` and
`Languages` folders and `game.cfg`) into `ports/recry/gamedata/`.

Everything the port writes stays in `ports/recry/`: settings and saves in `conf/`,
converted textures in `cache/` (made from your game files; do not share it), logs in
`log.txt` and `logs/`. The first visit to each level is slower for a minute while its
textures are converted.

## Performance

On the Anbernic RG40XX-H (H700) all 20 levels run at the 30 FPS cap at 640x480 with
the launcher's Auto settings.

## Launcher settings

| Setting | |
|---|---|
| Resolution | Game resolution. Auto = the screen up to 480 lines, else 480 lines at the screen's shape. 4:3, 16:10 and 16:9 screens are supported. |
| Picture | Keep aspect (bars when shapes differ) or stretch |
| Physics / Effects / View distance | Auto chooses by device: simplified on slow handhelds (H700, RK3326, ...), stock Far Cry on fast ones |
| Menu text size | Bigger menu text for small screens |
| Frame limit | 30, 60 or uncapped |
| FPS counter | Off, On (a small counter) or Built-in (the engine's own statistics) |
| CPU | Performance governor while playing (restored on exit) |
| Controls | Classic or Modern shooter (see Controls) |

## Controls

Two presets, chosen in the launcher (Controls). Both go through PortMaster's gptokeyb2, so
they work with each handheld's own controls. Classic: gptokeyb2 presents a virtual Xbox pad
and the game maps it. Modern shooter: `recry/controls/modern.gptk` maps the buttons to the
game's keyboard/mouse bindings (edit it, or add your own `<name>.gptk` there).

**Classic**

| Button | Action |
|---|---|
| Left stick | Move |
| Right stick | Look |
| RT / LT | Fire / aim (zoom) |
| A | Jump |
| B | Use, enter vehicles |
| X | Reload |
| Y / D-pad left | Next / previous weapon |
| LB / RB | Lean left / right |
| L3 | Sprint |
| R3 | Crouch / prone |
| D-pad up | Fire mode |
| D-pad down | Throw grenade |
| Select | Objectives / score |
| Start | Pause menu |

**Modern shooter** (based on the Steam Input layout "Far Cry 1 custom layout to match modern
shooters"; gptokeyb2 has no tap-vs-hold, so its hold actions are on an LB layer):

| Button | Action | | Button | Action |
|---|---|---|---|---|
| Left stick | Move | | RB | Throw grenade |
| Right stick | Look | | L3 / R3 | Sprint / flashlight |
| RT / LT | Fire / aim | | D-pad up / down | Binoculars / grenade type |
| A | Jump | | D-pad left / right | Lean left / right |
| B | Crouch (hold) | | Select | Pause menu |
| X | Reload | | Start | Objectives / score |
| Y | Next weapon | | LB + B / X / Y | Prone / use / fire mode |
| | | | LB + right stick | Lean |

## Building

See the project's README.md and BUILDING.md: `tools/build.sh` builds the game, the
launcher and this package; no game files are needed.
