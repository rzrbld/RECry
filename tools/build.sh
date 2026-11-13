#!/bin/bash
# Build RECry for aarch64 and assemble the PortMaster package (dist/recry.zip).
# Needs no game files. Host: Debian 12 / Ubuntu 22.04+ (x86-64) with the arm64
# SDL2 and SDL2_ttf development packages (see BUILDING.md, or use the CI
# container). Everything downloaded or built goes to .deps/ and build/.
#
#   tools/build.sh            all steps
#   tools/build.sh engine     one step: toolchain | ffmpeg | mesa | engine | launcher | package
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
DEPS=$ROOT/.deps
BUILD=$ROOT/build
JOBS=${JOBS:-$(nproc)}
TOOLCHAIN_NAME=aarch64--glibc--stable-2020.02-2
TOOLCHAIN_URL=https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/$TOOLCHAIN_NAME.tar.bz2
TOOLCHAIN_ROOT=${TOOLCHAIN_ROOT:-$DEPS/$TOOLCHAIN_NAME}
FFMPEG_VER=6.1.2
FFMPEG_URL=https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz
FFMPEG_OUT=$BUILD/ffmpeg/out
# arm64 SDL2 / SDL2_ttf from the host's multiarch packages: only linked
# against; the device provides SDL2 at run time.
ARM_LIBDIR=${ARM_LIBDIR:-/usr/lib/aarch64-linux-gnu}
SDL2_LIB=${SDL2_LIB:-$ARM_LIBDIR/libSDL2-2.0.so.0}
SDL2_TTF_LIB=${SDL2_TTF_LIB:-$ARM_LIBDIR/libSDL2_ttf-2.0.so.0}
SDL2_TTF_H=${SDL2_TTF_H:-/usr/include/SDL2/SDL_ttf.h}
CROSS=$TOOLCHAIN_ROOT/bin/aarch64-buildroot-linux-gnu

step_toolchain() {
  [ -x "$CROSS-gcc" ] && return 0
  mkdir -p "$DEPS"
  echo "== toolchain: $TOOLCHAIN_NAME"
  curl -fsSL "$TOOLCHAIN_URL" | tar -xj -C "$DEPS"
  # The toolchain records its install path; relocate it once.
  [ -x "$TOOLCHAIN_ROOT/relocate-sdk.sh" ] && "$TOOLCHAIN_ROOT/relocate-sdk.sh" >/dev/null || true
}

# Minimal FFmpeg (LGPL, shared, unmodified upstream): file protocol, Bink
# demuxer, Bink video/audio decoders, libswresample.
step_ffmpeg() {
  [ -f "$FFMPEG_OUT/lib/libavcodec.so.60" ] && return 0
  echo "== ffmpeg $FFMPEG_VER"
  mkdir -p "$DEPS" "$BUILD/ffmpeg"
  [ -d "$DEPS/ffmpeg-$FFMPEG_VER" ] || curl -fsSL "$FFMPEG_URL" | tar -xJ -C "$DEPS"
  (
    cd "$BUILD/ffmpeg"
    export PATH="$TOOLCHAIN_ROOT/bin:$PATH"
    # Neutral prefix / compiler names: FFmpeg embeds its configure line.
    "$DEPS/ffmpeg-$FFMPEG_VER/configure" --prefix=/usr --enable-cross-compile --arch=aarch64 --target-os=linux \
      --cross-prefix=aarch64-buildroot-linux-gnu- --extra-cflags="-O2 -mcpu=cortex-a53" \
      --enable-shared --disable-static --enable-pic --disable-programs --disable-doc --disable-network --disable-autodetect \
      --disable-everything --disable-avdevice --disable-avfilter --disable-postproc --disable-swscale \
      --enable-avformat --enable-avcodec --enable-swresample --enable-protocol=file \
      --enable-demuxer=bink --enable-decoder=bink,binkaudio_rdft,binkaudio_dct >/dev/null
    make -j"$JOBS" >/dev/null && make install DESTDIR="$BUILD/ffmpeg/stage" >/dev/null
    rm -rf "$FFMPEG_OUT"; mkdir -p "$FFMPEG_OUT"
    cp -a stage/usr/include stage/usr/lib "$FFMPEG_OUT/"
  )
}

# Mesa 25.0.7, Panfrost only, with the tiler fix (port/mesa/): used on
# Panfrost devices instead of the firmware's Mesa (launcher: GPU driver).
MESA_OUT=$BUILD/mesa/libs.mesa
step_mesa() {
  [ -f "$MESA_OUT/libgallium-25.0.7.so" ] && return 0
  echo "== mesa (panfrost)"
  TOOLCHAIN_ROOT="$TOOLCHAIN_ROOT" MESA_BUILD_DIR="$BUILD/mesa/work" MESA_OUT="$MESA_OUT" "$ROOT/port/mesa/build_mesa.sh" >/dev/null
}

