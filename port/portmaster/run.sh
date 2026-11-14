#!/bin/bash
# RECry (PortMaster): prepares the game's runtime folder and starts it.
# Called by RECry.sh after the launcher. Everything persistent lives in
# this port folder: gamedata/ (the user's game, unpacked by the launcher),
# conf/ (settings, engine config, saves), cache/ (converted textures), logs/.
set -e
BASE=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$BASE/conf" "$BASE/logs"
fail() { echo "RECry: $*" >&2; command -v pm_message >/dev/null && pm_message "$*"; exit 2; }
FARCRY_BIN=${FARCRY_BIN:-$BASE/farcry.${DEVICE_ARCH:-aarch64}}
[[ -x "$FARCRY_BIN" ]] || fail "Executable missing: $FARCRY_BIN"
ASSETS=${FARCRY_ASSETS:-$BASE/gamedata}
[[ -d "$ASSETS" ]] || fail "No game files: start the port from the menu to unpack your GOG installer."
ASSETS=$(cd -- "$ASSETS" && pwd)
# Windows file names in any case (the GOG installer uses FCData, Levels, ...).
find_child() { find "$1" -mindepth 1 -maxdepth 1 -iname "$2" -print -quit; }
for name in fcdata levels shaders languages; do
    [[ -d "$(find_child "$ASSETS" "$name")" ]] || fail "Game folder missing in gamedata/: $name"
done
fcdata=$(find_child "$ASSETS" fcdata)
for pak in scripts.pak objects.pak textures.pak sounds.pak; do
    [[ -n $(find_child "$fcdata" "$pak") ]] || fail "Game archive missing: gamedata/FCData/$pak"
done

# Launcher settings (conf/recry.cfg); RECRY_* already in the environment win (testing).
_recry_env=$(declare -p $(compgen -v RECRY_) 2>/dev/null)
[[ ! -f "$BASE/conf/recry.cfg" ]] || source "$BASE/conf/recry.cfg"
[[ -z $_recry_env ]] || eval "$_recry_env"

# The engine wants one folder with its data and configs. SD cards formatted
# FAT32/exFAT cannot hold symlinks, so this folder of links lives in RAM; it
# holds no data of its own and is removed on exit.
RUNTIME=$(mktemp -d /tmp/recry.XXXXXX)
cleanup() {
    rc=$?
    cp -f "$RUNTIME/log.txt" "$BASE/logs/engine.log" 2>/dev/null || true
    for f in "$RUNTIME"/*.cfg "$RUNTIME"/profiles\\*; do
        [[ -f "$f" ]] && cp -f "$f" "$BASE/conf/"
    done
    rm -rf -- "$RUNTIME"
    exit "$rc"
}
trap cleanup EXIT
mkdir -p "$BASE/conf/profiles" "$BASE/conf/tmp"
ln -s "$BASE/conf/profiles" "$RUNTIME/profiles"     # saves and player profiles
ln -s "$BASE/conf/tmp" "$RUNTIME/tmp"
mkdir -p "$RUNTIME/fcdata"
for archive in "$fcdata"/*; do ln -s "$archive" "$RUNTIME/fcdata/${archive##*/}"; done
for name in levels shaders languages devmode.lua; do
    path=$(find_child "$ASSETS" "$name")
    [[ -n "$path" ]] && ln -s "$path" "$RUNTIME/$name"
done

# Engine config: one config, the game's own System.Cfg. The handheld preset
# is written only when there is none (first start, port update with a
# changed preset, or the launcher's "Restore default game settings"); after
# that the in-game Options menu owns it. The launcher adds only what the game
# cannot set itself (resolution, frame limit, FPS counter), replacing its own
# previous lines.
PRESET="$BASE/presets/handheld.cfg"
preset_id=$(cksum < "$PRESET")
if [[ $(cat "$BASE/conf/preset.id" 2>/dev/null) != "$preset_id" ]]; then
    rm -f "$BASE/conf/System.Cfg" "$BASE/conf/system.cfg"
    echo "$preset_id" > "$BASE/conf/preset.id"
