#!/usr/bin/env bash
# Silent test run of the Windows build under Proton (umu-run) in a headless
# gamescope: no window on the desktop, no sound, no input devices, its own
# Wine prefix and user data. As tools/run-headless.sh, for the Windows exe.
#
# usage: tools/run-headless-windows.sh <seconds> <out_dir> [extra ng2 args...]
#   NG2_WIN_DIR=<dir>   the packaged game (default out/windows/NinjaGaiden2)
#   NG2_SHOTS="10 30"   gamescope screenshots at those seconds
#   PROTONPATH=<dir>    Proton for umu (default Steam's Proton - Experimental)
#   NG2_TEST_REFRESH=120  the headless display's refresh rate (Hz)
#   NG2_PAD_SCRIPT, NG2_SAVE_FROM: as in run-headless.sh
set -euo pipefail

secs=${1:?seconds}
out=$(realpath -m "${2:?out dir}")
shift 2
root=$(cd "$(dirname "$0")/.." && pwd)
win=$(realpath "${NG2_WIN_DIR:-$root/out/windows/NinjaGaiden2}")
mkdir -p "$out/user" "$out/prefix"
if [[ -n ${NG2_SAVE_FROM:-} ]]; then cp -a "$NG2_SAVE_FROM/." "$out/user/"; fi
unset $(compgen -e | grep '^REX') 2>/dev/null || true

# Linux paths as Wine sees them (Z: is /).
winpath() { printf 'Z:%s' "$(realpath -m "$1" | tr / '\\')"; }

inner='
shots=$1 out=$2 secs=$3; shift 3
timeout --kill-after=5 "$secs" "$@" &
pid=$!
t=0
for s in $shots; do
  sleep $((s - t)); t=$s
  kill -0 $pid 2>/dev/null || break
  gamescopectl screenshot "$out/shot-$s.png" >/dev/null 2>&1 || true
done
wait $pid
'

# NG2_AUDIO=0: the null audio device (SDL's driver variables don't reach a
# Proton game reliably). The log and config are passed as paths.
# HOME stays: umu keeps its runtime there. The prefix, settings, saves and
# log are this run's.
exec env -u WAYLAND_DISPLAY -u DISPLAY \
  WINEPREFIX="$out/prefix" GAMEID=umu-0 PROTONPATH="${PROTONPATH:-$HOME/.steam/steam/steamapps/common/Proton - Experimental}" \
  STEAM_COMPAT_MOUNTS="$root:$out" UMU_LOG=0 PROTON_LOG=0 \
  NG2_AUDIO=0 NG2_CONFIG="$(winpath "$out/ng2.toml")" \
  SDL_GAMECONTROLLER_IGNORE_DEVICES=0x2dc8/0x310a SDL_JOYSTICK_IGNORE_DEVICES=0x2dc8/0x310a \
  timeout --kill-after=10 "$((secs + 30))" \
  gamescope --backend headless -W 1280 -H 720 -w 1280 -h 720 ${NG2_TEST_REFRESH:+-r $NG2_TEST_REFRESH} -- \
  bash -c "$inner" inner "${NG2_SHOTS:-}" "$out" "$((secs + 20))" \
  umu-run "$win/ng2.exe" --game_data_root="$(winpath "$root/game_files")" \
  --user_data_root="$(winpath "$out/user")" --input_backend=none \
  --log_file="$(winpath "$out/ng2.log")" "$@"
