#!/usr/bin/env bash
# Ninja Gaiden II (static recompilation) as an AppImage, for testing on the
# machine that built it (packaging/appimage/build.sh makes the portable one).
#
#   packaging/appimage/build-local.sh [out-dir]      (default out/appimage-local)
#
# Packs the current release build (out/build/linux-amd64-release): the game,
# ReXGlue's runtime and GPU plugin (looked up next to the executable), the
# ISO extractor and a launcher. FFmpeg, Vulkan, the GPU drivers, SDL's
# window-system libraries and glibc come from the system, so this is not a
# portable release (that needs an older-glibc container build and a small
# static FFmpeg with the WMV/WMA decoders). No game data is included: the
# first launch asks for the extracted game folder or the disc image.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
build=$repo/out/build/linux-amd64-release
out=$(mkdir -p "${1:-$repo/out/appimage-local}" && cd "${1:-$repo/out/appimage-local}" && pwd)
tool=${APPIMAGETOOL:-$(command -v appimagetool || true)}

[ -x "$build/ng2" ] || { echo "No build at $build: build the release preset first." >&2; exit 1; }
[ -x "$tool" ] || { echo "appimagetool not found (set APPIMAGETOOL)." >&2; exit 1; }

app=$out/AppDir
rm -rf "$app"
mkdir -p "$app/usr/bin"
cp "$build/ng2" "$build/librexruntime.so" "$app/usr/bin/"  # no GPU plugin: no GPU emulation
[ -f "$build/libTracyClient.so" ] && cp "$build/libTracyClient.so" "$app/usr/bin/"
strip --strip-unneeded "$app/usr/bin/"*
cp "$repo/tools/xdvdfs_extract.py" "$repo/tools/stfs_extract.py" "$repo/tools/find_dlc.py" "$app/usr/bin/"
cp "$here/AppRun" "$app/AppRun"
chmod +x "$app/AppRun"
cp "$here/ng2.desktop" "$app/"
# RecompLauncher reads it at the AppImage's root.
cp "$repo/packaging/recomplauncher.json" "$app/"
cp "$here/ng2.png" "$app/ng2.png"

ARCH=x86_64 "$tool" --no-appstream "$app" "$out/NinjaGaiden2-x86_64.AppImage" >/dev/null
echo "$out/NinjaGaiden2-x86_64.AppImage"
