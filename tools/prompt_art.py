#!/usr/bin/env python3
"""The PlayStation, Nintendo and keyboard / mouse button prompts, made from the game's own button
atlas so they match its art: the same chrome plates and ovals, the same glossy discs, legends
engraved as the game engraves its plate letters.

Nothing of the game's art is stored here: everything is built at run time from the decoded atlas
handed in (512x256 RGBA). Needs Pillow and numpy; reads no files, uses no fonts (the letters are
pixel drawings: prompt_font.py and the small maps below).

    CELLS, MARGIN
    build_sets(atlas) -> {'kPlayStation': {cell: image}, 'kNintendo': {cell: image}}
    build_keys(atlas) -> {(name, shape): image}, shape in 'kSquare', 'kTall', 'kWide'
    reduce_colours(images, ...) -> the same pictures on a small shared palette

Every image is RGBA, (w + 2 * MARGIN, h + 2 * MARGIN): the cell with the margin the game's drop
shadow needs.
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

import prompt_font as kf

# The atlas's button cells (x, y, width, height), from its visible pixels.
CELLS = {
    'X': (13, 13, 39, 40), 'Y': (77, 13, 39, 40), 'B': (141, 13, 39, 40), 'A': (206, 13, 39, 40),
    'LT': (278, 10, 21, 44), 'RT': (342, 10, 21, 44), 'LB': (394, 20, 44, 24), 'RB': (460, 20, 43, 24),
    'LS': (76, 76, 40, 40), 'RS': (140, 76, 40, 40),
    'BACK': (396, 204, 41, 39), 'START': (460, 204, 40, 39),
}
MARGIN = 6
M = MARGIN
SS = 8  # supersampling for the drawn shapes
KEY_SHAPES = {'kSquare': (39, 40), 'kTall': (21, 44), 'kWide': (44, 24)}

# ---- pixel maps: a plate's brightness in tenths (a = untouched, b = the lit lip under a foot) ----
# The digits follow the letters they replace: the wide plates' B (dark second row, 3 px stem 8-6-7,
# fading towards the foot, lip below), the tall plates' T (stem core 4-5, two-row foot, no lip).
DIGIT_1 = ['aaaa9768aaaa', 'aaa86546aaaa', 'aaaa9867aaaa', 'aaaaa866aaaa', 'aaaaa867aaaa', 'aaaaa867aaaa',
           'aaaaa877aaaa', 'aaaaa877aaaa', 'aaaaa887aaaa', 'aaaaa988aaaa', 'aaaaa987aaaa', 'aaa9877689aa',
           'aaabbbbbbbaa']
DIGIT_2 = ['aaaa998899aaaa', 'aaa85455558aaa', 'aaa57aaa9647aa', 'aaa68aaaa856aa', 'aaaaaaaaa857aa',
           'aaaaaaaa9659aa', 'aaaaaaa9659aaa', 'aaaaaa9659aaaa', 'aaaaa9659aaaaa', 'aaaa9659aaa97a',
           'aaa855555556aa', 'aaa977777778aa']
# Z in the cut of the game's T and L on the tall plate: the T's top bar with its soft row above and
# its left serif, a 45-degree stroke with a 2 px dark core, the L's foot with its rising right serif.
LETTER_Z = ['a9998988988aaa', 'a86455554558aa', 'a855aaaa8546aa', 'a758aaa8549aaa', 'aaaaaa8549aaaa',
            'aaaaa8549aaaaa', 'aaaa8549aaaaaa', 'aaa8549aaaaaaa', 'aa8549aaaa989a', 'a8549aaaaa648a',
            'a854aaaa97559a', 'a55675444565aa', 'aa9999999999aa']
# atlas boxes (x0, y0, x1, y1 inclusive, the border letter-free)
BOX_LB_L, BOX_LB_B, BOX_LB = (404, 23, 416, 37), (416, 23, 427, 37), (404, 23, 427, 37)
BOX_RB_R, BOX_RB_B, BOX_RB = (469, 23, 481, 37), (481, 23, 491, 37), (469, 23, 491, 37)
TALL_X = {'LT': 281, 'RT': 345}     # the tall face's left column; rows 17..32 (first letter), 33..46 (second)
PS_COLORS = {'cross': (0.48, 0.66, 1.0), 'circle': (1.0, 0.36, 0.40),
             'square': (0.98, 0.52, 0.86), 'triangle': (0.30, 0.86, 0.66)}
NX_INK = (0.90, 0.90, 0.92)
# the game's label letters: rows 203..216 of the atlas (the shadow runs two rows lower)
LABEL_Y0, LABEL_H = 203, 14
GAME_LETTERS = {'S': (460, 467), 'T': (492, 501), 'C': (416, 424)}   # atlas x0, x1 (exclusive)
# the oval arrows' outlines with a 3 px margin (x in the cell's picture, atlas rows)
ARROW = {'BACK': [(33, 217.5), (13.5, 229.5), (33, 241.5)], 'START': [(16.5, 217.5), (37.5, 230), (16.5, 241.5)]}
MOUSE_RED = np.array([1.0, 0.16, 0.14])


def _shift(a, dx, dy):
    o = np.zeros_like(a)
    H, W = a.shape[:2]
    o[max(dy, 0):H + min(dy, 0), max(dx, 0):W + min(dx, 0)] = a[max(-dy, 0):H + min(-dy, 0), max(-dx, 0):W + min(-dx, 0)]
    return o


def _lum(c):
    return 0.299 * c[..., 0] + 0.587 * c[..., 1] + 0.114 * c[..., 2]


def _blur(a, sigma, scale=100.0):
    im = Image.fromarray(np.clip(a * scale, 0, 255).astype(np.uint8))
    return np.asarray(im.filter(ImageFilter.GaussianBlur(sigma))).astype(np.float32) / scale


def _coons(f):
    """A face without its letters, from its letter-free border rows and columns (shading kept both ways)."""
    v = np.linspace(0, 1, f.shape[0])[:, None, None]
    u = np.linspace(0, 1, f.shape[1])[None, :, None]
    return ((1 - v) * f[:1] + v * f[-1:] + (1 - u) * f[:, :1] + u * f[:, -1:]
            - ((1 - u) * (1 - v) * f[0, 0] + u * (1 - v) * f[0, -1] + (1 - u) * v * f[-1, 0] + u * v * f[-1, -1]))


def _tenths(rows):
    return np.array([[int(v, 16) / 10 for v in row] for row in rows], dtype=np.float32)


def _img(c):
    return Image.fromarray((np.clip(c, 0, 1) * 255).astype(np.uint8), 'RGBA')


class _Art:
    def __init__(self, atlas):
        self.A = np.asarray(atlas.convert('RGBA')).astype(np.float32) / 255.0
        self._disc()

    # ---- basics ----
    def crop(self, k):
        x, y, w, h = CELLS[k]
        return self.A[y - M:y + h + M, x - M:x + w + M].copy()

    def region(self, c, k, box):
        """The view of an atlas box (x0, y0, x1, y1 inclusive) in cell k's picture c (colour only)."""
        ox, oy = CELLS[k][0] - M, CELLS[k][1] - M
        x0, y0, x1, y1 = box
        return c[y0 - oy:y1 - oy + 1, x0 - ox:x1 - ox + 1, :3]

    def clean(self, c, k, box):
        reg = self.region(c, k, box)
        reg[...] = _coons(reg.copy())

    def mult(self, c, k, ratio, x, y):
        """Engrave: the picture's brightness times a ratio map, its corner at atlas (x, y)."""
        if ratio.ndim == 2:
            ratio = ratio[..., None]
        ox, oy = CELLS[k][0] - M, CELLS[k][1] - M
        reg = c[y - oy:y - oy + ratio.shape[0], x - ox:x - ox + ratio.shape[1], :3]
        reg[...] = np.clip(reg * ratio, 0, 1)

    # ---- the disc: the game's face buttons' lighting on a near-black glossy body ----
    def _disc(self):
        discs = [self.crop(k) for k in 'XYBA']
        ls = []
        for d in discs:
            l = _lum(d)
            ls.append(l / max(np.median(l[d[..., 3] > 0.5]), 1e-3))
        # a letter covers a pixel on at most two of the four discs: the median is the disc without them
        L = np.median(np.stack(ls), axis=0)
        alpha = np.median(np.stack([d[..., 3] for d in discs]), axis=0)
        self.h, self.w = h, w = L.shape
        yy, xx = np.mgrid[0:h, 0:w]
        # the disc's centre, measured: the opaque part's box (the cell also holds the shadow, down right)
        ys, xs = np.nonzero(alpha > 0.9)
        self.cx, self.cy = cx, cy = (xs.min() + xs.max()) / 2, (ys.min() + ys.max()) / 2
        R = ((xs.max() - xs.min() + 1) + (ys.max() - ys.min() + 1)) / 4   # the rim's outer edge
        r_body = R - 1.5                                                # where the chrome rim starts
        r = np.hypot(xx - cx, yy - cy)
        # What is left of the letters' relief goes: inside the body the light is the wide blur of the
        # body alone (the rim kept out of it), blended in from 2.5 px inside the rim.
        inside = (r < r_body - 0.5).astype(np.float32)
        smooth = _blur(L * inside, 2.6) / np.maximum(_blur(inside, 2.6), 1e-3)
        wgt = np.clip((r_body - 1.5 - r) / 2.5, 0, 1)
        L2 = L * (1 - wgt) + smooth * wgt
        self.L2 = L2
        body = np.clip(L2[..., None] * np.array([0.16, 0.16, 0.18]) * 1.25, 0, 1)
        spec = np.clip((L2 - 1.6) / 1.2, 0, 1)[..., None]     # the glass's highlights stay bright
        body = body * (1 - spec) + spec * np.array([0.92, 0.92, 0.95])
        rim = np.clip(r - r_body + 0.5, 0, 1)[..., None]
        grey_rim = np.mean(np.stack([_lum(d) for d in discs]), axis=0)[..., None] * np.ones(3)
        body = body * (1 - rim) + grey_rim * rim
        self.disc = np.dstack([body, alpha]).astype(np.float32)

    def emboss(self, mask, color):
        """The game's relief: the shape raised, lit from the top left, a dark edge and a cast shadow down right."""
        out = self.disc.copy()
        m = mask[..., None]
        up = _shift(mask, 1, 1)[..., None]
        dn = _shift(mask, -1, -1)[..., None]
        shade = np.clip(self.L2[..., None] * 0.30 + 0.72, 0.8, 1.3)
        fill = np.clip(np.array(color) * shade, 0, 1)
        fill = fill + np.clip(m - up, 0, 1) * 0.62 * (1 - fill)      # upper-left edges: lit
        fill = fill * (1 - np.clip(m - dn, 0, 1) * 0.62)             # lower-right edges: in shade
        cast = np.clip(np.maximum(_shift(mask, 1, 1), _shift(mask, 2, 2) * 0.55)[..., None] - m, 0, 1)
        halo = np.asarray(Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))).astype(np.float32)[..., None] / 255
        halo = np.clip(halo - m, 0, 1)
        out[..., :3] = out[..., :3] * (1 - np.clip(halo * 0.45 + cast * 0.75, 0, 0.92))
        out[..., :3] = out[..., :3] * (1 - m) + fill * m
        return out

    def symbol_mask(self, kind):
        """A PlayStation symbol's coverage, centred on the disc's measured centre."""
        w, h = self.w, self.h
        im = Image.new('L', (w * SS, h * SS), 0)
        d = ImageDraw.Draw(im)
        c, e = (self.cx + 0.5) * SS, (self.cy + 0.5) * SS
        q, t = 7.4 * SS, 2.8 * SS
        if kind == 'cross':
            for a in (1, -1):
                d.line([(c - q, e - a * q), (c + q, e + a * q)], fill=255, width=int(t * 1.1))
        elif kind == 'circle':
            k = q * 1.47
            d.ellipse([c - k, e - k, c + k, e + k], outline=255, width=int(t))
        elif kind == 'square':
            k = q * 1.31
            d.rectangle([c - k, e - k, c + k, e + k], outline=255, width=int(t))
        elif kind == 'triangle':
            # equilateral; the centre sits between its centroid and its box's middle, so it looks centred
            k = q * 1.50
            e2 = e + 1.1 * SS
            pts = [(c, e2 - k), (c + k * 0.866, e2 + k * 0.5), (c - k * 0.866, e2 + k * 0.5)]
            d.line(pts + [pts[0], pts[1]], fill=255, width=int(t), joint='curve')
        return np.asarray(im.resize((w, h), Image.BOX)).astype(np.float32) / 255.0

    def letter_mask(self, ch):
        """A, B, X, Y drawn stroke by stroke: one stroke weight, one height, flat terminals, open
        counters; centred on the disc's centre."""
        w, h = self.w, self.h
        S = SS
        im = Image.new('L', (w * S, h * S), 0)
        d = ImageDraw.Draw(im)
        ox, oy = (self.cx + 0.5) * S, (self.cy + 0.5) * S
        H, T = 7.0, 3.0                       # half height, stroke

        def P(x, y):
            return (ox + x * S, oy + y * S)

        def line(x0, y0, x1, y1, far=True):
            # run past the ends (far=False: only past the first); the clip below cuts the terminals flat
            dx, dy = x1 - x0, y1 - y0
            n = (dx * dx + dy * dy) ** 0.5
            ex, ey = dx / n * 3, dy / n * 3
            d.line([P(x0 - ex, y0 - ey), P(x1 + ex * far, y1 + ey * far)], fill=255, width=int(round(T * S)))

        def box(x0, y0, x1, y1):
            d.rectangle([P(x0, y0), P(x1, y1)], fill=255)

        if ch == 'A':
            line(0, -H - 1.2, -5.6, H)            # the legs meet above the top: a flat, 3 px wide apex
            line(0, -H - 1.2, 5.6, H)
            box(-3.4, 1.6, 3.4, 4.2)
        elif ch == 'X':
            line(-4.6, -H, 4.6, H)
            line(4.6, -H, -4.6, H)
        elif ch == 'Y':
            line(-5.2, -H, 0, 0.2, far=False)
            line(5.2, -H, 0, 0.2, far=False)
            box(-T / 2, -1.0, T / 2, H)
        elif ch == 'B':
            b = 2.6
            t = int(round(b * S))
            d.rounded_rectangle([P(-5.2, -H), P(4.4, b / 2)], radius=3.4 * S, outline=255, width=t)
            d.rounded_rectangle([P(-5.2, -b / 2), P(5.4, H)], radius=3.8 * S, outline=255, width=t)
            box(-5.2, -H, -5.2 + T, H)            # the stem, square corners on its side
            box(-5.2, -H, -1, -H + b)
            box(-5.2, H - b, -1, H)
            box(-5.2, -b / 2, -1, b / 2)
        m = np.asarray(im).astype(np.float32) / 255.0
        clip = np.zeros_like(m)
        clip[int(round(oy - H * S)):int(round(oy + H * S))] = 1
        return np.asarray(Image.fromarray((m * clip * 255).astype(np.uint8)).resize((w, h), Image.BOX)).astype(np.float32) / 255.0

    # ---- plates ----
    def ps_plate(self, k):
        """L1 / R1 / L2 / R2: the game's own L and R stay where they are, the second letter goes
        (its patch of chrome rebuilt), a digit comes in its place."""
        box, digit, (dx, dy) = {
            'LB': (BOX_LB_B, DIGIT_1, (415, 24)), 'RB': (BOX_RB_B, DIGIT_1, (479, 24)),
            'LT': ((281, 33, 294, 46), DIGIT_2, (281, 34)), 'RT': ((345, 33, 358, 46), DIGIT_2, (345, 34)),
        }[k]
        c = self.crop(k)
        self.clean(c, k, box)
        self.mult(c, k, _tenths(digit), dx, dy)
        return c

    def nx_wide(self, k):
        """L / R: the B goes, the game's own letter moves to the plate's middle."""
        lbox, allbox = {'LB': (BOX_LB_L, BOX_LB), 'RB': (BOX_RB_R, BOX_RB)}[k]
        c = self.crop(k)
        reg = self.region(c, k, lbox)
        ratio = reg / np.maximum(_coons(reg.copy()), 1e-3)
        self.clean(c, k, allbox)
        xs = np.nonzero((c[..., 3] > 0.9).any(0))[0]
        mid = CELLS[k][0] - M + (xs.min() + xs.max()) / 2
        ink = np.nonzero((ratio.mean(-1) < 0.93).any(0))[0]
        lmid = lbox[0] + (ink.min() + ink.max()) / 2
        self.mult(c, k, ratio, lbox[0] + int(round(mid - lmid)), lbox[1])
        return c

    def nx_tall(self, k):
        """ZL / ZR: the Z above, the game's own L / R below, stacked as the game's LT / RT."""
        x0 = TALL_X[k]
        # the clean upper face is LT's for both: RT's R touches its box's border columns, LT's L
        # doesn't, and the two plates are the same
        lt = self.region(self.crop('LT'), 'LT', (TALL_X['LT'], 17, TALL_X['LT'] + 13, 32))
        clean = _coons(lt.copy())
        c = self.crop(k)
        reg = self.region(c, k, (x0, 17, x0 + 13, 32))
        ratio = reg / np.maximum(clean, 1e-3)
        reg[...] = clean
        self.clean(c, k, (x0, 33, x0 + 13, 46))
        self.mult(c, k, ratio, x0, 17 + 15)     # the letter's rows 19..30 go to the T's rows 34..45
        self.mult(c, k, _tenths(LETTER_Z), x0, 19)
        return c

    # ---- ovals ----
    def clean_oval(self, k):
        """The oval without its arrow: each row's chrome carried across the arrow from the clean chrome
        on both sides of it (outside the arrow's own edge shading), then evened between rows."""
        c = self.crop(k)
        hh, ww = c.shape[:2]
        oy = CELLS[k][1] - M
        im = Image.new('L', (ww, hh), 0)
        ImageDraw.Draw(im).polygon([(x, y - oy) for x, y in ARROW[k]], fill=255)
        m = np.asarray(im) > 0
        solid = c[..., 3] > 0.97
        out = c.copy()
        for y in range(hh):
            xs = np.nonzero(m[y] & solid[y])[0]
            if not len(xs):
                continue
            x0, x1 = xs.min(), xs.max()
            la = [x for x in (x0 - 2, x0 - 1) if x >= 0 and solid[y, x]]
            ra = [x for x in (x1 + 1, x1 + 2) if x < ww and solid[y, x]]
            if not la or not ra:
                continue
            lv, rv = c[y, la, :3].mean(0), c[y, ra, :3].mean(0)
            t = ((np.arange(x0, x1 + 1) - (x0 - 1)) / (x1 - x0 + 2))[:, None]
            out[y, x0:x1 + 1, :3] = lv * (1 - t) + rv * t
        sm = out.copy()
        sm[1:-1, :, :3] = (out[:-2, :, :3] + 2 * out[1:-1, :, :3] + out[2:, :, :3]) / 4
        mm = m & solid
        mm[[0, -1]] = False
        out[mm] = sm[mm]
        return out

    def oval_centre(self, c):
        top = M + 15
        ys, xs = np.nonzero(c[top:, :, 3] > 0.97)
        return (xs.min() + xs.max()) / 2, top + (ys.min() + ys.max()) / 2

    def nx_oval(self, k, mark):
        """Minus / Plus: the mark engraved pixel by pixel on the oval's centre; no label (the Switch has none)."""
        c = self.clean_oval(k)
        hh, ww = c.shape[:2]
        c[:M + 15, :, 3] = 0
        gx, gy = self.oval_centre(c)
        x0, y0 = int(gx - 5.5), int(gy - 0.5)
        ink = np.zeros((hh, ww), bool)
        if mark == 'minus':
            ink[y0:y0 + 2, x0:x0 + 12] = True          # 12 x 2
        else:
            ink[y0:y0 + 2, x0 + 1:x0 + 11] = True      # 10 x 10, arms 2 thick
            ink[y0 - 4:y0 + 6, x0 + 5:x0 + 7] = True
        lip = (_shift(ink.astype(np.float32), 1, 1) > 0.5) & ~ink     # the cut's lit edge, down right
        c[..., :3] = np.where(lip[..., None], np.clip(c[..., :3] * 1.12 + 0.03, 0, 1), c[..., :3])
        c[..., :3] = np.where(ink[..., None], c[..., :3] * 0.40, c[..., :3])
        return c

    def ps_oval(self, k):
        """SELECT / START: the PS3's rectangle / triangle on the oval; START keeps the game's own label,
        BACK's becomes SELECT in the game's label letters."""
        src = self.crop(k)
        c = self.clean_oval(k)
        hh, ww = c.shape[:2]
        top = M + 13
        c[:top, :, 3] = 0
        gx = (self.oval_centre(src)[0] + 0.5) * SS
        gy = (top + (hh - top) / 2 - 3) * SS
        im = Image.new('L', (ww * SS, hh * SS), 0)
        d = ImageDraw.Draw(im)
        if k == 'START':
            d.polygon([(gx - 4 * SS, gy - 4.5 * SS), (gx + 5 * SS, gy), (gx - 4 * SS, gy + 4.5 * SS)], fill=255)
        else:
            d.rectangle([gx - 5 * SS, gy - 2.5 * SS, gx + 5 * SS, gy + 2.5 * SS], fill=255)
        m = np.asarray(im.resize((ww, hh), Image.LANCZOS)).astype(np.float32)[..., None] / 255
        c[..., :3] = c[..., :3] * (1 - m) + np.array([0.30, 0.30, 0.32]) * m
        if k == 'START':
            c[:top] = src[:top]
            return c
        lab = self.label('SELECT')
        lw = lab.shape[1]
        ox = int(round(self.oval_centre(src)[0] + 0.5 - lw / 2))
        y0 = LABEL_Y0 - (CELLS[k][1] - M)
        reg = c[y0:y0 + lab.shape[0], ox:ox + lw]
        la, ra = lab[..., 3:4], reg[..., 3:4]
        oa = la + ra * (1 - la)
        reg[..., :3] = (lab[..., :3] * la + reg[..., :3] * ra * (1 - la)) / np.maximum(oa, 1e-6)
        reg[..., 3:4] = oa
        return c

    # ---- the label letters: the game's own S, T, C; E and L built from its B, T and K ----
    def _col(self, x, rows=slice(None)):
        return self.A[LABEL_Y0:LABEL_Y0 + LABEL_H, x][rows].copy()

    def _label_built(self, ch):
        """E and L from the game's glyph pixels: the B's stem (with its slab serifs), the T's bar end as
        the top arm, the B's own middle bar; the foot is the T's bar end mirrored, re-toned for its rows."""
        A, g = self.A, self._col
        t = np.zeros((LABEL_H, 8, 4), np.float32)
        for i in range(4):
            t[:, i] = g(400 + i)
        arm = np.stack([g(497 + i) for i in range(4)], axis=1)
        if ch == 'E':
            t[:6, 4:8] = arm[:6]
            t[6:8, 4] = g(404, slice(6, 8))
            t[6:8, 5] = g(405, slice(6, 8))
            t[6:8, 6, 3] = g(406, slice(6, 8))[:, 3] * 0.55
            t[6:8, 6, :3] = g(405, slice(6, 8))[:, :3] * 0.6
            t[5, 4:6] = g(404, slice(5, 6))[0] * np.array([1, 1, 1, 0.5])
            t[8, 4:6] = g(404, slice(8, 9))[0] * np.array([1, 1, 1, 0.5])
        else:
            t[:3, 4] = g(427, slice(0, 3))
            t[6:9, 3] = t[5, 3]
        # per label row, the game letters' brightness against coverage: for the mirrored foot
        ref = np.concatenate([A[LABEL_Y0:LABEL_Y0 + LABEL_H, 400:433], A[LABEL_Y0:LABEL_Y0 + LABEL_H, 460:501]], axis=1)
        for j in range(5):
            dst, src = LABEL_H - 1 - j, 1 + j
            a, l = ref[dst, :, 3], _lum(ref[dst])
            mk = a > 0.15
            fit = np.polyfit(a[mk], l[mk], 1) if mk.sum() > 4 else np.array([0.0, 0.0])
            al = arm[src, :, 3]
            t[dst, 4:8, 3] = al
            tone = np.clip(np.polyval(fit, al), 0, 1)
            t[dst, 4:8, :3] = np.where(al[:, None] > 0.4, tone[:, None], 0.0)
        return t

    def label(self, text):
        tiles = []
        for ch in text:
            if ch in GAME_LETTERS:
                x0, x1 = GAME_LETTERS[ch]
                t = self.A[LABEL_Y0:LABEL_Y0 + LABEL_H, x0:x1].copy()
                if ch == 'C':   # the A's foot beside it
                    t[[0, 1, 2, 12, 13], 0, 3] = 0
            else:
                t = self._label_built(ch)
            tiles.append(t)
        kern = [-1 if ch == 'L' else 0 for ch in text]   # the L's open right side
        wd = sum(t.shape[1] for t in tiles) + sum(kern[:-1])
        lab = np.zeros((LABEL_H + 4, wd + 4, 4), np.float32)
        x = 0
        for t, kn in zip(tiles, kern):
            reg = lab[:LABEL_H, x:x + t.shape[1]]
            reg[...] = np.where((t[..., 3] > reg[..., 3])[..., None], t, reg)
            x += t.shape[1] + kn
        # the shadow, fitted to the game's own under BACK and START
        solid = ((lab[..., 3] > 0.3) & (_lum(lab) > 0.02)).astype(np.float32)
        lab[..., 3] = np.maximum(lab[..., 3], _blur(_shift(solid, 2, 2), 1.6, 255.0) * 0.32)
        return lab

    # ---- the sets ----
    def sets(self):
        stick = {'LS': self.crop('LS'), 'RS': self.crop('RS')}
        ps = {
            'A': self.emboss(self.symbol_mask('cross'), PS_COLORS['cross']),
            'B': self.emboss(self.symbol_mask('circle'), PS_COLORS['circle']),
            'X': self.emboss(self.symbol_mask('square'), PS_COLORS['square']),
            'Y': self.emboss(self.symbol_mask('triangle'), PS_COLORS['triangle']),
            **{k: self.ps_plate(k) for k in ('LB', 'RB', 'LT', 'RT')},
            'BACK': self.ps_oval('BACK'), 'START': self.ps_oval('START'), **stick,
        }
        nx = {  # by position: the atlas's A cell shows the Switch's B, ...
            'A': self.emboss(self.letter_mask('B'), NX_INK), 'B': self.emboss(self.letter_mask('A'), NX_INK),
            'X': self.emboss(self.letter_mask('Y'), NX_INK), 'Y': self.emboss(self.letter_mask('X'), NX_INK),
            'LB': self.nx_wide('LB'), 'RB': self.nx_wide('RB'), 'LT': self.nx_tall('LT'), 'RT': self.nx_tall('RT'),
            'BACK': self.nx_oval('BACK', 'minus'), 'START': self.nx_oval('START', 'plus'), **stick,
        }
        out = {}
        for name, cells in (('kPlayStation', ps), ('kNintendo', nx)):
            out[name] = {}
            for k in CELLS:
                c = cells[k]
                assert c.shape[:2] == (CELLS[k][3] + 2 * M, CELLS[k][2] + 2 * M), (name, k, c.shape)
                out[name][k] = _img(c)
        return out

    # ---- keys: chrome caps from the plates ----
    def _caps(self):
        tall = self.crop('LT')
        self.clean(tall, 'LT', (281, 17, 294, 46))
        l, r = self.crop('LB'), self.crop('RB')
        self.clean(l, 'LB', BOX_LB)
        self.clean(r, 'RB', BOX_RB)
        # the wide cap: LB's rounded left end and RB's rounded right end, joined in the middle
        w = KEY_SHAPES['kWide'][0] + 2 * M
        r = np.pad(r, ((0, 0), (w - r.shape[1], 0), (0, 0))) if r.shape[1] < w else r[:, r.shape[1] - w:]
        t = np.clip((np.arange(w) - (w / 2 - 4)) / 8, 0, 1)[None, :, None]
        wide = l[:, :w] * (1 - t) + r * t
        self.tall = tall
        # each cap: the picture and its face's centre (x, y) in it
        self.caps = {'kTall': (tall, (15.5, 27.5)), 'kWide': (wide, (27.5, 16.0)),
                     'kSquare': (self.nine(39 + 2 * M, 40 + 2 * M), (24.5, 25.5))}
        lb, rb, lt = self.crop('LB'), self.crop('RB'), self.crop('LT')
        self.game = {'L': self._game_letter(lb, 'LB', BOX_LB_L), 'B': self._game_letter(lb, 'LB', BOX_LB_B),
                     'R': self._game_letter(rb, 'RB', BOX_RB_R), 'T': self._game_letter(lt, 'LT', (281, 33, 294, 46))}
        self.game_tall = {'L': self._game_letter(lt, 'LT', (281, 17, 294, 32)), 'T': self.game['T']}

    def nine(self, w, h, xs=(10, 22), ys=(12, 44)):
        """The tall plate resized to (w, h) keeping its ends (corners, bevel, shadow): the columns and
        rows between are resampled (nearest: no new tones, the face's fade kept)."""
        src = self.tall
        H, W = src.shape[:2]

        def idx(n, N, a, b):
            mid = n - a - (N - b)
            return np.concatenate([np.arange(a), a + np.floor((np.arange(mid) + 0.5) * (b - a) / mid).astype(int), np.arange(b, N)])
        return src[idx(h, H, *ys)][:, idx(w, W, *xs)].copy()

    def _game_letter(self, c, k, box):
        """One of the game's own plate letters as a ratio map, trimmed to its ink."""
        reg = self.region(c, k, box)
        r = (reg / np.maximum(_coons(reg.copy()), 1e-3)).mean(-1)
        ys, xs = np.nonzero(np.abs(r - 1) > 0.045)
        return r[ys.min():ys.max() + 1, xs.min():xs.max() + 1]

    def legend(self, spec, shape):
        """The legend as ratio maps to stack (top to bottom)."""
        kind, text = spec
        lip = shape != 'kTall'
        if kind == 'reg':
            if len(text) == 1 and text in self.game and shape != 'kSquare':
                return [self.game_tall.get(text, self.game[text]) if shape == 'kTall' else self.game[text]]
            if shape == 'kTall' and len(text) > 1:
                font, adv = (kf.REG, 9) if len(text) == 2 else (kf.CON, 7)
                return [engrave(skeleton(ch, font, adv), False) for ch in text]
            if shape == 'kSquare' and len(text) <= 2:
                return [engrave_big(skeleton(text, kf.REG, 9), 2.0 if len(text) == 1 else 1.5)]
            return [engrave(skeleton(text, kf.REG, 9), lip)]
        if shape == 'kTall':
            return [engrave(skeleton(ch, kf.CON, 7), False) for ch in text]
        return [engrave(skeleton(text, kf.CON, 7), lip)]

    def mouse(self, shape, button):
        """A chrome body from the tall plate, the pressed button (or the wheel) in red."""
        w, h = {'kSquare': (25, 38), 'kTall': (21, 44), 'kWide': (15, 22)}[shape]
        b = self.nine(w + 2 * M, h + 2 * M)
        solid = b[..., 3] > 0.9
        xc = M + (w - 1) // 2
        top, bot = M + 3, M + h - 5                 # the face's rows
        ys = top + int(round((bot - top) * 0.40))   # the buttons end here
        wy0, wy1 = top + max(1, (ys - top) // 5), ys - max(2, (ys - top) // 4)
        if button in 'LR':
            cols = slice(M + 1, xc) if button == 'L' else slice(xc + 1, M + w - 1)
            reg = b[M + 1:ys, cols, :3]
            reg[...] = np.where(solid[M + 1:ys, cols, None], reg * MOUSE_RED, reg)
        line = np.zeros(b.shape[:2], bool)
        line[ys, M + 3:M + w - 3] = True
        line[top:ys, xc] = True
        wheel = np.zeros(b.shape[:2], bool)
        wheel[wy0:wy1 + 1, xc - 1:xc + 2] = True
        line &= ~wheel
        b[..., :3] = np.where(line[..., None], b[..., :3] * 0.55, b[..., :3])
        lipm = _shift(line.astype(np.float32), 1, 1) > 0.5
        b[..., :3] = np.where((lipm & ~line & ~wheel)[..., None], np.clip(b[..., :3] * 1.08, 0, 1), b[..., :3])
        wf = wheel.astype(np.float32)
        edge = wheel & ~(_shift(wf, 0, 1) > 0.5) | wheel & ~(_shift(wf, 0, -1) > 0.5)
        edge[wy0:wy1 + 1, xc - 1] = True
        edge[wy0:wy1 + 1, xc + 1] = True
        if button == 'M':
            b[..., :3] = np.where(wheel[..., None], b[..., :3] * MOUSE_RED, b[..., :3])
        b[..., :3] = np.where((edge & wheel)[..., None], b[..., :3] * 0.5, b[..., :3])
        full = np.zeros((KEY_SHAPES[shape][1] + 2 * M, KEY_SHAPES[shape][0] + 2 * M, 4), np.float32)
        oy, ox = (full.shape[0] - b.shape[0]) // 2, (full.shape[1] - b.shape[1]) // 2
        full[oy:oy + b.shape[0], ox:ox + b.shape[1]] = b
        return full

    def key(self, name, shape):
        spec = KEYS[name]
        if isinstance(spec, str) and spec.startswith('mouse'):
            return self.mouse(shape, spec[-1])
        cap, (cx, cy) = self.caps[shape]
        c = cap.copy()
        if spec is None:
            return c
        if isinstance(spec, str):
            _put(c, arrow_map(spec, shape == 'kSquare'), cx, cy)
            return c
        maps = self.legend(spec[shape], shape)
        gap = 1 if len(maps) > 2 else 3
        y = cy - (sum(m.shape[0] for m in maps) + gap * (len(maps) - 1)) / 2
        for m in maps:
            _put(c, m, cx, y + m.shape[0] / 2)
            y += m.shape[0] + gap
        return c

    def keys(self):
        self._caps()
        return {(n, s): self.key(n, s) for n in KEYS for s in KEY_SHAPES}


# ---- legends: engraved as the game's plate letters ----
def engrave(core, lip=True):
    """A stroke skeleton (bool) as the plates' engraving, as the plate's brightness ratio. Row by row:
    a stroke 1-2 pixels wide (a stem or a diagonal's step) is a dark core with a softer right side and a
    faint left one (the game's 8-6-7), so stems and diagonals get the same weight; a longer run is a bar,
    darker, with a light edge above; two strokes one pixel apart keep that pixel clean. The lit lip goes
    under the foot (wide plates only). One row and column of room is added around."""
    c = np.pad(core.astype(bool), 2)
    d = np.zeros(c.shape, np.float32)
    for y in range(c.shape[0]):
        xs = np.nonzero(c[y])[0]
        if not len(xs):
            continue
        for r in np.split(xs, np.nonzero(np.diff(xs) > 1)[0] + 1):
            a, b = r[0], r[-1]
            if len(r) >= 3:
                d[y, a:b + 1] = 0.48
                up = ~c[y - 1, a:b + 1] & (d[y - 1, a:b + 1] == 0)
                d[y - 1, a:b + 1][up] = 0.08
            else:
                d[y, a:b + 1] = 0.40
                if not c[y, b + 2]:
                    d[y, b + 1] = max(d[y, b + 1], 0.28)
                if not c[y, a - 2] and d[y, a - 1] == 0:
                    d[y, a - 1] = 0.16
    d[c & (d < 0.4)] = 0.40
    r = 1 - d
    if lip:
        r[-2][c[-3] & ~c[-2]] = 1.1
    return r[1:-1, 1:-1]


def engrave_big(core, k):
    """The same cut k times larger, for the square cap's single characters: the skeleton enlarged
    (smooth), strokes k pixels wide, the same core / softer right side / lit lip."""
    c = np.pad(core.astype(np.float32), 2)
    im = Image.fromarray((c * 255).astype(np.uint8)).resize((round(c.shape[1] * k), round(c.shape[0] * k)), Image.BICUBIC)
    cov = np.clip(np.asarray(im).astype(np.float32) / 255 * 1.35, 0, 1)
    d = 0.42 * cov + 0.24 * np.clip(_shift(cov, 1, 0) - cov, 0, 1)
    lip = 0.08 * np.clip(_shift(cov, 1, 1) - cov - _shift(cov, 1, 0), 0, 1)
    return 1 - d + lip


def skeleton(text, font, adv):
    g = [np.array([[ch == '#' for ch in row] for row in font[c]]) for c in text]
    out = np.zeros((g[0].shape[0], adv * (len(g) - 1) + g[-1].shape[1]), bool)
    for i, a in enumerate(g):
        out[:, i * adv:i * adv + a.shape[1]] |= a
    return out


def _put(c, r, cx, cy):
    y = int(round(cy + 0.5 - r.shape[0] / 2))
    x = int(round(cx + 0.5 - r.shape[1] / 2))
    reg = c[y:y + r.shape[0], x:x + r.shape[1], :3]
    reg[...] = np.clip(reg * r[..., None], 0, 1)


# Arrows: one pixel drawing per size (pointing up), the other three its exact rotations; engraved
# solid in the legends' ink with the lit edge down right.
def _arrow_up(head, shaft_w, shaft_len):
    m = np.zeros((head + shaft_len, 2 * head - 1), bool)
    for i in range(head):
        m[i, head - 1 - i:head + i] = True
    m[head:, head - 1 - shaft_w // 2:head + shaft_w // 2] = True
    return m


ARROW_BIG = _arrow_up(8, 5, 7)      # 15 x 15, the square cap
ARROW_SMALL = _arrow_up(5, 3, 5)    # 9 x 10, the wide and tall caps
TURNS = {'up': 0, 'left': 1, 'down': 2, 'right': 3}   # quarter turns (counter-clockwise)


def arrow_map(direction, big):
    ink = np.pad(np.rot90(ARROW_BIG if big else ARROW_SMALL, TURNS[direction]), 1)
    f = ink.astype(np.float32)
    r = np.ones(ink.shape, np.float32)
    r[(_shift(f, 1, 1) > 0.5) & ~ink] = 1.1
    r[ink] = 0.52
    r[ink & ~((_shift(f, 1, 0) > 0.5) & (_shift(f, 0, 1) > 0.5))] = 0.44   # the cut's wall in shade, up left
    return r


# ---- the keys (the names of tools/make_prompts.py's KEY_NAMES, and '?': a key without a legend) ----
def _word(wide, square=None, tall=None):
    return {'kWide': ('word', wide), 'kSquare': ('word', square or wide), 'kTall': ('word', tall or square or wide)}


def _reg(text):
    return {s: ('reg', text) for s in KEY_SHAPES}


KEYS = {
    **{c: _reg(c) for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'},
    **{f'F{i}': _reg(f'F{i}') for i in range(1, 25)},
    **{f'Numpad{i}': _reg(str(i)) for i in range(10)},
    'Escape': _word('ESC'), 'Return': _word('ENTER', 'ENT'), 'NumpadEnter': _word('ENTER', 'ENT'),
    'Space': _word('SPACE', 'SPC'), 'Tab': _word('TAB'), 'Backspace': _word('BKSP', 'BKSP', 'BSP'),
    'Delete': _word('DEL'), 'Insert': _word('INS'), 'Home': _word('HOME', 'HOME', 'HOM'), 'End': _word('END'),
    'PageUp': _word('PGUP', 'PGUP', 'PGU'), 'PageDown': _word('PGDN', 'PGDN', 'PGD'),
    'Left': 'left', 'Right': 'right', 'Up': 'up', 'Down': 'down',
    'Shift': _word('SHIFT', 'SHFT', 'SFT'), 'Control': _word('CTRL', 'CTRL', 'CTL'), 'Alt': _word('ALT'),
    'CapsLock': _word('CAPS', 'CAPS', 'CAP'), 'NumLock': _word('NUM'), 'PrintScreen': _word('PRTSC', 'PRT'),
    'Pause': _word('PAUSE', 'PAUS', 'PAU'), 'ScrollLock': _word('SCRLK', 'SCRL', 'SCR'),
    'Backtick': _reg('`'), 'Minus': _reg('-'), 'Plus': _reg('+'), 'NumpadPlus': _reg('+'), 'NumpadMinus': _reg('-'),
    'NumpadStar': _reg('*'), 'NumpadSlash': _reg('/'), 'Slash': _reg('/'), 'Backslash': _reg('\\'), 'Semicolon': _reg(';'),
    'Quote': _reg("'"), 'LBracket': _reg('['), 'RBracket': _reg(']'), 'Comma': _reg(','), 'Period': _reg('.'),
    'LMB': 'mouseL', 'RMB': 'mouseR', 'MMB': 'mouseM', '?': None,
}


# ---- a small shared palette ----
def _lloyd(v, wt, k, it=30):
    """k centres for the rows of v (weights wt): k-means from spread starts (deterministic)."""
    order = np.argsort(v[:, 0] + v[:, -1] * 1e-3, kind='stable')
    c = v[order[np.linspace(0, len(v) - 1, k).astype(int)]].astype(np.float64)
    for _ in range(it):
        a = ((v[:, None, :] - c[None]) ** 2).sum(-1).argmin(1)
        for j in range(k):
            m = a == j
            if m.any():
                c[j] = (v[m] * wt[m, None]).sum(0) / wt[m].sum()
    return c


def reduce_colours(images, n_grey=40, n_colour=9, n_soft=13):
    """The pictures (a list of RGBA images) on one palette of at most n_grey opaque greys + n_colour
    opaque colours + n_soft half-transparent tones (62 by default: what the atlas palette has free).
    Pixels under alpha 12 become fully transparent. Returns (images, palette as an (n, 4) array)."""
    return _reduce([np.asarray(im.convert('RGBA')).astype(np.int32) for im in images], n_grey, n_colour, n_soft)


def _reduce(arrs, n_grey=40, n_colour=9, n_soft=13):
    """reduce_colours on integer arrays (values may pass 255 where a rebuilt face overshoots; they
    count as their own tones and end at 255, as in the approved pictures)."""
    px = np.concatenate([a.reshape(-1, 4) for a in arrs])
    px = px[px[:, 3] >= 12]
    u, cnt = np.unique(px, axis=0, return_counts=True)
    sat = u[:, :3].max(1) - u[:, :3].min(1)
    opaque = u[:, 3] >= 250
    pal = []
    for m, k, grey in ((opaque & (sat < 24), n_grey, True), (opaque & (sat >= 24), n_colour, False), (~opaque, n_soft, False)):
        if not m.any() or k < 1:
            continue
        v = u[m].astype(np.float64)
        wt = cnt[m].astype(np.float64)
        if grey:
            g = v[:, :3].mean(1, keepdims=True)
            pal += [(x, x, x, 255.0) for x in _lloyd(g, wt, min(k, len(np.unique(g))))[:, 0]]
        else:
            if opaque[m].all():
                v[:, 3] = 255
            pal += [tuple(x) for x in _lloyd(v, wt, min(k, len(np.unique(v, axis=0))))]
    pal = np.unique(np.round(np.array(pal)).astype(np.int32), axis=0)
    out = []
    for a in arrs:
        uu, inv = np.unique(a.reshape(-1, 4), axis=0, return_inverse=True)
        near = pal[(((uu[:, None, :] - pal[None]) ** 2) * np.array([1, 1, 1, 3])).sum(-1).argmin(1)]
        near[uu[:, 3] < 12] = 0
        out.append(Image.fromarray(np.clip(near[inv.reshape(-1)].reshape(a.shape), 0, 255).astype(np.uint8), 'RGBA'))
    return out, pal


# ---- the API ----
def build_sets(atlas):
    """{'kPlayStation': {cell: image}, 'kNintendo': {cell: image}} for all 12 cells of CELLS; LS and RS
    are the game's own stick cells, unchanged. Full colour: reduce each set with reduce_colours or the
    caller's own quantizer."""
    return _Art(atlas).sets()


def build_keys(atlas):
    """{(name, shape): image} for every key of KEYS in the three shapes of KEY_SHAPES, the key centred.
    The whole set shares one palette of at most 62 colours (reduce_colours' defaults)."""
    raw = _Art(atlas).keys()
    names = list(raw)
    for (n, s), c in raw.items():
        assert c.shape[:2] == (KEY_SHAPES[s][1] + 2 * M, KEY_SHAPES[s][0] + 2 * M), (n, s, c.shape)
    reduced, _ = _reduce([np.round(raw[k] * 255).astype(np.int32) for k in names])
    return dict(zip(names, reduced))
