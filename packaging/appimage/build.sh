#!/usr/bin/env bash
# Ninja Gaiden II for Linux as a portable AppImage (the one for releases and testing).
#
#   packaging/appimage/build.sh [out-dir]      (default out/appimage)
#
# Builds the ReXGlue SDK and the game in an Ubuntu 22.04 container (Dockerfile here), so the
# AppImage needs only glibc 2.35 (SteamOS, Ubuntu 22.04+, Debian 12, Fedora 36+), and packs
# <out-dir>/NinjaGaiden2-x86_64.AppImage with the libraries a system may lack: FFmpeg (a small
# LGPL build of 9.0.2 with the movies' codecs, see the Dockerfile); libstdc++ is linked in.
# glibc, zstd, the Vulkan loader and drivers, and the X11/Wayland libraries are the system's.
#
# The build runs in out/portable (NG2_PORTABLE_DIR): a copy of the sources in src/ (the code
# generator writes into the tree), with the SDK patches already applied in thirdparty/, and
# the SDK's and the game's build trees. The game's default.xex (and
# default.xexp) are read from game_files/ (NG2_GAME_DIR) for the code and shaders. No game data
# goes into the AppImage. packaging/appimage/build-local.sh is the quick, unportable build for
# this machine.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
dest=$(mkdir -p "${1:-$repo/out/appimage}" && cd "${1:-$repo/out/appimage}" && pwd)
out=$(mkdir -p "${NG2_PORTABLE_DIR:-$repo/out/portable}" && cd "${NG2_PORTABLE_DIR:-$repo/out/portable}" && pwd)
# The sources are copied into $out/src with rsync --delete: never into the sources themselves.
case "$out/" in
    "$repo/out/"?*) ;;
    "$repo/"*) echo "NG2_PORTABLE_DIR must be outside the sources, or under out/." >&2; exit 1 ;;
esac
game=$(cd "${NG2_GAME_DIR:-$repo/game_files}" && pwd)
image=ng2-build:jammy
jobs=${JOBS:-$(nproc)}
tool=${APPIMAGETOOL:-$(command -v appimagetool || true)}

[ -f "$game/default.xex" ] && [ -f "$game/default.xexp" ] ||
    { echo "No default.xex and default.xexp in $game (NG2_GAME_DIR)." >&2; exit 1; }
[ -x "$tool" ] || { echo "appimagetool not found (set APPIMAGETOOL)." >&2; exit 1; }
# (appimagetool skips the .zsync without a word when zsyncmake is missing: checked before the build)
command -v zsyncmake >/dev/null || { echo "zsyncmake is needed for the .zsync file (zsync package)" >&2; exit 1; }

docker build -q -t "$image" "$here" >/dev/null

# The sources, without build trees and the reference checkouts (copies share blocks on btrfs).
mkdir -p "$out/src"
rsync -a --delete --exclude /out --exclude /game_files --exclude /thirdparty/xenia-edge \
    --exclude 'thirdparty/*/out' --exclude /.git --exclude '/*.log' \
    "$repo/" "$out/src/"
mkdir -p "$out/src/game_files"  # the game's files are mounted here (the manifest's path)

docker run --rm -m 24g --shm-size=4g -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$out":/out -v "$game":/out/src/game_files:ro "$image" bash -euc '
# The C++ runtime linked in stays private (--exclude-libs; only it: the SDK exports SDL and the
# rest the game uses): exported, it took the place of
# the system copy for Vulkan layers written in C++, and one that throws aborted the game (#27,
# lsfg-vk). The unwinder is the system one (libgcc_s, on every glibc system), not a private copy:
# a thread exit (pthread_exit) unwinds with the system one, and the C++ runtime handlers then
# called the private one, never set up, which aborted (the SDK tools at the build, 11-16).
flags="-static-libstdc++ -fuse-ld=lld -Wl,--exclude-libs,libstdc++.a:libgcc.a"
# The SDK for any x86-64 PC since about 2009 (x86-64-v2: SSE4.2); the game twice, for those
# and for AVX2 machines (x86-64-v3: the Steam Deck, any Ryzen, Intel since Haswell), which
# the first hands over to where the CPU has it (src/cpu_handover.cpp).
arch="-march=x86-64-v2"
sdk=/out/src/thirdparty/rexglue-sdk
# A step that fails shows the end of its log.
step() { log=$1; shift; "$@" >"$log" 2>&1 || { echo "== $log" >&2; tail -20 "$log" >&2; exit 1; }; }
step /out/sdk-configure.log cmake -S "$sdk" -B /out/sdk-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_STANDARD=23 \
    -DCMAKE_C_FLAGS="$arch" -DCMAKE_CXX_FLAGS="$arch" \
    -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags" \
    -DCMAKE_INSTALL_PREFIX=/out/sdk-install -DREXGLUE_ENABLE_TRACY=OFF
step /out/sdk-build.log ninja -C /out/sdk-build -j'"$jobs"'
step /out/sdk-install.log cmake --install /out/sdk-build

game() {  # <build dir> <march>
    step $1.configure.log cmake -S /out/src -B $1 -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="-march=$2" -DCMAKE_CXX_FLAGS="-march=$2" \
        -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags" \
        -DCMAKE_PREFIX_PATH="/out/sdk-install;/usr/local" -DNG2_GAME_DIR=/out/src/game_files '"${NG2_CMAKE_ARGS:-}"'
    step $1.log ninja -C $1 -j'"$jobs"' ng2
}
game /out/build-v2 x86-64-v2
game /out/build x86-64-v3
'

