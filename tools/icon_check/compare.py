#!/usr/bin/env python3
"""compare.py <c++ out dir> [python dir] [sheet.png]: the icon made by src/game_icon.cpp (tools/icon_check/main.cpp's
ng2_<size>.pam, or the game's cached ng2_<size>.png) against the Python mock-ups (out/icon/n_plate_over_ii2_<size>.png):
mean and largest difference per size, the share of channels more than 2 apart, and a side-by-side sheet (Python,
C++, the difference x8). Game art: keep the sheet under out/."""
import os, sys
import numpy as np
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
cpp = sys.argv[1]
py = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, '../../out/icon')
sheet_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(cpp, 'compare.png')
SIZES = (512, 256, 128, 64, 48, 32, 16)

def load(path):
    if path.endswith('.pam'):
        raw = open(path, 'rb').read(); end = raw.index(b'ENDHDR\n') + 7
        head = dict(l.split(' ', 1) for l in raw[:end].decode().splitlines()[1:-1])
        w, h = int(head['WIDTH']), int(head['HEIGHT'])
        return np.frombuffer(raw[end:], np.uint8).reshape(h, w, 4)
    return np.array(Image.open(path).convert('RGBA'))

rows = []; worst = 0
for s in SIZES:
    c = next((os.path.join(cpp, 'ng2_%d.%s' % (s, e)) for e in ('pam', 'png') if os.path.exists(os.path.join(cpp, 'ng2_%d.%s' % (s, e)))), None)
    a = np.array(Image.open(os.path.join(py, 'n_plate_over_ii2_%d.png' % s)).convert('RGBA')).astype(np.int16)
    b = load(c).astype(np.int16)
    d = np.abs(a - b)
    print('%3d  mean %.4f  max %3d  >2: %.3f%%' % (s, d.mean(), d.max(), 100 * (d > 2).mean()))
    worst = max(worst, d.mean()); rows.append((s, a, b, d))

z = lambda s: max(1, 128 // s)                         # the small sizes enlarged for looking
W = sum(s * z(s) + 16 for s, *_ in rows) + 16
H = 3 * (512 + 16) + 16
sheet = Image.new('RGB', (W, H), (24, 26, 30)); x = 16
for s, a, b, d in rows:
    k = z(s)
    for r, im in enumerate((a, b, np.clip(d.max(-1, keepdims=True).repeat(3, -1) * 8, 0, 255))):
        im = Image.fromarray(im.astype(np.uint8), 'RGBA' if im.shape[-1] == 4 else 'RGB')
        im = im.resize((s * k, s * k), Image.NEAREST)
        sheet.paste(im, (x, 16 + r * (512 + 16)), im if im.mode == 'RGBA' else None)
    x += s * k + 16
sheet.save(sheet_path)
print('sheet', sheet_path, ' worst mean %.4f' % worst)
