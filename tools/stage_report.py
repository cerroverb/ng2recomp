"""Report of a scenario sweep (tools/stage_sweep.sh).

usage: stage_report.py <sweep dir> <seconds per run>   report.csv, report.txt, sheet.png
       stage_report.py --one <run dir> <seconds>       one line for the terminal

Per scenario, from its log (all.log) and screenshots:
  status   ok | no jump (the scenario wasn't loaded) | guest fault (the game's
           code read or wrote unmapped memory) | exited early (crash or
           quit) | stalled (no frames in the last 10 s, or under 5 fps) |
           slow (median under 55 fps) | black (last screenshot all dark)
  start_s  seconds from boot to the scenario start
  fps      median and minimum frame rate from 20 s after the start
  gpu_ms   GPU time a frame (NG2_PROFILE), median over the same window
  warn/err distinct warning / error messages after the start (renderer ones
           listed in report.txt)
"""
import csv, datetime, os, re, statistics, sys

TIME = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\] \[(\w+)\] \[\w+\] \[t\d+\] (.*)$')
START = re.compile(r'scenario start: (\d+) (\S*) ?entry')
JUMP = re.compile(r'scenario jump: \d+ ')
FPS = re.compile(r'^fps ([\d.]+)')
GPU = re.compile(r'^video: GPU ([\d.]+) ms a frame')
FAULT = re.compile(r'Unhandled guest access violation: (read|write) of guest (0x[0-9A-Fa-f]+)')
# Boot noise every run has.
IGNORED = ('ResolvePath(\\Device)', "VFS: '\\Device'", "path='\\Device\\Image'")


def parse_time(s):
    return datetime.datetime.strptime(s, '%Y-%m-%d %H:%M:%S.%f')


def analyse(run, secs):
    r = {'scenario': os.path.basename(run), 'name': '', 'status': '', 'start_s': '', 'fps': '',
         'fps_min': '', 'gpu_ms': '', 'warn': 0, 'err': 0, 'later': '', 'shot': ''}
    log = os.path.join(run, 'all.log')
    lines = []
    if os.path.exists(log):
        with open(log, errors='replace') as f:
            for line in f:
                m = TIME.match(line.rstrip('\n'))
                if m:
                    lines.append((parse_time(m.group(1)), m.group(2), m.group(3)))
    if not lines:
        r['status'] = 'no log'
        return r, []
    boot = lines[0][0]
    end = lines[-1][0]
    jumped = start = None
    later = []
    for t, level, msg in lines:
        m = JUMP.search(msg)
        if m:
            jumped = True
        m = START.search(msg)
        if m:
            if start is None and jumped:
                start = t
                r['name'] = m.group(2)
            elif start is not None:
                later.append(m.group(2) or m.group(1))
    r['later'] = ' '.join(later)
    messages = {}
    if start is not None:
        r['start_s'] = f'{(start - boot).total_seconds():.0f}'
        window = start + datetime.timedelta(seconds=20)
        fps = [float(FPS.match(m).group(1)) for t, _, m in lines if t >= window and FPS.match(m)]
        gpu = [float(GPU.match(m).group(1)) for t, _, m in lines if t >= window and GPU.match(m)]
        if fps:
            r['fps'] = f'{statistics.median(fps):.1f}'
            r['fps_min'] = f'{min(fps):.1f}'
        if gpu:
            r['gpu_ms'] = f'{statistics.median(gpu):.2f}'
        for t, level, msg in lines:
            if t < start or level not in ('warning', 'error') or any(s in msg for s in IGNORED):
                continue
            key = re.sub(r'0x[0-9a-fA-F]+|\b[0-9A-F]{8,}\b|\d+', '#', msg)
            messages.setdefault((level, key), msg)
        r['warn'] = sum(1 for level, _ in messages if level == 'warning')
        r['err'] = sum(1 for level, _ in messages if level == 'error')
    shots = sorted((f for f in os.listdir(run) if re.match(r'shot-\d+\.png$', f)),
                   key=lambda f: int(re.findall(r'\d+', f)[0]))
    if shots:
        r['shot'] = os.path.join(run, shots[-1])
    wall = None
    try:
        wall = int(open(os.path.join(run, 'wall')).read())
    except (OSError, ValueError):
        pass
    recent = [float(FPS.match(m).group(1)) for t, _, m in lines
              if t >= end - datetime.timedelta(seconds=10) and FPS.match(m)]
    fault = next((FAULT.search(m) for _, level, m in lines if level == 'error' and FAULT.search(m)),
                 None)
    if jumped is None or start is None:
        r['status'] = 'no jump'
    elif fault:
        r['status'] = f'guest fault ({fault.group(1)} {fault.group(2)})'
    elif (end - boot).total_seconds() < secs - 10 or (wall is not None and wall < secs - 10):
        r['status'] = 'exited early'
    elif not recent or max(recent) < 5:
        r['status'] = 'stalled'
    elif r['fps'] and float(r['fps']) < 55:
        r['status'] = 'slow'
    elif r['shot'] and dark(r['shot']):
        r['status'] = 'black'
    else:
        r['status'] = 'ok'
    return r, [f'{level}: {msg}' for (level, _), msg in messages.items()]