step_engine() {
  echo "== engine"
  cmake -S "$ROOT" -B "$BUILD/engine" -DCMAKE_TOOLCHAIN_FILE="$ROOT/port/cmake/aarch64-bootlin.cmake" \
    -DTOOLCHAIN_ROOT="$TOOLCHAIN_ROOT" -DFFMPEG_ROOT="$FFMPEG_OUT" -DSDL2_LIB="$SDL2_LIB" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
  cmake --build "$BUILD/engine" --target farcry -j"$JOBS"
}

step_launcher() {
  echo "== launcher"
  local inc=$BUILD/launcher/include
  mkdir -p "$inc/SDL2"
  cp "$ROOT"/port/third_party/SDL2/*.h "$inc/SDL2/"; cp "$SDL2_TTF_H" "$inc/SDL2/"
  "$CROSS-gcc" -O2 -Wall -Wno-format-truncation -I"$inc" -D_REENTRANT -o "$BUILD/launcher/recry-launcher.aarch64" \
    "$ROOT/port/launcher/recry_launcher.c" "$SDL2_TTF_LIB" "$SDL2_LIB" -lm -Wl,--allow-shlib-undefined
}

# PortMaster layout: RECry.sh + recry/ ; no game data.
step_package() {
  echo "== package"
  local out=$ROOT/dist/pm r
  r=$out/recry
  rm -rf "$out"; mkdir -p "$r"/{presets,controls,libs.aarch64,fonts,licenses,installer,gamedata}
  cp "$ROOT/port/portmaster/RECry.sh" "$out/"
  "$CROSS-strip" -o "$r/farcry.aarch64" "$BUILD/engine/farcry"
  "$CROSS-strip" -o "$r/recry-launcher.aarch64" "$BUILD/launcher/recry-launcher.aarch64"
  chmod +x "$out/RECry.sh" "$r/farcry.aarch64" "$r/recry-launcher.aarch64"
  cp "$ROOT/port/portmaster/run.sh" "$ROOT/port/portmaster/recry.gptk" "$r/"
  cp "$ROOT/port/presets/handheld.cfg" "$r/presets/"
  for l in libavcodec.so.60 libavformat.so.60 libavutil.so.58 libswresample.so.4; do cp -L "$FFMPEG_OUT/lib/$l" "$r/libs.aarch64/"; done
  cp "$ROOT"/port/portmaster/fonts/*.ttf "$r/fonts/"
  cp "$ROOT"/port/portmaster/controls/*.gptk "$r/controls/"
  cp "$ROOT"/port/portmaster/licenses/* "$r/licenses/"
  mkdir -p "$r/libs.mesa/gbm"
  cp "$MESA_OUT"/*.so* "$r/libs.mesa/"; cp "$MESA_OUT/gbm/dri_gbm.so" "$r/libs.mesa/gbm/"
  cp "$ROOT/port/mesa/MESA-LICENSE.rst" "$r/licenses/Mesa-LICENSE.rst"
  cp "$ROOT/port/mesa/0001-panfrost-tiler-hierarchy-finest-levels.patch" "$r/licenses/Mesa-panfrost-tiler.patch"
  cp "$ROOT/LICENSE" "$r/licenses/CryEngine-SDK.txt"
  cp "$ROOT"/port/portmaster/pm/{README.md,port.json,gameinfo.xml,screenshot.png} "$r/"
  cp "$ROOT/port/portmaster/pm/PUT_THE_GOG_INSTALLER_HERE.txt" "$r/installer/"
  cp "$ROOT/port/portmaster/pm/GAME_FILES_GO_HERE.txt" "$r/gamedata/"
  echo "newest glibc symbol needed:"
  for b in "$r/farcry.aarch64" "$r/recry-launcher.aarch64" "$r"/libs.aarch64/* "$r"/libs.mesa/*.so* "$r"/libs.mesa/gbm/*.so; do
    printf "  %-24s %s\n" "$(basename "$b")" "$("$CROSS-objdump" -T "$b" | grep -o 'GLIBC_[0-9.]*' | sort -V -u | tail -1)"
  done
  mkdir -p "$ROOT/dist"; rm -f "$ROOT/dist/recry.zip"
  (cd "$out" && zip -qr9 "$ROOT/dist/recry.zip" RECry.sh recry)
  echo "package: dist/recry.zip ($(du -h "$ROOT/dist/recry.zip" | cut -f1))"
}

steps=${1:-all}
[ "$steps" = all ] && steps="toolchain ffmpeg mesa engine launcher package"
for s in $steps; do
  case $s in
    toolchain) step_toolchain ;;
    ffmpeg) step_toolchain; step_ffmpeg ;;
    mesa) step_toolchain; step_mesa ;;
    engine) step_toolchain; step_ffmpeg; step_engine ;;
    launcher) step_toolchain; step_launcher ;;
    package) step_package ;;
    *) echo "unknown step: $s" >&2; exit 2 ;;
  esac
done
