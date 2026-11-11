# Building RECry

RECry builds on an x86-64 Linux host for aarch64 devices. No game files are needed: the
build uses only this repository, a Bootlin toolchain, FFmpeg's source and the arm64 SDL2
development packages. The result is `dist/recry.zip`, ready to unzip on a PortMaster
device.

## 1. Host

Debian 12 (bookworm) or Ubuntu 22.04+ on x86-64, with arm64 multiarch for the SDL2 /
SDL2_ttf link libraries:

```sh
sudo dpkg --add-architecture arm64
sudo apt-get update
sudo apt-get install -y ca-certificates curl git make cmake bzip2 xz-utils zip \
    libsdl2-dev:arm64 libsdl2-ttf-dev:arm64
```

On other distributions, run the build in a `debian:bookworm` container (as the CI does):

```sh
docker run --rm -it -v "$PWD":/src -w /src debian:bookworm bash -c '
  dpkg --add-architecture arm64 && apt-get update &&
  apt-get install -y --no-install-recommends ca-certificates curl git make cmake bzip2 xz-utils zip \
      libsdl2-dev:arm64 libsdl2-ttf-dev:arm64 &&
  tools/build.sh'
```

SDL2 and SDL2_ttf are only linked against. At run time the device's own SDL2 provides
display, OpenGL ES, sound and input, so no SDL library is shipped.

## 2. One command

```sh
tools/build.sh
```

| Path | Contents |
|---|---|
| `.deps/` | Downloaded toolchain and FFmpeg source |
| `build/` | Build trees |
| `dist/pm/` | The unpacked package |
| `dist/recry.zip` | The package |

Each step can be run on its own:

```sh
tools/build.sh toolchain   # Bootlin aarch64--glibc--stable-2020.02-2 -> .deps/
tools/build.sh ffmpeg      # minimal FFmpeg 6.1.2 -> build/ffmpeg/out
tools/build.sh engine      # the game -> build/engine/farcry
tools/build.sh launcher    # the launcher -> build/launcher/recry-launcher.aarch64
tools/build.sh package     # dist/pm and dist/recry.zip
```

Environment overrides:

| Variable | Default | |
|---|---|---|
| `JOBS` | `nproc` | parallel jobs |
| `TOOLCHAIN_ROOT` | `.deps/aarch64--glibc--stable-2020.02-2` | use an existing toolchain |
| `ARM_LIBDIR` | `/usr/lib/aarch64-linux-gnu` | arm64 libraries |
| `SDL2_LIB`, `SDL2_TTF_LIB` | in `ARM_LIBDIR` | link libraries |
| `SDL2_TTF_H` | `/usr/include/SDL2/SDL_ttf.h` | SDL2_ttf header |

## 3. The steps by hand

### Toolchain

The release builds use Bootlin's `aarch64--glibc--stable-2020.02-2` (GCC 8.4, glibc 2.30).
With it the binaries need nothing newer than glibc 2.29, so they run on older firmware
too (ArkOS-era and newer).

```sh
curl -fsSL https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/aarch64--glibc--stable-2020.02-2.tar.bz2 | tar -xj -C .deps
export TOOLCHAIN_ROOT=$PWD/.deps/aarch64--glibc--stable-2020.02-2
```

### FFmpeg (Bink cutscenes)

A minimal, unmodified, shared LGPL build with only what Far Cry's videos need: the file
protocol, the Bink demuxer, the Bink video and audio decoders and libswresample.

```sh
curl -fsSL https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz | tar -xJ -C .deps
mkdir -p build/ffmpeg && cd build/ffmpeg
PATH=$TOOLCHAIN_ROOT/bin:$PATH ../../.deps/ffmpeg-6.1.2/configure --prefix=/usr \
  --enable-cross-compile --arch=aarch64 --target-os=linux --cross-prefix=aarch64-buildroot-linux-gnu- \
  --extra-cflags="-O2 -mcpu=cortex-a53" --enable-shared --disable-static --enable-pic \
  --disable-programs --disable-doc --disable-network --disable-autodetect --disable-everything \
  --disable-avdevice --disable-avfilter --disable-postproc --disable-swscale \
  --enable-avformat --enable-avcodec --enable-swresample --enable-protocol=file \
  --enable-demuxer=bink --enable-decoder=bink,binkaudio_rdft,binkaudio_dct
PATH=$TOOLCHAIN_ROOT/bin:$PATH make -j"$(nproc)" && make install DESTDIR=$PWD/stage
mkdir -p out && cp -a stage/usr/include stage/usr/lib out/
cd ../..
```

