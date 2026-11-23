#!/usr/bin/env bash
# Starts scenarios one by one from the New Game path (NG2_SCENARIO,
# src/stage_jump.cpp) in silent headless runs and reports what each did:
# tools/stage_report.py writes <out>/report.csv, <out>/report.txt and
# <out>/sheet.png (the last screenshot of every scenario). The scenario starts
# about 45 s after boot; a chapter shows its card, then its opening.
#
# usage: tools/stage_sweep.sh <out dir> [scenario ...]
#   <out dir> on the home disk (out/stages, say), not a tmpfs /tmp: a run's log
#   can reach hundreds of MB before it's trimmed.
#   scenario: a number (9-22 are chapters 1-14, 31-57 the cutscene rooms,
#   58-108 Mission Mode's missions 00-50), a range (9-22) or a name (chap_05,
#   r03_10). Default: the chapters, 9-22.
#   NG2_SWEEP_SECS=150   seconds per run
#   NG2_SWEEP_SHOTS="90 145"  screenshot times
#   NG2_SWEEP_KEEP=1     keep each run's full folder (default: log + shots only)
#   NG2_SWEEP_ARGS="--render_scale=2"  more game options for every run
#   Other variables (NG2_RENDERER, NG2_TEST_SIZE, ...) pass to run-headless.sh.
set -euo pipefail

out=$(realpath -m "${1:?out dir}")
shift
root=$(cd "$(dirname "$0")/.." && pwd)
secs=${NG2_SWEEP_SECS:-150}
shots=${NG2_SWEEP_SHOTS:-"90 145"}
# Title, New Game, difficulty, then the Mission Mode DLC's costume screen (two
# more presses where it's installed), then presses through a chapter card and
# its opening.
pad=30000:start,34000:a,37000:a,40000:a,43000:a,46000:a,70000:a,75000:a,80000:a,100000:a,105000:a,130000:a

scenarios=()
if [ $# -eq 0 ]; then
  for i in $(seq 9 22); do scenarios+=("$i"); done
else
  for s in "$@"; do
    if [[ $s =~ ^([0-9]+)-([0-9]+)$ ]]; then
      for i in $(seq "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}"); do scenarios+=("$i"); done
    else
      scenarios+=("$s")
    fi
  done
fi

mkdir -p "$out/runs"
n=0
for s in "${scenarios[@]}"; do
  n=$((n + 1))
  run="$out/runs/$s"
  rm -rf "$run"
  printf '[%d/%d] scenario %s ... ' "$n" "${#scenarios[@]}" "$s"
  t0=$(date +%s)
  # Every rotated log is kept (a guest fault can log ~100 MB a minute and push
  # the scenario start out of the default 20 files).
  env -u NG2_CHAPTER NG2_SCENARIO=$s NG2_FPS_LOG=1 NG2_PROFILE=1 "NG2_PAD_SCRIPT=$pad" "NG2_SHOTS=$shots" \
    "$root/tools/run-headless.sh" "$secs" "$run" --log_max_files=10000 ${NG2_SWEEP_ARGS:-} >"$run.out" 2>&1 &
  pid=$!
  # A guest fault repeats in the log ~50,000 times a second: stop the run a
  # few seconds after the first one.
  while kill -0 $pid 2>/dev/null; do
    sleep 2
    if grep -qs -m1 'Unhandled guest access violation' "$run/ng2.log"; then
      sleep 3
      pkill -f -- "--user_data_root=$run/user" || true
      break
    fi
  done
  wait $pid || true
  mkdir -p "$run"  # (a run that died before making its folder)
  echo "$(( $(date +%s) - t0 ))" >"$run/wall"
  mv "$run.out" "$run/run.out" 2>/dev/null || true
  # One log, oldest first (ng2.<n>.log: higher n is older), each warning or
  # error message at most 50 times, then how often it repeated.
  { find "$run" -maxdepth 1 -name 'ng2.*.log' -printf '%f\n' | sort -t. -k2,2nr |
      sed "s|^|$run/|"; [ -f "$run/ng2.log" ] && echo "$run/ng2.log"; } |
    xargs -r cat |
    awk '/\] \[(warning|error)\] / { k = $0; sub(/^[^]]*\] /, "", k); sub(/\[t[0-9]+\] /, "", k);
           if (++n[k] > 50) next }
         { print }
         END { for (k in n) if (n[k] > 50) print "[repeated " n[k] " times] " k }' \
      >"$run/all.log" || true
  if [ "${NG2_SWEEP_KEEP:-0}" != 1 ]; then
    find "$run" -mindepth 1 -maxdepth 1 ! -name all.log ! -name 'shot-*.png' ! -name wall \
      ! -name run.out -exec rm -rf {} +
  fi
  python3 -I "$root/tools/stage_report.py" --one "$run" "$secs"
done

python3 -I "$root/tools/stage_report.py" "$out" "$secs"
