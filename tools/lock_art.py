#!/usr/bin/env python3
"""The "locked secret achievement" icon: a chrome padlock on the weathered grey tile of the game's
own achievement icons, so it sits in a list beside them as one of the set.

Nothing of the game's art is stored here: the picture is built at run time from the game's decoded
achievement icons handed in (64x64, as extracted from its XDBF). Needs Pillow and numpy; reads no
files, uses no fonts. The padlock's shapes are drawn by code (distance fields, a height field lit
from the top left like the icons' engraved characters).

    build_lock(icons) -> 64x64 RGBA image
        icons: {name: PIL.Image}; the names are the icon files' names, with or without ".png"

What it takes from the icons, and what for (the nine "nine stars" tiles, grey stone with two chrome
characters each, GROUND_TILES below):
  - the ground: the nine tiles share one stone ground under different characters. For every pixel
    the largest group of tiles that agree there is the ground (the characters differ from tile to
    tile), their mean its colour; the few pixels where no three agree are filled from around them.
  - the metal's colours: the characters' own pixels (those well away from the ground found above)
    give the chrome's bright and dark tones, the ground gives its mid tone, so the padlock has the
    tint and contrast of the engraved characters.
  - the wear: the ground's fine detail (ground minus its blur) is laid over the metal as scratches
    and pits.

    python3 tools/lock_art.py <dir with the icons' PNGs> [out dir]     (default out/lock)
writes lock.png and preview.png (the padlock among real icons at 64, 48 and 32, plain and greyed).
"""
import numpy as np
from PIL import Image

SIZE = 64
SS = 6  # supersampling of the drawn shapes

# The nine grey tiles with the stars' names (9 purple ... 1 white): one ground, nine pairs of characters.
GROUND_TILES = ['03a4b34c', '03a4c774', '03a4dba9', '03a4efb4', '03a503e1', '03a517f6', '03a52c34',
                '03a54048', '03a55441']

# ---- the padlock, in icon pixels ----
BODY_C, BODY_H, BODY_R = (32.0, 43.0), (16.5, 12.0), 3.5     # centre, half size, corner radius
SHACKLE_C, SHACKLE_R, SHACKLE_W = (32.0, 25.5), 10.0, 2.9     # arch centre, mid radius, half width
KEY_C, KEY_R, KEY_FOOT = (32.0, 40.5), 3.3, 49.0              # keyhole: circle and the slot's end row
LIGHT = np.array([-0.52, -0.66, 0.54])                        # from the top left, like the characters
SHADOW = (2.0, 2.4)                                           # the drop shadow's offset


def _blur(a, sigma):
    """Gaussian blur of a 2D or 3D float array (edges repeated)."""
    r = max(1, int(sigma * 3 + 0.5))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    for axis in (0, 1):
        pad = [(0, 0)] * a.ndim
        pad[axis] = (r, r)
        p = np.pad(a, pad, mode='edge')
        a = sum(k[i] * np.take(p, np.arange(i, i + a.shape[axis]), axis=axis) for i in range(2 * r + 1))
    return a


def _lum(rgb):
    return rgb @ np.array([0.299, 0.587, 0.114])


def tile_ground(tiles):
    """The ground the tiles share (float RGB), and the mask of pixels where it was certain."""
    a = np.stack(tiles)                                             # n, 64, 64, 3
    d = np.abs(a[:, None] - a[None]).max(-1)                        # n, n, h, w: how far apart two tiles are
    agree = d < 14
    count = agree.sum(1)                                            # per tile: tiles agreeing with it
    best = count.argmax(0)                                          # the tile in the largest group
    members = np.take_along_axis(agree, best[None, None], axis=0)[0]  # n, h, w
    ground = (a * members[..., None]).sum(0) / members.sum(0)[..., None]
    sure = count.max(0) >= 5
    # the unsure pixels (characters on most tiles): an estimate from the sure ones around them, then
    # the tile that comes nearest to it there, so the stone keeps its grain
    w = sure.astype(float)
    est = ground.copy()
    for _ in range(6):
        if w.min() > 0.5:
            break
        num, den = _blur(est * w[..., None], 1.5), _blur(w, 1.5)
        fill = (w < 0.5) & (den > 0.05)
        est[fill] = num[fill] / den[fill][..., None]
        w[fill] = 1.0
    off = np.abs(a - est[None]).max(-1)                             # n, h, w
    near = np.take_along_axis(a, off.argmin(0)[None, ..., None], axis=0)[0]
    take = ~sure & (off.min(0) < 26)
    ground = np.where(sure[..., None], ground, est)
    ground[take] = near[take]
    return ground, sure, a