### The game

```sh
cmake -S . -B build/engine -DCMAKE_TOOLCHAIN_FILE=port/cmake/aarch64-bootlin.cmake \
  -DTOOLCHAIN_ROOT=$TOOLCHAIN_ROOT -DFFMPEG_ROOT=$PWD/build/ffmpeg/out \
  -DSDL2_LIB=/usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/engine --target farcry -j"$(nproc)"
```

Notes:
- libstdc++ and libgcc are linked statically.
- `--allow-shlib-undefined` lets the build link against the host's newer arm64 libSDL2.
  The binary itself needs only the toolchain's glibc.
- The rpath is `$ORIGIN/libs.aarch64`, where the package keeps FFmpeg.

### The launcher

One C file using SDL2 and SDL2_ttf:

```sh
mkdir -p build/launcher/include/SDL2
cp port/third_party/SDL2/*.h /usr/include/SDL2/SDL_ttf.h build/launcher/include/SDL2/
$TOOLCHAIN_ROOT/bin/aarch64-buildroot-linux-gnu-gcc -O2 -Ibuild/launcher/include -D_REENTRANT \
  -o build/launcher/recry-launcher.aarch64 port/launcher/recry_launcher.c \
  /usr/lib/aarch64-linux-gnu/libSDL2_ttf-2.0.so.0 /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0 \
  -lm -Wl,--allow-shlib-undefined
```

### The package

`tools/build.sh package` strips both binaries and assembles the PortMaster layout:

```
RECry.sh                    launch script (PortMaster template)
recry/
  farcry.aarch64            the game
  recry-launcher.aarch64    the launcher
  run.sh                    links the game files, applies the settings, starts the game
  recry.gptk                gptokeyb fallback mapping (hotkey exit only)
  controls/*.gptk           controller presets for gptokeyb2 (classic, modern)
  presets/handheld.cfg      engine settings for handhelds
  libs.aarch64/             FFmpeg (libavcodec, libavformat, libavutil, libswresample)
  fonts/                    launcher fonts (Noto Sans, OFL)
  licenses/                 all licenses
  installer/                the user puts the GOG installer here
  gamedata/                 the launcher unpacks the game here
  README.md port.json gameinfo.xml screenshot.png
```

## 4. Running on a device

1. Copy `dist/pm/RECry.sh` into the Ports folder and `dist/pm/recry` into `ports`.
2. Add the GOG installer to `ports/recry/installer/` (see README.md).

Useful environment variables for testing over SSH:

| Variable | Effect |
|---|---|
| `RECRY_SKIP_LAUNCHER=1` | start the game directly |
| `FARCRY_MAP=<level>` with `FARCRY_START_GAMEPLAY=1` | load a level |
| `RECRY_RES=WxH` | render resolution |
| `HANDHELD_PHYSICS`, `HANDHELD_EFFECTS`, `HANDHELD_VIEW` (`0` or `1`) | force the trade-offs |

## Project layout

| Path | |
|---|---|
| `Cry*/`, `RenderDll/` | Engine source (CryEngine 1 source SDK) with the port's changes |
| `RenderDll/XRenderGLES3/` | Native OpenGL ES 3 renderer |
| `port/main`, `port/platform` | Linux entry point and platform layer: SDL2 sound, Bink via FFmpeg, timing |
| `port/shim`, `port/compat`, `port/inc_all`, `port/gles3_compat` | Windows / MSVC compatibility headers |
| `port/third_party` | SDL2, GLES3/KHR headers, stb_vorbis |
| `third_party/etcpak` | ETC2 texture encoder (BSD-3) |
| `port/launcher` | The launcher |
| `port/portmaster` | Launch script, run script, metadata, fonts, licenses |
| `port/presets` | Engine settings for handhelds |
| `tools/build.sh` | Build and package |