# The AppDir: the program and the SDK's runtime and GPU plugin, the launcher and its tools, and
# the libraries a system may lack (found with ldd in the container).
app=$out/AppDir
rm -rf "$app"
mkdir -p "$app/usr/bin" "$app/usr/lib" "$app/usr/share/licenses/ng2"
# (no GPU plugin: no GPU emulation; the runtime the baseline build's, for both)
cp "$out/build-v2/ng2" "$out/build-v2/librexruntime.so" "$app/usr/bin/"
cp "$out/build/ng2" "$app/usr/bin/ng2-avx2"
cp "$repo/tools/xdvdfs_extract.py" "$repo/tools/stfs_extract.py" "$repo/tools/find_dlc.py" "$app/usr/bin/"
cp "$here/AppRun" "$app/AppRun"
chmod +x "$app/AppRun"
cp "$here/ng2.desktop" "$app/"
cp "$here/ng2.png" "$app/ng2.png"
cp "$here/ng2.png" "$app/.DirIcon"
cp "$repo/LICENSE" "$repo/THIRD_PARTY.md" "$app/usr/share/licenses/ng2/"
# The pipelines recorded playing the game (tools/collect_pipelines.sh), made in the background at
# the first start so new objects and effects don't pop in.
mkdir -p "$app/usr/share/ng2"
cp "$repo/packaging/ng2-pipelines-seed.bin" "$app/usr/share/ng2/"

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$out":/out "$image" bash -euc '
app=/out/AppDir
llvm-strip --strip-unneeded "$app"/usr/bin/ng2 "$app"/usr/bin/ng2-avx2 "$app"/usr/bin/*.so
# The licence of the bundled FFmpeg (LGPL 2.1 or later, source ffmpeg.org 9.0.2).
cp /usr/local/share/ffmpeg/LICENSE.md "$app/usr/share/licenses/ng2/LICENSE-FFmpeg.md"
cp /usr/local/share/ffmpeg/COPYING.LGPLv2.1 "$app/usr/share/licenses/ng2/COPYING-FFmpeg.LGPLv2.1"
# Never bundled: glibc, GPU, X11/Wayland, font and session libraries, which must come from
# the system (as in the Ninja Gaiden Black AppImage).
system_lib="^(ld-linux|libc\.so|libm\.so|libdl\.so|librt\.so|libpthread|libresolv|libutil\.so|libstdc\+\+|libgcc_s|libGL|libEGL|libGLX|libGLdispatch|libOpenGL|libdrm|libgbm|libvulkan|libX11|libXau|libXdmcp|libXext|libXrender|libxcb\.so|libxcb-(shm|render|xfixes|randr|shape|sync|xkb|xinput|glx|dri2|dri3|present)\.so|libwayland-|libfontconfig|libfreetype|libharfbuzz|libexpat|libz\.|libdbus-1|libudev|libsystemd|libgpg-error|libselinux|libmount|libblkid|libffi|liblzma|liblz4|libzstd|libgcrypt|libcap|libbsd|libmd\.)"
for bin in "$app"/usr/bin/ng2 "$app"/usr/bin/ng2-avx2 "$app"/usr/bin/*.so; do
    missing=$(LD_LIBRARY_PATH="$app/usr/bin:/usr/local/lib" ldd "$bin" | grep "not found" || true)
    [ -z "$missing" ] || { echo "$bin: $missing" >&2; exit 1; }
    LD_LIBRARY_PATH="$app/usr/bin:/usr/local/lib" ldd "$bin" | awk "/=>/ {print \$3}" | while read -r lib; do
        b=$(basename "$lib")
        case "$lib" in "$app"/*) continue ;; esac
        echo "$b" | grep -Eq "$system_lib" && continue
        [ -e "$app/usr/lib/$b" ] || cp -L "$lib" "$app/usr/lib/"
    done
done
for so in "$app"/usr/lib/*.so*; do patchelf --set-rpath "\$ORIGIN" "$so"; done
patchelf --set-rpath "\$ORIGIN:\$ORIGIN/../lib" "$app"/usr/bin/ng2 "$app"/usr/bin/ng2-avx2 "$app"/usr/bin/*.so
echo "Bundled:"; ls "$app/usr/lib"
echo "Needs from the system:"
for bin in "$app"/usr/bin/ng2 "$app"/usr/bin/ng2-avx2 "$app"/usr/bin/*.so "$app"/usr/lib/*.so*; do
    readelf -d "$bin" | sed -n "s/.*NEEDED.*\[\(.*\)\]/\1/p"
done | sort -u | grep -vxF -f <(ls "$app/usr/lib"; ls "$app/usr/bin") | tr "\n" " "; echo
echo "Newest glibc symbol:"
for bin in "$app"/usr/bin/ng2 "$app"/usr/bin/ng2-avx2 "$app"/usr/bin/*.so "$app"/usr/lib/*.so*; do
    objdump -T "$bin" | grep -o "GLIBC_[0-9.]*"
done | sort -uV | tail -1
'

# Update information for AppImageUpdate and the like: the newest release on GitHub, by the .zsync
# made beside the AppImage (zsyncmake, in the current folder), which goes up with each release.
rm -f "$dest/NinjaGaiden2-x86_64.AppImage" "$dest/NinjaGaiden2-x86_64.AppImage.zsync"  # (never a stale pair)
(cd "$dest" && ARCH=x86_64 "$tool" --appimage-extract-and-run --no-appstream \
    -u "gh-releases-zsync|gamboozino|ng2recomp|latest|NinjaGaiden2-x86_64.AppImage.zsync" "$app" \
    "$dest/NinjaGaiden2-x86_64.AppImage" >"$out/appimagetool.log" 2>&1) ||
    { tail -20 "$out/appimagetool.log"; exit 1; }
ls -la "$dest/NinjaGaiden2-x86_64.AppImage" "$dest/NinjaGaiden2-x86_64.AppImage.zsync"