def metal_tones(a, ground):
    """Bright, mid and dark RGB of the tiles' chrome characters."""
    diff = _lum(a) - _lum(ground)[None]
    bright = a[diff > 60]
    dark = a[diff < -35]
    hi = np.percentile(bright, 92, axis=0) if len(bright) > 50 else np.array([250.0, 250, 244])
    lo = np.percentile(dark, 12, axis=0) if len(dark) > 50 else np.array([14.0, 14, 16])
    mid = np.percentile(ground.reshape(-1, 3), 62, axis=0)
    return hi, mid, lo


def _sd_round_box(x, y, c, h, r):
    qx, qy = np.abs(x - c[0]) - (h[0] - r), np.abs(y - c[1]) - (h[1] - r)
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def _shapes():
    """Distance fields and the height field of the padlock, supersampled."""
    n = SIZE * SS
    y, x = (np.mgrid[0:n, 0:n] + 0.5) / SS
    body = _sd_round_box(x, y, BODY_C, BODY_H, BODY_R)
    # the shackle: an arch over two straight legs, a round bar
    dx, dy = x - SHACKLE_C[0], y - SHACKLE_C[1]
    ring = np.where(dy < 0, np.abs(np.hypot(dx, dy) - SHACKLE_R), np.abs(np.abs(dx) - SHACKLE_R))
    shackle = np.maximum(ring - SHACKLE_W, y - (BODY_C[1] - BODY_H[1] + 3.0))
    # the keyhole: a circle over a slot widening downwards
    t = np.clip((y - KEY_C[1]) / (KEY_FOOT - KEY_C[1]), 0, 1)
    slot = np.maximum(np.abs(x - KEY_C[0]) - (1.3 + 1.5 * t), np.maximum(KEY_C[1] - y, y - KEY_FOOT))
    key = np.minimum(np.hypot(x - KEY_C[0], y - KEY_C[1]) - KEY_R, slot)

    # heights, in icon pixels
    bevel = 3.2
    e = np.clip(-body / bevel, 0, 1)
    h_body = 3.0 + 2.4 * np.sin(e * np.pi / 2) ** 0.8
    # a slight cushion so the face isn't dead flat, and a groove under the top edge
    h_body += 1.7 * (1 - ((x - BODY_C[0]) / BODY_H[0]) ** 2) * (1 - ((y - BODY_C[1]) / BODY_H[1]) ** 2)
    groove_y = BODY_C[1] - BODY_H[1] + 5.4
    h_body -= 0.55 * np.exp(-((y - groove_y) / 0.55) ** 2) * (np.abs(x - BODY_C[0]) < BODY_H[0] - 3.2)
    # the keyhole is cut in, with a raised rim round it
    h_body += 0.5 * np.exp(-(np.maximum(key, 0) / 1.1) ** 2) * (key > 0)
    h_body -= 2.6 * np.clip(-key / 1.0, 0, 1)
    h_sh = 2.6 * np.sqrt(np.clip(1 - (ring / SHACKLE_W) ** 2, 0, 1))
    return body, shackle, key, h_body, h_sh