fi
for name in System.Cfg system.cfg; do
    [[ -f "$BASE/conf/$name" ]] || cp "$PRESET" "$BASE/conf/$name"
    grep -v -E '^[[:space:]]*(r_Width|r_Height|r_Fullscreen|g_maxfps|r_DisplayInfo)[[:space:]]*=|^-- Far Cry port (settings|preset)' "$BASE/conf/$name" > "$RUNTIME/$name"
done
if [[ ! -f "$BASE/conf/game.cfg" ]]; then
    cfg=$(find_child "$ASSETS" game.cfg)
    [[ -f "$cfg" ]] || fail 'game.cfg missing in gamedata/'
    cp "$cfg" "$BASE/conf/game.cfg"
fi
cp "$BASE/conf/game.cfg" "$RUNTIME/game.cfg"
for f in "$BASE/conf"/profiles\\*; do [[ ! -f "$f" ]] || cp "$f" "$RUNTIME/"; done

# ---------------------------------------------------------------- GPU driver
# Panfrost (Mesa's open-source Mali driver): the package bundles Mesa 25.0.7
# with a fixed tiler setup in libs.mesa/ (see the comment at the resolution
# below). RECRY_MESA=patched (default) uses it, system keeps the firmware's
# Mesa and caps the auto resolution instead. Other GPUs/drivers: untouched.
PANFROST=0; PANFROST_MESA=none
if [[ -d /sys/bus/platform/drivers/panfrost ]] && ls /sys/bus/platform/drivers/panfrost/ 2>/dev/null | grep -q '\.gpu$'; then
    PANFROST=1; PANFROST_MESA=system
    if [[ ${RECRY_MESA:-patched} != system && -f $BASE/libs.mesa/libgallium-25.0.7.so ]]; then
        PANFROST_MESA=patched
        export LD_LIBRARY_PATH="$BASE/libs.mesa${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        export GBM_BACKENDS_PATH="$BASE/libs.mesa/gbm"
    fi
fi

# ---------------------------------------------------------------- display
# Render resolution: auto = the screen itself up to 480 lines, else 480 lines
# at the screen's shape. The picture is scaled to the screen (fit keeps the
# aspect ratio with bars, stretch fills it).
SW=${DISPLAY_WIDTH:-640}; SH=${DISPLAY_HEIGHT:-480}
if (( SW < SH )); then t=$SW; SW=$SH; SH=$t; fi
res=${RECRY_RES:-auto}
if [[ $res =~ ^([0-9]+)x([0-9]+)$ ]]; then RW=${BASH_REMATCH[1]}; RH=${BASH_REMATCH[2]}
else
    RH=$(( SH <= 480 ? SH : 480 )); RW=$(( (RH * SW / SH + 1) & ~1 ))
    # Panfrost (Mesa) on Mali GPUs whose tiler has only two hierarchy levels
    # (e.g. Mali-G52 in the RK3566/RK3568) bins every primitive into tiles of
    # 256 px or more once the render target's largest side exceeds 496 px
    # (pan_select_tiler_hierarchy_mask), and each 16x16 tile then re-reads
    # all of them. RG DS, river: 640x480 69.8 ms GPU per frame, 512x384 62.0,
    # 496x372 20.9. Auto keeps the largest side at 496 there and scales up.
    if [[ $PANFROST == 1 && $PANFROST_MESA != patched ]]; then
        if (( RW > 496 || RH > 496 )); then
            if (( RW >= RH )); then RH=$(( (496 * RH / RW) & ~1 )); RW=496
            else RW=$(( (496 * RW / RH) & ~1 )); RH=496; fi
            PANFROST_CAP=1
        fi
    fi
fi
export FARCRY_SCALE=${RECRY_SCALE:-fit}