def dark(path):
    try:
        from PIL import Image, ImageStat
    except ImportError:
        return False
    try:
        im = Image.open(path).convert('L').resize((160, 90))
    except OSError:  # a shot cut short when the run was stopped
        return False
    return ImageStat.Stat(im).mean[0] < 4


def line(r):
    return (f"{r['scenario']:>4} {r['name']:<10} {r['status']:<12} start {r['start_s'] or '-':>3} s  "
            f"fps {r['fps'] or '-':>5} (min {r['fps_min'] or '-':>5})  gpu {r['gpu_ms'] or '-':>5} ms  "
            f"warn {r['warn']} err {r['err']}" + (f"  then {r['later']}" if r['later'] else ''))


def sheet(rows, path):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return False
    rows = [r for r in rows if r['shot']]
    if not rows:
        return False
    w, h, cols = 320, 180, 6
    out = Image.new('RGB', (cols * w, ((len(rows) + cols - 1) // cols) * (h + 16)), (24, 24, 24))
    d = ImageDraw.Draw(out)
    for i, r in enumerate(rows):
        x, y = (i % cols) * w, (i // cols) * (h + 16)
        try:
            out.paste(Image.open(r['shot']).convert('RGB').resize((w, h)), (x, y + 16))
        except OSError:
            pass
        color = (120, 220, 120) if r['status'] == 'ok' else (240, 110, 90)
        d.text((x + 4, y + 2), f"{r['scenario']} {r['name']} {r['status']}", fill=color)
    out.save(path)
    return True


def main():
    if len(sys.argv) < 3 or (sys.argv[1] == '--one' and len(sys.argv) < 4):
        sys.exit('usage: stage_report.py <sweep dir> <seconds> | --one <run dir> <seconds>')
    if sys.argv[1] == '--one':
        r, _ = analyse(sys.argv[2], int(sys.argv[3]))
        print(line(r).strip())
        return
    top, secs = sys.argv[1], int(sys.argv[2])
    runs = os.path.join(top, 'runs')
    names = [n for n in os.listdir(runs) if os.path.isdir(os.path.join(runs, n))]
    names.sort(key=lambda n: (0, int(n)) if n.isdigit() else (1, n))
    rows, text = [], []
    for n in names:
        r, messages = analyse(os.path.join(runs, n), secs)
        rows.append(r)
        text.append(line(r))
        text += [f'       {m}' for m in messages if 'video' in m or 'error' in m]
    fields = ['scenario', 'name', 'status', 'start_s', 'fps', 'fps_min', 'gpu_ms', 'warn', 'err',
              'later', 'shot']
    with open(os.path.join(top, 'report.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    counts = {}
    for r in rows:
        counts[r['status']] = counts.get(r['status'], 0) + 1
    summary = ', '.join(f'{v} {k}' for k, v in sorted(counts.items()))
    with open(os.path.join(top, 'report.txt'), 'w') as f:
        f.write('\n'.join(text) + f'\n\n{len(rows)} scenarios: {summary}\n')
    made = sheet(rows, os.path.join(top, 'sheet.png'))
    print(f'{len(rows)} scenarios: {summary}')
    print(f"{os.path.join(top, 'report.txt')}" + (f", {os.path.join(top, 'sheet.png')}" if made else ''))


main()
