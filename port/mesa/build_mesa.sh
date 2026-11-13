#!/bin/bash
# Build the Mesa bundled for Panfrost devices (recry/libs.mesa/).
#
# Mesa 25.0.7 (the version BananaOS ships on the RG DS), Panfrost only, EGL +
# GBM + GLES2/3, with 0001-panfrost-tiler-hierarchy-finest-levels.patch: on
# Mali tilers with two hierarchy levels (G52/G31) upstream bins every
# primitive into 256 px tiles once the framebuffer's largest side exceeds
# 496 px, and Far Cry's frames spent ~70 ms of GPU time re-walking them
# (patched: ~10 ms at 640x480 on the RG DS).
#
# Uses the glibc 2.30 Bootlin toolchain (result needs glibc >= 2.29) and a
# private meson/ninja venv. Output: build/mesa/libs.mesa/ (packaged by
# tools/package.sh). Needs network access (sources) on the first run.
set -euo pipefail
W=$(cd -- "$(dirname -- "$0")/../../.." && pwd)   # work tree default; the release build passes the paths below
S=$(cd -- "$(dirname -- "$0")" && pwd)
TC=${TOOLCHAIN_ROOT:-$W/third_party/aarch64--glibc--stable-2020.02-2}
B=${MESA_BUILD_DIR:-$W/build/mesa/work}
P=$B/stage
OUT=${MESA_OUT:-$W/build/mesa/libs.mesa}
mkdir -p "$B" "$P"
cd "$B"

[ -x venv/bin/meson ] || { python3 -m venv venv && venv/bin/pip -q install meson==1.6.1 ninja mako pyyaml packaging; }
export PATH=$B/venv/bin:$PATH

fetch() { [ -f "$2" ] || curl -sSfL -o "$2" "$1"; }
fetch https://zlib.net/fossils/zlib-1.3.1.tar.gz zlib.tar.gz
fetch https://github.com/libexpat/libexpat/releases/download/R_2_6_4/expat-2.6.4.tar.xz expat.tar.xz
fetch https://dri.freedesktop.org/libdrm/libdrm-2.4.124.tar.xz libdrm.tar.xz
[ -d mesa-src ] || git clone -q --depth 1 --branch mesa-25.0.7 https://gitlab.freedesktop.org/mesa/mesa.git mesa-src

cat > cross.ini <<EOF
[binaries]
c = '$TC/bin/aarch64-buildroot-linux-gnu-gcc'
cpp = '$TC/bin/aarch64-buildroot-linux-gnu-g++'
ar = '$TC/bin/aarch64-buildroot-linux-gnu-ar'
strip = '$TC/bin/aarch64-buildroot-linux-gnu-strip'
pkg-config = 'pkg-config'
[properties]
pkg_config_libdir = '$P/lib/pkgconfig:$P/share/pkgconfig'
[built-in options]
c_args = ['-I$P/include', '-I$P/include/libdrm']
cpp_args = ['-I$P/include', '-I$P/include/libdrm']
c_link_args = ['-L$P/lib']
cpp_link_args = ['-L$P/lib']
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'cortex-a55'
endian = 'little'
EOF

# Dependencies, only to build against: the device's own libdrm/expat/zlib are used at run time.
export CC=$TC/bin/aarch64-buildroot-linux-gnu-gcc
[ -f "$P/lib/libz.so" ] || ( rm -rf zlib-1.3.1 && tar xf zlib.tar.gz && cd zlib-1.3.1 && CHOST=aarch64-buildroot-linux-gnu ./configure --prefix="$P" && make -j"$(nproc)" && make install )
[ -f "$P/lib/libexpat.so" ] || ( rm -rf expat-2.6.4 && tar xf expat.tar.xz && cd expat-2.6.4 && ./configure --host=aarch64-buildroot-linux-gnu --prefix="$P" --without-docbook --without-examples --without-tests && make -j"$(nproc)" && make install )
[ -f "$P/lib/libdrm.so" ] || ( rm -rf libdrm-2.4.124 && tar xf libdrm.tar.xz && cd libdrm-2.4.124 && meson setup b --cross-file ../cross.ini --prefix="$P" --libdir=lib \
    -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Dfreedreno=disabled -Detnaviv=disabled \
    -Dexynos=disabled -Domap=disabled -Dtegra=disabled -Dvc4=disabled -Dcairo-tests=disabled -Dvalgrind=disabled -Dman-pages=disabled \
    -Dtests=false -Dudev=false && ninja -C b install )
unset CC

cd mesa-src
git checkout -q -- src/panfrost/lib/pan_util.h
git apply "$S/0001-panfrost-tiler-hierarchy-finest-levels.patch"
rm -rf b
meson setup b --cross-file ../cross.ini --prefix=/usr --libdir=lib --buildtype=release -Db_ndebug=true \
  -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms= -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled \
  -Dopengl=false -Dglx=disabled -Dllvm=disabled -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
  -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled -Dgallium-nine=false -Dgallium-opencl=disabled \
  -Dgallium-rusticl=false -Dtools= -Dexpat=enabled -Dshader-cache=enabled -Dbuild-tests=false -Dosmesa=false
ninja -C b
rm -rf "$B/inst" && DESTDIR="$B/inst" ninja -C b install

rm -rf "$OUT" && mkdir -p "$OUT/gbm"
L=$B/inst/usr/lib
for f in libgallium-25.0.7.so libEGL.so.1 libgbm.so.1 libGLESv2.so.2; do cp -L "$L/$f" "$OUT/"; done
cp -L "$L/gbm/dri_gbm.so" "$OUT/gbm/"
"$TC/bin/aarch64-buildroot-linux-gnu-strip" "$OUT"/*.so* "$OUT"/gbm/*.so
echo "$OUT"