# ---------------------------------------------------------------- options
# Trade-offs for handhelds: auto picks by device class (PortMaster's
# DEVICE_CPU / DEVICE_RAM): simplified on every handheld SoC (H700, RK3326,
# RK3566/RK3568, ...; the game is CPU-bound on all of them), stock Far Cry on
# desktop-class devices. on = simplified, off = stock Far Cry.
device_class() {
    local cpu=${DEVICE_CPU,,}
    case "$cpu" in
        h700|a133*|rk3326|rk3128|sun50i*) echo weak; return ;;
        rk3566|rk3568|rk3399|s922x|a311d|t618|t820) echo mid; return ;;
        sd865|sm8250|sd8*|sm8*|x86*|*ryzen*|*intel*) echo strong; return ;;
    esac
    [[ ${DEVICE_RAM:-0} =~ ^[0-9]+$ && ${DEVICE_RAM:-0} -le 1 ]] && { echo weak; return; }
    echo mid
}
DEVCLASS=$(device_class)
option() { # option <on|off|auto> <weak> <mid> <strong>
    case "${1,,}" in on|1|yes) echo 1; return ;; off|0|no) echo 0; return ;; esac
    case "$DEVCLASS" in weak) echo "$2" ;; mid) echo "$3" ;; *) echo "$4" ;; esac
}
export HANDHELD_PHYSICS=${HANDHELD_PHYSICS:-$(option auto 1 1 0)}
export HANDHELD_EFFECTS=${HANDHELD_EFFECTS:-$(option auto 1 1 0)}
export HANDHELD_VIEW=${HANDHELD_VIEW:-$(option auto 1 1 0)}
if [[ $HANDHELD_PHYSICS != 0 ]]; then
    # Ropes and ragdolls settle sooner (Pier: ropes 4.5 %, Steam: ragdolls ~7 % of frame time).
    export FARCRY_ROPE_EMIN_SCALE=${FARCRY_ROPE_EMIN_SCALE:-100}
    export FARCRY_RAGDOLL_EMIN_SCALE=${FARCRY_RAGDOLL_EMIN_SCALE:-4} FARCRY_RAGDOLL_MAX_AWAKE=${FARCRY_RAGDOLL_MAX_AWAKE:-6}
fi
# Particles fade within 1.5 m of the camera (smoke in your face).
if [[ $HANDHELD_EFFECTS != 0 ]]; then export FARCRY_PARTICLE_NEAR_FADE=${FARCRY_PARTICLE_NEAR_FADE:-1.5}; fi
# Characters keep a longer draw distance than props; terrain-hidden objects
# are re-tested every third frame.
if [[ $HANDHELD_VIEW != 0 ]]; then export FARCRY_CHAR_VIEW_RATIO=${FARCRY_CHAR_VIEW_RATIO:-46} FARCRY_TERRAIN_OCCL_HOLD=${FARCRY_TERRAIN_OCCL_HOLD:-2}; fi
handheld_cvars() {
    echo "r_Width = \"$RW\""; echo "r_Height = \"$RH\""; echo 'r_Fullscreen = "1"'
    echo "g_maxfps = \"${RECRY_LIMIT:-30}\""                 # 30 / 60 / 0 = uncapped
    # FPS counter: 1 = small counter (FARCRY_SHOW_FPS), 2 = the engine's own statistics
    if [[ ${RECRY_FPS:-0} == 2 ]]; then echo 'r_DisplayInfo = "1"'; else echo 'r_DisplayInfo = "0"'; fi
}
for name in System.Cfg system.cfg; do
    { echo "-- Far Cry port settings (launcher)"; handheld_cvars; } >> "$RUNTIME/$name"
done

# Menu text size: auto = bigger on small screens (1.3x at 480 lines and
# below, 1.15x up to 600); HUD text grows less (fixed HUD positions).
ui=${RECRY_TEXT:-auto}
if [[ $ui == auto ]]; then
    if (( RH <= 480 )); then ui_scale=1.3; elif (( RH <= 600 )); then ui_scale=1.15; else ui_scale=1.0; fi
