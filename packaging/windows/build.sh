#!/usr/bin/env bash
# Ninja Gaiden II for Windows x64, cross-compiled on Linux.
#
#   packaging/windows/build.sh
#
# Builds the ReXGlue SDK for Windows (thirdparty/rexglue-sdk/out/install/windows-x64) when it
# isn't there, fetches FFmpeg's Windows build into out/windows-deps once, builds the game
# (cmake/toolchains/windows-x64-clang-xwin.cmake: clang and lld against Microsoft's C runtime
# and Windows SDK from xwin in ~/.xwin) and packs out/windows/NinjaGaiden2/ -- the program and
# the DLLs it needs that Windows doesn't ship -- into out/windows/NinjaGaiden2-windows-x64.zip.
# No game data: the first launch installs the game and asks for Title Update 3.
#
# Needs: a native build first (out/build/linux-amd64-release: its code generator output and
# shader cache are used), cmake, ninja, clang, lld, curl, bsdtar, cabextract, and xwin's splat
# (`xwin --accept-license splat --output ~/.xwin`: the license is yours to accept).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
toolchain=$repo/cmake/toolchains/windows-x64-clang-xwin.cmake
sdk=$repo/thirdparty/rexglue-sdk
deps=$repo/out/windows-deps
out=$repo/out/windows
ffmpeg=ffmpeg-n9.0.2-17-g2a571b6068-win64-lgpl-shared-9.0
ffmpeg_url=https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/$ffmpeg.zip
ffmpeg_sha256=7157177b8a6cb2174c1650ba8c71b363f2c78cba5330f88c4c02cf5b2b880646
# Microsoft's Visual C++ 2015-2022 runtime 14.44.35211 (x64): its DLLs go next to ng2.exe, so a
# PC without the redistributable installed runs the game too.
vcredist_url=https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B/VC_redist.x64.exe
vcredist_sha256=cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b
# (msvcp140_1: std::pmr's functions, src/gpu/interp.cpp)
vcruntime_dlls=(msvcp140 msvcp140_1 msvcp140_atomic_wait vcruntime140 vcruntime140_1)
mkdir -p "$deps" "$out"

[ -f "$repo/out/build/linux-amd64-release/shaders/ng2_shader_cache.cpp" ] ||
    { echo "No native build: build out/build/linux-amd64-release first (README)." >&2; exit 1; }

# The SDK for Windows (its D3D12 backend off: the game draws with its own Vulkan renderer),
# brought up to date every time (cheap when nothing changed).
if [ ! -f "$sdk/out/build/windows-x64/build.ninja" ]; then
    cmake -S "$sdk" -B "$sdk/out/build/windows-x64" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=23 \
        -DCMAKE_INSTALL_PREFIX="$sdk/out/install/windows-x64" \
        -DREXGLUE_USE_D3D12=OFF -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF \
        >"$out/sdk-configure.log"
fi
cmake --build "$sdk/out/build/windows-x64" -j"${JOBS:-$(nproc)}" >"$out/sdk-build.log"
cmake --install "$sdk/out/build/windows-x64" >/dev/null

# FFmpeg (LGPL shared build: headers, import libraries, DLLs)
if [ ! -f "$deps/ffmpeg/lib/avformat.lib" ]; then
    [ -s "$deps/$ffmpeg.zip" ] || curl -fL -o "$deps/$ffmpeg.zip" "$ffmpeg_url" ||
        { echo "FFmpeg's build is gone from $ffmpeg_url: put $ffmpeg.zip in $deps." >&2; exit 1; }
    echo "$ffmpeg_sha256  $deps/$ffmpeg.zip" | sha256sum -c --quiet
    rm -rf "$deps/ffmpeg" && mkdir -p "$deps/ffmpeg"
    bsdtar -xf "$deps/$ffmpeg.zip" -C "$deps/ffmpeg" --strip-components 1
fi