def _shade(h, tones, wear, rough=1.0):
    """A height field as chrome: RGB (float 0..255) from the slope against the light."""
    hi, mid, lo = tones
    gy, gx = np.gradient(_blur(h, SS * 0.35), 1.0 / SS)
    nrm = np.stack([-gx, -gy, np.ones_like(h)], -1)
    nrm /= np.linalg.norm(nrm, axis=-1, keepdims=True)
    lit = nrm @ (LIGHT / np.linalg.norm(LIGHT))                     # -1..1
    flat = LIGHT[2] / np.linalg.norm(LIGHT)                         # a level face's value
    # level faces are pale steel, slopes to the light burn out, slopes away fall to near black
    up = np.clip((lit - flat) / (1 - flat), 0, 1)
    down = np.clip((flat - lit) / (flat + 0.55), 0, 1)
    steel = mid + (hi - mid) * 0.70
    rgb = steel + (hi * 1.04 - steel) * (up ** 0.6)[..., None]
    rgb = rgb + (lo - rgb) * (down ** 0.8)[..., None]
    # chrome's dark horizon: a narrow dark band on faces just turned from the light
    band = np.exp(-((lit - (flat - 0.2)) / 0.07) ** 2) * 0.22
    rgb = rgb * (1 - band[..., None])
    return rgb * (1 + rough * wear)[..., None]


def build_lock(icons):
    """The padlock icon (64x64 RGBA) from the game's achievement icons ({name: image})."""
    byname = {str(k).rsplit('/', 1)[-1].removesuffix('.png').lower(): v for k, v in icons.items()}
    tiles = [np.asarray(byname[n].convert('RGB').resize((SIZE, SIZE)), float) for n in GROUND_TILES if n in byname]
    if len(tiles) < 5:
        raise ValueError('lock_art: needs the grey star tiles: ' + ', '.join(GROUND_TILES))
    ground, _, a = tile_ground(tiles)
    tones = metal_tones(a, ground)

    # the wear: the ground's fine detail, enlarged to the drawing's grid
    g = _lum(ground)
    detail = np.clip((g - _blur(g, 2.0)) / 70.0, -0.5, 0.5)
    wear = np.asarray(Image.fromarray(detail.astype(np.float32), 'F').resize((SIZE * SS,) * 2, Image.BICUBIC), float)
    wear = wear + 0.6 * np.roll(wear[::-1, ::-1], (11 * SS, 7 * SS), (0, 1))     # denser than the stone's

    body, shackle, key, h_body, h_sh = _shapes()
    px = 1.0 / SS

    def cover(sd):                                                   # antialiased inside
        return np.clip(0.5 - sd / px / 2, 0, 1)

    rgb_body = _shade(h_body, tones, wear * 0.30)
    rgb_sh = _shade(h_sh, tones, wear * 0.2)
    # a polished face: two slanted glints across the body
    yy, xx = (np.mgrid[0:SIZE * SS, 0:SIZE * SS] + 0.5) / SS
    u = (xx - BODY_C[0]) + 0.55 * (yy - BODY_C[1])
    glint = 0.16 * np.exp(-((u + 7.5) / 2.6) ** 2) + 0.09 * np.exp(-((u - 3.0) / 1.2) ** 2) - 0.10 * np.exp(-((u - 11.0) / 4.0) ** 2)
    rgb_body = rgb_body * (1 + glint)[..., None]
    # the keyhole's pit: dark, its far wall catching a little light
    pit = cover(key + 0.45)
    rgb_body = rgb_body + (tones[2] * 0.9 - rgb_body) * (pit * 0.86)[..., None]
    # the shackle drops into the body: darker where it meets it
    into = np.clip((np.mgrid[0:SIZE * SS, 0:SIZE * SS][0] / SS - (BODY_C[1] - BODY_H[1] - 3.5)) / 3.5, 0, 1)
    rgb_sh = rgb_sh * (1 - 0.55 * into[..., None])

    # dark outline: the characters' edge, about a pixel, round the whole silhouette and the body over the shackle
    out_c = tones[2] * 0.8
    edge_w = 1.15

    def layer(rgb, sd):
        al = cover(sd - edge_w)                                      # shape and its outline
        inner = cover(sd + 0.15)
        col = out_c + (rgb - out_c) * inner[..., None]
        return col, al

    col_s, al_s = layer(rgb_sh, shackle)
    col_b, al_b = layer(rgb_body, body)
    col = col_s * (1 - al_b[..., None]) + col_b * al_b[..., None]
    al = al_s + al_b - al_s * al_b
    # premultiplied, down to the icon's grid
    n = SIZE

    def down(v):
        return v.reshape(n, SS, n, SS, *v.shape[2:]).mean((1, 3))

    al64 = down(al)
    col64 = down(col * al[..., None]) / np.maximum(al64, 1e-6)[..., None]

    # the drop shadow on the stone
    sh = down(al)
    sh = np.roll(sh, (int(round(SHADOW[1])), int(round(SHADOW[0]))), (0, 1))
    sh = _blur(sh, 1.5)
    out = ground * (1 - 0.72 * sh[..., None])
    # a faint light rim on the stone up-left of the lock, as round the engraved characters
    rim = np.clip(_blur(np.roll(al64, (-1, -1), (0, 1)), 0.8) - al64, 0, 1)
    out = out + (tones[0] - out) * (0.10 * rim[..., None])
    out = out * (1 - al64[..., None]) + col64 * al64[..., None]
    # the icons are soft: take the drawing's edge off, keep the stone as it is
    soft = _blur(out, 0.42)
    m = _blur(np.maximum(al64, sh), 1.0)[..., None]
    out = out * (1 - m) + soft * m
    rgba = np.dstack([np.clip(out, 0, 255), np.full((n, n), 255.0)]).round().astype(np.uint8)
    return Image.fromarray(rgba, 'RGBA')