else ui_scale=$(awk -v p="$ui" 'BEGIN{printf "%.2f", p/100}'); fi
export FARCRY_UI_TEXT_SCALE=${FARCRY_UI_TEXT_SCALE:-$ui_scale}
[[ ${RECRY_FPS:-0} == 1 ]] && export FARCRY_SHOW_FPS=1
export FARCRY_HUD_TEXT_SCALE=${FARCRY_HUD_TEXT_SCALE:-$(awk -v s="$FARCRY_UI_TEXT_SCALE" 'BEGIN{printf "%.2f", 1+(s-1)*0.625}')}

# ---------------------------------------------------------------- engine
export FARCRY_DATA="$RUNTIME" FARCRY_GLES=1
# Renderer (launcher): native = the native GLES3 backend running the game's
# own shader programs (bump/specular materials, water reflections); classic =
# the older fixed-function renderer (bump/water cvars: presets/handheld.cfg).
if [[ ${RECRY_RENDERER:-native} != classic ]]; then
  export FARCRY_RENDERER=${FARCRY_RENDERER:-native}
fi
mkdir -p "$BASE/logs/snaps"; export FARCRY_SNAP_DIR=${FARCRY_SNAP_DIR:-$BASE/logs/snaps} FARCRY_CAPTURE_DIR=${FARCRY_CAPTURE_DIR:-$BASE/logs/snaps}
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}
export FARCRY_AUDIO_RATE=${FARCRY_AUDIO_RATE:-22050}
export FARCRY_AUDIO_MONO_3D=${FARCRY_AUDIO_MONO_3D:-1}
export FARCRY_GLES_QUEUE_FRAMES=${FARCRY_GLES_QUEUE_FRAMES:-2}
# DXT textures are converted once on spare cores and cached here. The cache
# is made from the user's own game files: never redistribute it.
export FARCRY_TEXCACHE=${FARCRY_TEXCACHE:-$BASE/cache/textures}
export FARCRY_TEXCONV_THREADS=${FARCRY_TEXCONV_THREADS:-3}
export FARCRY_PAD_DEADZONE=${FARCRY_PAD_DEADZONE:-0.22}
# Stick speeds from the launcher (percent; 30 = the tuned default):
# look 30 % = 180 degrees/s, menu pointer 30 % = 700 virtual pixels/s.
look=${RECRY_LOOK:-30}; pointer=${RECRY_POINTER:-30}
export FARCRY_PAD_LOOK_SCALE=${FARCRY_PAD_LOOK_SCALE:-$(( look * 6 ))}
export FARCRY_PAD_CURSOR_SPEED=${FARCRY_PAD_CURSOR_SPEED:-$(( pointer * 70 / 3 ))}
# Video, audio and the GL driver come from the firmware through SDL.
echo "Device: ${DEVICE_NAME:-?} cpu=${DEVICE_CPU:-?} ram=${DEVICE_RAM:-?}GB class=$DEVCLASS screen=${SW}x${SH} render=${RW}x${RH} scale=$FARCRY_SCALE${PANFROST_CAP:+ (Panfrost tiler cap)}"
echo "Options: physics=$HANDHELD_PHYSICS effects=$HANDHELD_EFFECTS view=$HANDHELD_VIEW text=$FARCRY_UI_TEXT_SCALE limit=${RECRY_LIMIT:-30} fps=${RECRY_FPS:-0}"
echo "Video driver: ${SDL_VIDEODRIVER:-<SDL default>}  audio driver: ${SDL_AUDIODRIVER:-<SDL default>}  GPU driver: $( [[ $PANFROST == 1 ]] && echo "Panfrost, $PANFROST_MESA Mesa" || echo "system" )"
"$FARCRY_BIN" "$@" &
child=$!
[[ -z ${FARCRY_PIDFILE:-} ]] || echo "$child" > "$FARCRY_PIDFILE"
trap 'kill -TERM "$child" 2>/dev/null; wait "$child" 2>/dev/null' TERM INT
wait "$child"
