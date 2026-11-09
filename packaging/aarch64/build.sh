#!/usr/bin/env bash
# Ninja Gaiden II for 64-bit ARM Linux, cross-compiled on an x86-64 Linux host.
#
#   packaging/aarch64/build.sh
#
# Follows packaging/windows/build.sh: needs a native build first (out/build/linux-amd64-release,
# whose code generator output and shader cache are used, as the shader translator and DXC run
# on x86-64 only) and the native SDK (thirdparty/rexglue-sdk/out/install/linux-amd64). Builds
# the SDK for aarch64 (thirdparty/rexglue-sdk/out/install/linux-arm64), a small FFmpeg with the
# movies' codecs as the AppImage's (packaging/appimage/Dockerfile), the game
# (cmake/toolchains/linux-aarch64-clang.cmake) and collects the program and the libraries it
# needs that a system may lack in out/aarch64-dist.
#
# Needs: thirdparty/ffmpeg-n9.0.2 (github.com/FFmpeg/FFmpeg, tag n9.0.2), cmake, ninja, clang,
# lld, llvm-ar, gpgv and the Ubuntu archive keyring (/usr/share/keyrings, for the sysroot).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
toolchain=$repo/cmake/toolchains/linux-aarch64-clang.cmake
sdk=$repo/thirdparty/rexglue-sdk
sysroot=$repo/out/aarch64-sysroot
ffmpeg_src=$repo/thirdparty/ffmpeg-n9.0.2
logs=$repo/out/logs
dist=$repo/out/aarch64-dist
jobs=${JOBS:-$(nproc)}
mkdir -p "$logs"

[ -f "$repo/out/build/linux-amd64-release/shaders/ng2_shader_cache.cpp" ] ||
    { echo "No native build: build out/build/linux-amd64-release first (README)." >&2; exit 1; }
# The sysroot: what the SDK's own linux-arm64 build installs (its _build-platform.yaml) and the
# AppImage's Dockerfile, as Ubuntu 24.04 arm64 packages.
[ -f "$sysroot/usr/lib/aarch64-linux-gnu/libc.so.6" ] ||
    python3 -I "$here/fetch-sysroot.py" "$repo/out/noble-dl" "$sysroot" \
        libc6-dev libstdc++-13-dev libgcc-13-dev linux-libc-dev libzstd-dev \
        libx11-dev libx11-xcb-dev libxcb1-dev libxext-dev libxrandr-dev libxcursor-dev \
        libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev \
        wayland-protocols libdecor-0-dev libegl-dev libgbm-dev libdrm-dev libvulkan-dev \
        libpulse-dev libpipewire-0.3-dev libasound2-dev libdbus-1-dev libudev-dev \
        libusb-1.0-0-dev >"$logs/sysroot-packages.txt"

# The SDK for aarch64, brought up to date every time (cheap when nothing changed).
if [ ! -f "$sdk/out/build/linux-arm64/build.ninja" ]; then
    cmake -S "$sdk" -B "$sdk/out/build/linux-arm64" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=23 \
        -DCMAKE_INSTALL_PREFIX="$sdk/out/install/linux-arm64" -DREXGLUE_ENABLE_TRACY=OFF \
        >"$logs/sdk-arm64-configure.log"
fi
cmake --build "$sdk/out/build/linux-arm64" -j"$jobs" >"$logs/sdk-arm64-build.log"
cmake --install "$sdk/out/build/linux-arm64" >/dev/null

# FFmpeg for the game's movies, configured as the AppImage's (an LGPL shared build with only the
# ASF demuxer and the WMV/VC-1 and WMA decoders), installed into the sysroot for the game to
# link against and bundled next to it.
if [ ! -f "$sysroot/usr/lib/libavformat.so" ]; then
    target="--target=aarch64-linux-gnu --sysroot=$sysroot"
    build=$repo/out/build/ffmpeg-aarch64
    rm -rf "$build" && mkdir -p "$build"
    (cd "$build" && "$ffmpeg_src/configure" --prefix=/usr --enable-cross-compile \
        --arch=aarch64 --target-os=linux --cc=clang --cxx=clang++ \
        --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm --strip=llvm-strip \
        --extra-cflags="$target -march=armv8-a" --extra-ldflags="$target -fuse-ld=lld" \
        --enable-shared --disable-static \
        --disable-programs --disable-doc --disable-network --disable-autodetect \
        --disable-everything --disable-avdevice --disable-avfilter \
        --enable-demuxer=asf --enable-protocol=file --enable-parser=vc1 \
        --enable-decoder=wmv1,wmv2,wmv3,vc1,wmav1,wmav2,wmapro,wmavoice \
        --enable-swscale --enable-swresample) >"$logs/ffmpeg-aarch64-configure.log"
    make -C "$build" -j"$jobs" >"$logs/ffmpeg-aarch64-build.log"
    make -C "$build" install DESTDIR="$sysroot" >/dev/null
fi

# The game, its code generator and shader cache from the native build.
build=$repo/out/build/linux-arm64-release
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$sdk/out/install/linux-arm64" \
    ${NG2_CMAKE_ARGS:-} >"$logs/ng2-arm64-configure.log"
cmake --build "$build" -j"$jobs" --target ng2 >"$logs/ng2-arm64-build.log"

# The program (ng2.bin) with a start script that gives it the game_files folder next to it, so a
# Steam shortcut to ng2 needs no launch options; the SDK's runtime and GPU plugin (the build
# stages them next to it) and FFmpeg ($ORIGIN rpath), the pipeline seed and the licences.
# Vulkan, the GPU driver, Wayland and the sound server are the system's.
rm -rf "${dist:?}"
mkdir -p "$dist/licenses"
llvm-strip -o "$dist/ng2.bin" "$build/ng2"
printf '%s\n' '#!/bin/sh' 'here=$(dirname "$(readlink -f "$0")")' \
    'exec "$here/ng2.bin" --game_data_root="$here/game_files" "$@"' > "$dist/ng2"
chmod +x "$dist/ng2"
for lib in "$build"/lib*.so*; do cp -P "$lib" "$dist/"; done
for lib in avformat avcodec avutil swresample swscale; do
    cp -P "$sysroot/usr/lib/lib$lib.so."* "$dist/"
done
cp "$repo/packaging/ng2-pipelines-seed.bin" "$dist/"
cp "$repo/LICENSE" "$repo/THIRD_PARTY.md" "$dist/licenses/"
cp "$ffmpeg_src/LICENSE.md" "$dist/licenses/LICENSE-FFmpeg.md"
cp "$ffmpeg_src/COPYING.LGPLv2.1" "$dist/licenses/COPYING-FFmpeg.LGPLv2.1"
echo "$dist"
