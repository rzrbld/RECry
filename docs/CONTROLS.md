# RECry controls

RECry has two gamepad layouts. Pick one in the launcher under **Controls**.
Both go through PortMaster's gptokeyb2, so they work with each handheld's own buttons.
Button names below follow the Xbox layout PortMaster uses: A is the bottom face button,
B the right one, X the left one and Y the top one.

- **Classic**: gptokeyb2 presents the handheld as a virtual Xbox 360 pad and the game
  binds the buttons itself.
- **Modern shooter**: gptokeyb2 turns the buttons into the game's own keyboard and mouse
  bindings, using `recry/controls/modern.gptk`. The game still reads both sticks
  itself. The layout is based on the Steam Input layout "Far Cry 1 custom layout to
  match modern shooters".

Stick speed is set in the launcher (**Look speed**, **Pointer speed**) and works the
same in both layouts.

## Classic

| Button | Action |
|---|---|
| Left stick | Move |
| Right stick | Look |
| RT | Fire |
| LT | Aim / zoom |
| A | Jump |
| B | Use, enter vehicles |
| X | Reload |
| Y | Next weapon |
| D-pad left | Previous weapon |
| D-pad up | Fire mode |
| D-pad down | Throw grenade |
| LB | Lean left |
| RB | Lean right |
| L3 (press left stick) | Sprint |
| R3 (press right stick) | Crouch / prone |
| Select | Objectives / score |
| Start | Pause menu |

## Modern shooter

| Button | Action |
|---|---|
| Left stick | Move |
| Right stick | Look |
| RT | Fire |
| LT | Aim / zoom |
| A | Jump |
| B | Crouch (hold) |
| X | Reload |
| Y | Next weapon |
| RB | Throw grenade |
| LB | Hold for the second layer (below) |
| L3 (press left stick) | Sprint (hold) |
| R3 (press right stick) | Flashlight |
| D-pad up | Binoculars |
| D-pad down | Use, enter vehicles |
| D-pad left | Lean left |
| D-pad right | Lean right |
| Select | Pause menu |
| Start | Objectives / score |

gptokeyb2 cannot tell a tap from a hold, so the layout's hold actions are on a second
layer while **LB** is held:

| Hold LB + | Action |
|---|---|
| B | Prone |
| Y | Fire mode |
| D-pad down | Switch grenade type |
| Right stick left / right | Lean left / right |

## Menus and cutscenes (both layouts)

| Button | Action |
|---|---|
| Either stick | Move the pointer |
| A | Click |
| A, B or Start | Skip movies and the cutscenes Far Cry lets you skip |

## Your own layout

Copy `recry/controls/modern.gptk` to `recry/controls/<name>.gptk` and change the buttons;
any gptokeyb2 key name works (see the gptokeyb2 README). Then set
`RECRY_CONTROLS=<name>` in `recry/conf/recry.cfg`. The game's default keyboard bindings
are listed under Options > Controls in the game.

Firmware with the older gptokeyb (no gptokeyb2) always uses the Classic layout.
