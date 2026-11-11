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

# Launcher settings (conf/recry.cfg); the environment wins for testing.
[[ ! -f "$BASE/conf/recry.cfg" ]] || source "$BASE/conf/recry.cfg"

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

# Engine config: the handheld preset over the saved values, then the
# launcher's choices. A changed preset (port update) replaces saved files.
PRESET="$BASE/presets/handheld.cfg"
preset_id=$(cksum < "$PRESET")
if [[ $(cat "$BASE/conf/preset.id" 2>/dev/null) != "$preset_id" ]]; then
    rm -f "$BASE/conf/System.Cfg" "$BASE/conf/system.cfg"
    echo "$preset_id" > "$BASE/conf/preset.id"
fi
for name in System.Cfg system.cfg; do
    [[ -f "$BASE/conf/$name" ]] || cp "$PRESET" "$BASE/conf/$name"
    cp "$BASE/conf/$name" "$RUNTIME/$name"
    { echo "-- Far Cry port preset (applied over saved values)"; grep -E '^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=' "$PRESET"; } >> "$RUNTIME/$name"
done
if [[ ! -f "$BASE/conf/game.cfg" ]]; then
    cfg=$(find_child "$ASSETS" game.cfg)
    [[ -f "$cfg" ]] || fail 'game.cfg missing in gamedata/'
    cp "$cfg" "$BASE/conf/game.cfg"
fi
cp "$BASE/conf/game.cfg" "$RUNTIME/game.cfg"
for f in "$BASE/conf"/profiles\\*; do [[ ! -f "$f" ]] || cp "$f" "$RUNTIME/"; done

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
fi
export FARCRY_SCALE=${RECRY_SCALE:-fit}

# ---------------------------------------------------------------- options
# Trade-offs for slow handhelds: auto picks by device class (PortMaster's
# DEVICE_CPU / DEVICE_RAM), on = simplified, off = stock Far Cry.
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
export HANDHELD_PHYSICS=${HANDHELD_PHYSICS:-$(option "${RECRY_PHYSICS:-auto}" 1 0 0)}
export HANDHELD_EFFECTS=${HANDHELD_EFFECTS:-$(option "${RECRY_EFFECTS:-auto}" 1 0 0)}
export HANDHELD_VIEW=${HANDHELD_VIEW:-$(option "${RECRY_VIEW:-auto}" 1 1 0)}
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
    if [[ $HANDHELD_PHYSICS != 0 ]]; then
        echo 'p_max_substeps = "2"'; echo 'p_max_contacts = "64"'; echo 'p_max_plane_contacts = "4"'
        echo 'p_max_MC_iters = "2000"'; echo 'p_max_LCPCG_iters = "3"'
        echo 'p_max_LCPCG_subiters = "40"'; echo 'p_max_LCPCG_microiters = "4000"'
    else
        echo 'p_max_substeps = "5"'; echo 'p_max_contacts = "150"'; echo 'p_max_plane_contacts = "8"'
        echo 'p_max_MC_iters = "6000"'; echo 'p_max_LCPCG_iters = "5"'
        echo 'p_max_LCPCG_subiters = "120"'; echo 'p_max_LCPCG_microiters = "12000"'
    fi
    if [[ $HANDHELD_EFFECTS != 0 ]]; then
        echo 'e_particles_lod = "0.25"'; echo 'e_particles_max_count = "120"'
        echo 'e_decals_life_time_scale = "0.5"'; echo 'ca_EnableDecals = "0"'
        echo 'e_flocks = "0"'; echo 'es_EnableCloth = "0"'; echo 'cl_weapon_fx = "1"'
        echo 'r_EnvTexUpdateInterval = "0.1"'      # reflections re-render the scene at 10 Hz, not every frame
        echo 'r_WaterReflections = "0"'            # the ocean reflection re-renders the scene
    else
        echo 'e_particles_lod = "1"'; echo 'e_decals_life_time_scale = "1"'; echo 'ca_EnableDecals = "1"'
        echo 'e_flocks = "1"'; echo 'es_EnableCloth = "1"'; echo 'cl_weapon_fx = "2"'
        echo 'r_EnvTexUpdateInterval = "0.001"'; echo 'r_WaterReflections = "1"'
    fi
    if [[ $HANDHELD_VIEW != 0 ]]; then
        echo 'e_obj_view_dist_ratio = "36"'
        echo 'e_terrain_occlusion_culling_precision = "0.3"'
    fi
    echo "r_Width = \"$RW\""; echo "r_Height = \"$RH\""; echo 'r_Fullscreen = "1"'
    echo "g_maxfps = \"${RECRY_LIMIT:-30}\""                 # 30 / 60 / 0 = uncapped
    # FPS counter: 1 = small counter (FARCRY_SHOW_FPS), 2 = the engine's own statistics
    if [[ ${RECRY_FPS:-0} == 2 ]]; then echo 'r_DisplayInfo = "1"'; else echo 'r_DisplayInfo = "0"'; fi
}
for name in System.Cfg system.cfg; do
    { echo "-- Far Cry port settings: physics=$HANDHELD_PHYSICS effects=$HANDHELD_EFFECTS view=$HANDHELD_VIEW"; handheld_cvars; } >> "$RUNTIME/$name"
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
mkdir -p "$BASE/logs/snaps"; export FARCRY_SNAP_DIR=${FARCRY_SNAP_DIR:-$BASE/logs/snaps} FARCRY_CAPTURE_DIR=${FARCRY_CAPTURE_DIR:-$BASE/logs/snaps}
export MALLOC_ARENA_MAX=${MALLOC_ARENA_MAX:-2}
export FARCRY_AUDIO_RATE=${FARCRY_AUDIO_RATE:-22050}
export FARCRY_AUDIO_MONO_3D=${FARCRY_AUDIO_MONO_3D:-1}
export FARCRY_WORLD_TEXTURE_MAX=${FARCRY_WORLD_TEXTURE_MAX:-192}
export FARCRY_GLES_QUEUE_FRAMES=${FARCRY_GLES_QUEUE_FRAMES:-2}
# DXT textures are converted once on spare cores and cached here. The cache
# is made from the user's own game files: never redistribute it.
export FARCRY_TEXCACHE=${FARCRY_TEXCACHE:-$BASE/cache/textures}
export FARCRY_TEXCONV_THREADS=${FARCRY_TEXCONV_THREADS:-3}
export FARCRY_PAD_DEADZONE=${FARCRY_PAD_DEADZONE:-0.22}
export FARCRY_PAD_LOOK_SCALE=${FARCRY_PAD_LOOK_SCALE:-180}
# Video, audio and the GL driver come from the firmware through SDL.
echo "Device: ${DEVICE_NAME:-?} cpu=${DEVICE_CPU:-?} ram=${DEVICE_RAM:-?}GB class=$DEVCLASS screen=${SW}x${SH} render=${RW}x${RH} scale=$FARCRY_SCALE"
echo "Options: physics=$HANDHELD_PHYSICS effects=$HANDHELD_EFFECTS view=$HANDHELD_VIEW text=$FARCRY_UI_TEXT_SCALE limit=${RECRY_LIMIT:-30} fps=${RECRY_FPS:-0}"
echo "Video driver: ${SDL_VIDEODRIVER:-<SDL default>}  audio driver: ${SDL_AUDIODRIVER:-<SDL default>}"
"$FARCRY_BIN" "$@" &
child=$!
[[ -z ${FARCRY_PIDFILE:-} ]] || echo "$child" > "$FARCRY_PIDFILE"
trap 'kill -TERM "$child" 2>/dev/null; wait "$child" 2>/dev/null' TERM INT
wait "$child"