# The runtime DLLs, out of the redistributable's installer: cabinets appended to the exe hold the
# MSI packages and their cabinets, where the x64 files are named <dll>_amd64.
# (the stamp names the DLLs too: one added to the list is extracted again)
if [ "$(cat "$deps/vcruntime/.sha256" 2>/dev/null)" != "$vcredist_sha256 ${vcruntime_dlls[*]}" ]; then
    exe=$deps/VC_redist.x64-$vcredist_sha256.exe
    if [ ! -s "$exe" ]; then
        curl -fL -o "$exe.tmp" "$vcredist_url" ||
            { echo "The VC++ redistributable is gone from $vcredist_url: put it in $exe." >&2; exit 1; }
        mv "$exe.tmp" "$exe"
    fi
    echo "$vcredist_sha256  $exe" | sha256sum -c --quiet ||
        { rm -f "$exe"; echo "$exe isn't the pinned VC++ redistributable (removed)." >&2; exit 1; }
    tmp=$deps/vcredist.tmp
    rm -rf "$tmp" && mkdir -p "$tmp/dll"
    for at in $(LC_ALL=C grep -obUaP 'MSCF\x00\x00\x00\x00' "$exe" | cut -d: -f1); do
        tail -c +$((at + 1)) "$exe" > "$tmp/cab"
        cabextract -q -d "$tmp/c$at" "$tmp/cab" 2>/dev/null || true
    done
    for payload in "$tmp"/c*/*; do
        cabextract -q -d "$tmp/dll" -F '*_amd64' "$payload" 2>/dev/null || true
    done
    rm -rf "$deps/vcruntime" && mkdir -p "$deps/vcruntime"
    for dll in "${vcruntime_dlls[@]}"; do
        cp "$tmp/dll/$dll.dll_amd64" "$deps/vcruntime/$dll.dll"
    done
    rm -rf "$tmp"
    echo "$vcredist_sha256 ${vcruntime_dlls[*]}" > "$deps/vcruntime/.sha256"
fi

# The bundled runtime must be at least as new as the C++ library the game is built against
# (the DLLs next to ng2.exe are found before a newer one in System32).
# (Same xwin as the toolchain: ~/.xwin.)
stl=$(sed -n 's/^#define _MSVC_STL_UPDATE *\([0-9]*\)L.*/\1/p' "$HOME/.xwin/crt/include/yvals_core.h" 2>/dev/null || true)
[ -n "$stl" ] || { echo "Can't read the MSVC version from ~/.xwin/crt/include/yvals_core.h." >&2; exit 1; }
[ "$stl" -le 202503 ] ||
    { echo "xwin's MSVC ($stl) is newer than the bundled runtime 14.44: update vcredist_url." >&2; exit 1; }

# The game twice: for any x86-64 PC since about 2009 (x86-64-v2, the toolchain's default) and for
# AVX2 machines (x86-64-v3), which the first hands over to where the CPU has it
# (src/cpu_handover.cpp); the SDK is built once, for the first.
build=$repo/out/build/windows-x64
build_avx2=$repo/out/build/windows-x64-avx2
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$sdk/out/install/windows-x64" \
    -DNG2_FFMPEG_DIR="$deps/ffmpeg" ${NG2_CMAKE_ARGS:-} >"$out/configure.log"
cmake --build "$build" -j"${JOBS:-$(nproc)}" --target ng2 >"$out/build.log"
cmake -S "$repo" -B "$build_avx2" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$sdk/out/install/windows-x64" \
    -DNG2_CROSS_FLAGS="-march=x86-64-v3" \
    -DNG2_FFMPEG_DIR="$deps/ffmpeg" ${NG2_CMAKE_ARGS:-} >"$out/configure-avx2.log"
cmake --build "$build_avx2" -j"${JOBS:-$(nproc)}" --target ng2 >"$out/build-avx2.log"

pkg=$out/NinjaGaiden2
rm -rf "${pkg:?}"
mkdir -p "$pkg/licenses"
llvm-strip -o "$pkg/ng2.exe" "$build/ng2.exe"
llvm-strip -o "$pkg/ng2-avx2.exe" "$build_avx2/ng2.exe"
cp "$sdk/out/install/windows-x64/bin/"{rexruntime,SDL3}.dll "$pkg/"
cp "$deps/ffmpeg/bin/"{avformat-63,avcodec-63,avutil-61,swresample-7,swscale-10}.dll "$pkg/"
for dll in "${vcruntime_dlls[@]}"; do cp "$deps/vcruntime/$dll.dll" "$pkg/"; done
# The pipelines recorded playing the game (tools/collect_pipelines.sh), made in the background at
# the first start.
cp "$repo/packaging/ng2-pipelines-seed.bin" "$pkg/"
cp "$repo/LICENSE" "$repo/THIRD_PARTY.md" "$pkg/licenses/"
cp "$deps/ffmpeg/LICENSE.txt" "$pkg/licenses/LICENSE-FFmpeg.txt"
cat > "$pkg/README.txt" <<'TXT'
Ninja Gaiden II - static recompilation for Windows (Vulkan)

Run ng2.exe. The first time it asks about portable mode (below), then installs the game:
  1. Your copy of the game: a disc image (.iso), the Games on Demand package (the file in
     544307D5\00007000, next to its .data folder), or default.xex in an extracted copy.
     A disc image or package is copied into %LOCALAPPDATA%\NinjaGaiden2\game (about 7 GB).
  2. Title Update 3 for the World release: its package (TU_1A461UL_000000G000000.00000000000O4)
     or default.xexp.
  3. Downloadable content, optional: the folder with the packages (544307D5\00000002).
     More can be added later in the "dlc" folder inside the game folder.

Needs a CPU with SSE4.2 and a GPU with Vulkan 1.3 drivers. The Microsoft Visual C++ runtime DLLs
are included. ng2.exe starts ng2-avx2.exe, the faster build, where the CPU has AVX2.

Settings, saves and the log are in %LOCALAPPDATA%\NinjaGaiden2. Portable mode: with a file named
portable.txt next to ng2.exe, they go in a "userdata" folder next to it instead (the folder must be
writable, so not inside Program Files). No game data is included.
TXT
rm -f "$out/NinjaGaiden2-windows-x64.zip"
(cd "$out" && bsdtar -a -cf NinjaGaiden2-windows-x64.zip NinjaGaiden2)
echo "$out/NinjaGaiden2-windows-x64.zip"
