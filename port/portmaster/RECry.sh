#!/bin/bash
# RECry - Far Cry (2004) for PortMaster: the original engine built for Linux / OpenGL ES.
# Structure follows the PortMaster "Basic Launchscript" template.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR=/$directory/ports/recry
CONFDIR="$GAMEDIR/conf"
mkdir -p "$CONFDIR"
cd "$GAMEDIR"

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export XDG_DATA_HOME="$CONFDIR"
export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
$ESUDO chmod +x "$GAMEDIR/farcry.${DEVICE_ARCH}" "$GAMEDIR/recry-launcher.${DEVICE_ARCH}" 2>/dev/null

# 1. Launcher: the first start unpacks the user's GOG installer from
#    installer/ into gamedata/ (PortMaster's innoextract); later starts show
#    the settings. 0 = start the game, 10 = the user left.
if [ -z "$RECRY_SKIP_LAUNCHER" ]; then
  export RECRY_INNO="$controlfolder/innoextract.${DEVICE_ARCH}"
  "$GAMEDIR/recry-launcher.${DEVICE_ARCH}" --root "$GAMEDIR"
  launcher_rc=$?
  if [ "$launcher_rc" != 0 ]; then
    [ "$launcher_rc" != 10 ] && echo "[recry] launcher failed ($launcher_rc)"
    pm_finish
    exit 0
  fi
fi
# Launcher settings; RECRY_* already set in the environment win (testing).
_recry_env=$(declare -p $(compgen -v RECRY_) 2>/dev/null)
[ -f "$CONFDIR/recry.cfg" ] && source "$CONFDIR/recry.cfg"
[ -n "$_recry_env" ] && eval "$_recry_env"

# CPU: the game is CPU-bound. "perf" holds the cores at their rated speed
# (performance governor) while playing; the firmware's settings are restored
# on exit. RECRY_CPU=system leaves them alone.
CPU_SAVED=""
cpu_restore() {
  local entry pol gov max
  for entry in $CPU_SAVED; do
    pol=${entry%%=*}; gov=${entry#*=}; max=${gov#*,}; gov=${gov%%,*}
    echo "$max" | $ESUDO tee "$pol/scaling_max_freq" >/dev/null 2>&1
    echo "$gov" | $ESUDO tee "$pol/scaling_governor" >/dev/null 2>&1
  done
  CPU_SAVED=""
}
cpu_perf() {
  local pol
  for pol in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -w "$pol/scaling_governor" ] || [ -n "$ESUDO" ] || continue
    grep -qw performance "$pol/scaling_available_governors" 2>/dev/null || continue
    CPU_SAVED="$CPU_SAVED $pol=$(cat "$pol/scaling_governor"),$(cat "$pol/scaling_max_freq")"
    echo performance | $ESUDO tee "$pol/scaling_governor" >/dev/null 2>&1
  done
  echo "[recry] cpu: performance governor (restored on exit)"
}
trap cpu_restore EXIT
[ "${RECRY_CPU:-perf}" = perf ] && cpu_perf

# Controller presets (launcher: Controls), all through PortMaster's gptokeyb2:
#   classic - gptokeyb2 -x presents the handheld as a virtual Xbox 360 pad and
#             the game maps the pad itself;
#   modern  - "Modern shooter": gptokeyb2 -c controls/modern.gptk turns the
#             buttons into the game's keyboard/mouse bindings; the game reads
#             the sticks itself (PortMaster's SDL controller mapping).
# gptokeyb2 takes either -x or -c, not both. User presets: put <name>.gptk in
# controls/ and set RECRY_CONTROLS=<name>. Older gptokeyb: classic layout.
RECRY_CONTROLS=${RECRY_CONTROLS:-modern}
[ "$RECRY_CONTROLS" = classic ] || [ -f "$GAMEDIR/controls/$RECRY_CONTROLS.gptk" ] || RECRY_CONTROLS=classic
if [ -n "$GPTOKEYB2" ] && [ "$RECRY_CONTROLS" != classic ]; then
  export FARCRY_PAD_LAYOUT=gptk
  $GPTOKEYB2 "farcry.${DEVICE_ARCH}" -c "$GAMEDIR/controls/$RECRY_CONTROLS.gptk" &
elif [ -n "$GPTOKEYB2" ]; then
  $GPTOKEYB2 "farcry.${DEVICE_ARCH}" -x &
else
  RECRY_CONTROLS=classic
  $GPTOKEYB "farcry.${DEVICE_ARCH}" -c "$GAMEDIR/recry.gptk" &
fi
echo "[recry] controls: $RECRY_CONTROLS"
pm_platform_helper "$GAMEDIR/farcry.${DEVICE_ARCH}"

# 2. The game (run.sh links the game files, applies the settings, starts it).
export DEVICE_NAME DEVICE_CPU DEVICE_RAM DEVICE_ARCH DISPLAY_WIDTH DISPLAY_HEIGHT
bash "$GAMEDIR/run.sh"

cpu_restore
pm_finish
