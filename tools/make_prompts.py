#!/usr/bin/env python3
"""Makes src/gpu/prompt_sets.inc: the PlayStation, Nintendo and keyboard button
prompts for the game's button atlas (src/gpu/prompts.cpp).

usage: make_prompts.py <dump dir> [<repo>]

<dump dir> holds the game's own atlas and palette as the port loads them
(atlas.bin, palette.bin), written by a run with NG2_DUMP_PROMPT_ATLAS=<dir>
once the atlas is loaded (in a stage: the HUD's buttons, e.g. NG2_CHAPTER=1
and the New Game pad script in AGENTS.md).

The pictures are made from the game's own buttons by tools/prompt_art.py.
Each set is reduced to at most 62 colours over all its cells (the atlas palette
has 62 entries free once those cells are cleared; index 0 is transparent),
and shipped masked: each picture's indices and each colour (as stored: B, G,
R, A) XORed with a ChaCha20 stream (RFC 8439) under a 256-bit key hashed from
the game's own atlas (the colours: its palette), never written down.
prompts.cpp hashes the game's textures as it loads them and unmasks them;
without the game the data is noise. The sticks (LS, RS) aren't shipped: the
PlayStation and Nintendo sets keep the game's own.
Needs Pillow, numpy and xxhash.
"""
import os
import re
import sys

import numpy as np
import xxhash
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prompt_art  # noqa: E402

DUMP = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
ROOT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..')
ATLAS_W, ATLAS_H = 512, 256
ATLAS_HASH, PALETTE_HASH = 0xEAF07C3B5DAB0BA1, 0xD1C855C91D4CEB65
CELLS = prompt_art.CELLS
M = prompt_art.MARGIN
ORDER = ['X', 'Y', 'B', 'A', 'LT', 'RT', 'LB', 'RB', 'LS', 'RS', 'BACK', 'START']
KEY_SHAPES = {'kSquare': (39, 40), 'kTall': (21, 44), 'kWide': (44, 24)}
# The masks' streams (prompts.cpp): a set's colours = its number, its cell c =
# number * 16 + c; the keyboard's colours = KEY_STREAM, key k's shape s
# (square, tall, wide) = 0x1000 + k * 4 + s.
SET_STREAMS = {'kPlayStation': 1, 'kNintendo': 2}
KEY_STREAM = 3
# The masks' key: XXH3-128 hashes of the game's texture with this seed and the next (prompts.cpp's kMaskSeed).
MASK_SEED = 0x4E47325052
# Cells the PlayStation and Nintendo sets leave as the game's.
GAME_CELLS = ('LS', 'RS')
# Every key the SDK names (rex/ui keybinds); prompts.cpp picks the bound key's picture.
KEY_NAMES = (
    list('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') + [f'F{i}' for i in range(1, 25)] +
    [f'Numpad{i}' for i in range(10)] +
    ['Escape', 'Return', 'NumpadEnter', 'Space', 'Tab', 'Backspace', 'Delete', 'Insert', 'Home', 'End',
     'PageUp', 'PageDown', 'Left', 'Right', 'Up', 'Down', 'Shift', 'Control', 'Alt', 'CapsLock', 'NumLock',
     'PrintScreen', 'Backtick', 'Minus', 'Plus', 'NumpadPlus', 'NumpadMinus', 'NumpadStar', 'NumpadSlash',
     'Slash', 'Backslash', 'Semicolon', 'Quote', 'LBracket', 'RBracket', 'Comma', 'Period', 'Pause', 'ScrollLock',
     'LMB', 'RMB', 'MMB'])


def free_entries():
    src = open(os.path.join(ROOT, 'src', 'gpu', 'prompts.cpp')).read()
    body = re.search(r'kFree\[\] = \{([^}]*)\}', src).group(1)
    return [int(v) for v in body.replace('\n', ' ').split(',') if v.strip()]


def load_game():
    atlas = open(os.path.join(DUMP, 'atlas.bin'), 'rb').read()[:ATLAS_W * ATLAS_H]
    palette = open(os.path.join(DUMP, 'palette.bin'), 'rb').read()[:1024]
    if xxhash.xxh3_64_intdigest(atlas) != ATLAS_HASH or xxhash.xxh3_64_intdigest(palette) != PALETTE_HASH:
        sys.exit('not the game\'s atlas and palette (hash differs)')
    pal = np.frombuffer(palette, np.uint8).reshape(256, 4)[:, [2, 1, 0, 3]]  # stored B, G, R, A
    rgba = pal[np.frombuffer(atlas, np.uint8)].reshape(ATLAS_H, ATLAS_W, 4)
    return atlas, palette, Image.fromarray(rgba, 'RGBA')