def greyed(im, dim=0.45):
    """An icon as a locked row shows it: without colour, dimmed."""
    a = np.asarray(im.convert('RGBA'), float)
    g = _lum(a[..., :3]) * dim
    return Image.fromarray(np.dstack([g, g, g, a[..., 3]]).round().astype(np.uint8), 'RGBA')


def preview(lock, icons, names):
    """The padlock among real icons at 64, 48 and 32, plain and greyed, on a dark ground."""
    row = [icons[n].convert('RGBA') for n in names]
    row.insert(len(row) // 2, lock)
    row.insert(0, lock)
    pad, scale_up = 8, 3
    widths = [64, 48, 32]
    w = pad + len(row) * (64 + pad)
    h = pad + sum(2 * (s + pad) for s in widths)
    sheet = Image.new('RGBA', (w, h + 64 * scale_up + 2 * pad), (16, 17, 26, 255))
    y = pad
    for s in widths:
        for grey in (False, True):
            for i, im in enumerate(row):
                p = im.resize((s, s), Image.LANCZOS) if s != 64 else im
                if grey:
                    p = greyed(p)
                sheet.alpha_composite(p, (pad + i * (64 + pad), y))
            y += s + pad
    x = pad
    for im in (lock, greyed(lock), row[2], row[3], row[-1]):
        sheet.alpha_composite(im.resize((64 * scale_up,) * 2, Image.NEAREST), (x, y))
        x += 64 * scale_up + pad
    return sheet


if __name__ == '__main__':
    import os
    import sys
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'out', 'lock')
    os.makedirs(dst, exist_ok=True)
    icons = {f[:-4]: Image.open(os.path.join(src, f)) for f in sorted(os.listdir(src)) if f.endswith('.png')}
    lock = build_lock(icons)
    lock.save(os.path.join(dst, 'lock.png'))
    names = sorted(icons)
    shown = [n for n in GROUND_TILES[3:6] if n in icons]
    others = [n for n in names if n not in GROUND_TILES]
    shown += [others[i] for i in np.linspace(4, len(others) - 1, 8).astype(int)]
    preview(lock, icons, shown).save(os.path.join(dst, 'preview.png'))
    print('wrote', os.path.join(dst, 'lock.png'), 'and preview.png')