def key_of(data):
    """prompts.cpp's KeyOf(): 8 words from XXH3-128 of the texture, seeds MASK_SEED and MASK_SEED + 1."""
    words = []
    for h in range(2):
        v = xxhash.xxh3_128_intdigest(data, seed=MASK_SEED + h)
        lo, hi = v & ((1 << 64) - 1), v >> 64
        words += [lo & 0xFFFFFFFF, lo >> 32, hi & 0xFFFFFFFF, hi >> 32]
    return words


def chacha_blocks(state):
    """ChaCha20 blocks for states (16, n) uint32: (n, 64) bytes."""
    x = state.copy()

    def rotl(v, c):
        return (v << np.uint32(c)) | (v >> np.uint32(32 - c))

    def quarter(a, b, c, d):
        x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 16)
        x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 12)
        x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 8)
        x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 7)
    for _ in range(10):
        quarter(0, 4, 8, 12); quarter(1, 5, 9, 13); quarter(2, 6, 10, 14); quarter(3, 7, 11, 15)
        quarter(0, 5, 10, 15); quarter(1, 6, 11, 12); quarter(2, 7, 8, 13); quarter(3, 4, 9, 14)
    x += state
    return np.ascontiguousarray(x.T).astype('<u4').view(np.uint8).reshape(-1, 64)


def mask(key, stream, n):
    """prompts.cpp's Mask: n bytes of ChaCha20 stream `stream` (its nonce) under `key` (8 words)."""
    blocks = (n + 63) // 64
    state = np.zeros((16, blocks), np.uint32)
    state[0:4] = np.array([0x61707865, 0x3320646E, 0x79622D32, 0x6B206574], np.uint32)[:, None]
    state[4:12] = np.array(key, np.uint32)[:, None]
    state[12] = np.arange(blocks, dtype=np.uint32)
    state[14] = stream & 0xFFFFFFFF
    state[15] = stream >> 32
    with np.errstate(over='ignore'):
        return chacha_blocks(state).ravel()[:n]


def reduce(pictures):
    """[(key, image)] -> colours (RGBA, at most 62) and each picture's indices (0: clear, k + 1)."""
    seen = {}
    for _, p in pictures:
        a = np.asarray(p).reshape(-1, 4)
        for c in map(tuple, np.unique(a[a[:, 3] >= 12], axis=0)):
            seen.setdefault(c, len(seen))
    if len(seen) <= 62:  # already reduced (the keys): every colour as it is
        colors = [tuple(int(v) for v in c) for c in seen]
        out = {}
        for key, p in pictures:
            a = np.asarray(p)
            data = np.zeros(a.shape[:2], np.uint8)
            for y, x in zip(*np.nonzero(a[:, :, 3] >= 12)):
                data[y, x] = seen[tuple(a[y, x])] + 1
            out[key] = data
        return colors, out
    sheet_w = sum(p.width for _, p in pictures)
    sheet = Image.new('RGBA', (sheet_w, max(p.height for _, p in pictures)), (0, 0, 0, 0))
    places, x = [], 0
    for _, p in pictures:
        sheet.paste(p, (x, 0))
        places.append(x)
        x += p.width
    q = sheet.quantize(colors=62, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    palette = q.getpalette('RGBA')[:62 * 4]
    colors = [tuple(palette[i * 4:i * 4 + 4]) for i in range(len(palette) // 4)]
    visible = [i for i, c in enumerate(colors) if c[3] >= 12]
    src, qq = np.asarray(sheet), np.asarray(q)
    out = {}
    for (key, p), x0 in zip(pictures, places):
        data = np.zeros((p.height, p.width), np.uint8)
        for y in range(p.height):
            for x in range(p.width):
                px = src[y, x0 + x]
                if px[3] < 12:
                    continue
                k = int(qq[y, x0 + x])
                if colors[k][3] < 12:  # a visible pixel put on a clear colour: the nearest visible one
                    k = min(visible, key=lambda i: sum((int(a) - int(b)) ** 2 for a, b in zip(colors[i], px)))
                data[y, x] = k + 1
        out[key] = data
    return colors, out


def color_lines(colors, key, stream):
    stored = np.array([(c[2], c[1], c[0], c[3]) for c in colors], np.uint8).ravel()
    masked = stored ^ mask(key, stream, len(stored))
    return ['    {' + ', '.join(str(int(v)) for v in masked[i:i + 4]) + '},' for i in range(0, len(masked), 4)]


def array(name, values, key, stream):
    flat = np.asarray(values, np.uint8).ravel()
    flat = flat ^ mask(key, stream, len(flat))
    lines = [f'constexpr uint8_t {name}[] = {{']
    for i in range(0, len(flat), 32):
        lines.append('    ' + ', '.join(str(int(v)) for v in flat[i:i + 32]) + ',')
    lines.append('};')
    return lines


def main():
    atlas, palette, image = load_game()
    free = free_entries()
    akey, pkey = key_of(atlas), key_of(palette)
    lines = ['// Made by tools/make_prompts.py from the game\'s own button atlas (tools/prompt_art.py),',
             '// masked with keys from the game\'s atlas and palette (prompts.cpp). Do not edit.', '']
    lines.append('constexpr PromptCell kPromptCells[] = {')
    for cell in ORDER:
        x, y, w, h = CELLS[cell]
        lines.append(f'    {{{x}, {y}, {w}, {h}}},  // {cell}')
    lines.append('};')
    lines.append(f'constexpr int kPictureMargin = {M};  // around each cell in its picture (the shadow)')
    lines.append(f'constexpr uint64_t kSetStream[] = {{0, {SET_STREAMS["kPlayStation"]}, {SET_STREAMS["kNintendo"]}}};'
                 '  // by Set: none, PlayStation, Nintendo')
    lines.append(f'constexpr uint64_t kKeyStream = {KEY_STREAM};')
    lines.append('')
    sets = prompt_art.build_sets(image)
    for name in SET_STREAMS:
        cells = sets[name]
        shipped = [c for c in ORDER if c not in GAME_CELLS]
        colors, data = reduce([(cell, cells[cell]) for cell in shipped])
        assert len(colors) <= len(free)
        stream = SET_STREAMS[name]
        lines.append(f'// {name}: its colours (index 1 is the first), then each cell\'s indices, masked.')
        lines.append(f'constexpr uint8_t {name}Colors[][4] = {{')
        lines += color_lines(colors, pkey, stream)
        lines.append('};')
        for ci, cell in enumerate(ORDER):
            if cell in data:
                lines += array(f'{name}{cell}', data[cell], akey, stream * 16 + ci)
        for cell in shipped:
            _, _, w, h = CELLS[cell]
            lines.append(f'static_assert(sizeof({name}{cell}) == {w + 2 * M} * {h + 2 * M});')
        lines.append(f'static_assert(sizeof({name}Colors) / 4 <= 62);')
        lines.append(f'constexpr const uint8_t* {name}Cells[] = {{' +
                     ', '.join('nullptr' if c in GAME_CELLS else f'{name}{c}' for c in ORDER) + '};  // nullptr: the game\'s')
        lines.append('')
    keys = prompt_art.build_keys(image)
    names = KEY_NAMES + ['?']  # '?': a key without a picture
    colors, data = reduce([((n, s), keys[(n, s)]) for n in names for s in KEY_SHAPES])
    assert len(colors) <= len(free)
    lines.append('// The keyboard set: its colours, then each key in each shape (square, tall, wide), masked.')
    lines.append('constexpr uint8_t kKeyboardColors[][4] = {')
    lines += color_lines(colors, pkey, KEY_STREAM)
    lines.append('};')
    lines.append('static_assert(sizeof(kKeyboardColors) / 4 <= 62);')
    for ki, n in enumerate(names):
        ident = 'Unknown' if n == '?' else n
        for shi, s in enumerate(KEY_SHAPES):
            lines += array(f'kKey{ident}{s[1:]}', data[(n, s)], akey, 0x1000 + ki * 4 + shi)
    lines.append('struct KeyPictures {')
    lines.append('  const char* name;  // the SDK\'s ("" the key without a picture)')
    lines.append('  const uint8_t* square;')
    lines.append('  const uint8_t* tall;')
    lines.append('  const uint8_t* wide;')
    lines.append('};')
    lines.append('constexpr KeyPictures kKeys[] = {')
    for n in names:
        ident = 'Unknown' if n == '?' else n
        lines.append(f'    {{"{"" if n == "?" else n}", kKey{ident}Square, kKey{ident}Tall, kKey{ident}Wide}},')
    lines.append('};')
    for s, (w, h) in KEY_SHAPES.items():
        lines.append(f'constexpr uint32_t {s}W = {w}, {s}H = {h};')
    lines.append('')
    path = os.path.join(ROOT, 'src', 'gpu', 'prompt_sets.inc')
    open(path, 'w').write('\n'.join(lines))
    print('wrote', path)


if __name__ == '__main__':
    main()
