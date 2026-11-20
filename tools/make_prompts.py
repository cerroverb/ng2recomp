#!/usr/bin/env python3
"""Makes src/gpu/prompt_sets.inc: the PlayStation, Nintendo, Steam Deck and keyboard button
prompts for the game's button atlas (src/gpu/prompts.cpp).

usage: make_prompts.py [<repo>]

The pictures are Xelu's controller prompts (CC0, assets/prompts); the tall
L2 / R2 plates are drawn here in the same style. Each set is reduced to at
most 62 colours over all its cells (the atlas palette has 62 entries free
once those cells are cleared; index 0 is transparent), and each cell to
those indices; prompts.cpp maps them onto the free entries.
Needs Pillow.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..')
ASSETS = os.path.join(ROOT, 'assets', 'prompts')
FONT = '/usr/share/fonts/TTF/DejaVuSans-Bold.ttf'

# The atlas's button cells (x, y, width, height), from its visible pixels.
CELLS = {
    'X': (13, 13, 39, 40), 'Y': (77, 13, 39, 40), 'B': (141, 13, 39, 40), 'A': (206, 13, 39, 40),
    'LT': (278, 10, 21, 44), 'RT': (342, 10, 21, 44), 'LB': (394, 20, 44, 24), 'RB': (460, 20, 43, 24),
    'LS': (76, 76, 40, 40), 'RS': (140, 76, 40, 40),
    'BACK': (396, 204, 41, 39), 'START': (460, 204, 40, 39),
}

# The atlas's own cells by position: A is the bottom face button, B the right one, ...
SETS = {
    'kPlayStation': {
        'A': 'ps/PS3_Cross.png', 'B': 'ps/PS3_Circle.png', 'X': 'ps/PS3_Square.png',
        'Y': 'ps/PS3_Triangle.png', 'LB': 'ps/PS3_L1.png', 'RB': 'ps/PS3_R1.png',
        'LT': ('plate', 'L', '2'), 'RT': ('plate', 'R', '2'),
        'LS': 'ps/PS3_Left_Stick_Click.png', 'RS': 'ps/PS3_Right_Stick_Click.png',
        'BACK': 'ps/PS3_Select.png', 'START': 'ps/PS3_Start.png',
    },
    'kNintendo': {
        'A': 'switch/Switch_B.png', 'B': 'switch/Switch_A.png', 'X': 'switch/Switch_Y.png',
        'Y': 'switch/Switch_X.png', 'LB': 'switch/Switch_LB.png', 'RB': 'switch/Switch_RB.png',
        'LT': 'switch/Switch_LT.png', 'RT': 'switch/Switch_RT.png',
        'LS': 'switch/Switch_Left_Stick_Click.png', 'RS': 'switch/Switch_Right_Stick_Click.png',
        'BACK': 'switch/Switch_Minus.png', 'START': 'switch/Switch_Plus.png',
    },
    'kSteamDeck': {
        'A': 'deck/SteamDeck_A.png', 'B': 'deck/SteamDeck_B.png', 'X': 'deck/SteamDeck_X.png',
        'Y': 'deck/SteamDeck_Y.png', 'LB': 'deck/SteamDeck_L1.png', 'RB': 'deck/SteamDeck_R1.png',
        'LT': 'deck/SteamDeck_L2.png', 'RT': 'deck/SteamDeck_R2.png',
        'LS': 'deck/SteamDeck_Left_Stick_Click.png', 'RS': 'deck/SteamDeck_Right_Stick_Click.png',
        'BACK': 'deck/SteamDeck_Inventory.png', 'START': 'deck/SteamDeck_Menu.png',
    },
}


def plate(w, h, top, bottom):
    """A tall trigger plate in Xelu's style: dark, outlined, two letters stacked."""
    s = 8
    im = Image.new('RGBA', (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((s, s, w * s - s, h * s - s), radius=5 * s, fill=(30, 30, 30, 255))
    d.rounded_rectangle((3 * s, 3 * s, w * s - 3 * s, h * s - 3 * s), radius=4 * s,
                        fill=(72, 72, 72, 255))
    font = ImageFont.truetype(FONT, 12 * s)
    for text, cy in ((top, h * s * 0.3), (bottom, h * s * 0.7)):
        d.text((w * s / 2, cy), text, font=font, fill=(222, 222, 222, 255), anchor='mm')
    return im.resize((w, h), Image.LANCZOS)


def fit(path, w, h, upright=False):
    """The picture scaled to fit (w, h), centred, its shape kept. upright: a
    wide picture turned on its side for a tall cell (read bottom to top)."""
    src = Image.open(os.path.join(ASSETS, path)).convert('RGBA')
    bbox = src.getbbox()
    src = src.crop(bbox)
    if upright and src.width > src.height * 1.3:
        src = src.rotate(90, expand=True)
    k = min(w / src.width, h / src.height)
    sw, sh = max(1, round(src.width * k)), max(1, round(src.height * k))
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    im.paste(src.resize((sw, sh), Image.LANCZOS), ((w - sw) // 2, (h - sh) // 2))
    return im


def make_set(name, cells):
    pictures = {}
    for cell, what in cells.items():
        _, _, w, h = CELLS[cell]
        pictures[cell] = plate(w, h, what[1], what[2]) if isinstance(what, tuple) else fit(what, w, h)
    # One sheet of all cells, reduced to 64 colours together.
    sheet_w = sum(p.width for p in pictures.values())
    sheet_h = max(p.height for p in pictures.values())
    sheet = Image.new('RGBA', (sheet_w, sheet_h), (0, 0, 0, 0))
    x = 0
    places = {}
    for cell, p in pictures.items():
        sheet.paste(p, (x, 0))
        places[cell] = x
        x += p.width
    q = sheet.quantize(colors=62, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    palette = q.getpalette("RGBA")[:62 * 4]
    colors = [tuple(palette[i * 4:i * 4 + 4]) for i in range(len(palette) // 4)]
    # A visible pixel the reduction put on a clear colour: the nearest visible one.
    visible = [i for i, c in enumerate(colors) if c[3] >= 12]
    def nearest(rgba):
        return min(visible, key=lambda i: sum((a - b) ** 2 for a, b in zip(colors[i], rgba)))
    out_cells = {}
    src = sheet.load()
    qq = q.load()
    for cell, p in pictures.items():
        x0 = places[cell]
        data = []
        for y in range(p.height):
            for x in range(p.width):
                pixel = src[x0 + x, y]
                if pixel[3] < 12:
                    data.append(0)
                else:
                    k = qq[x0 + x, y]
                    data.append((k if colors[k][3] >= 12 else nearest(pixel)) + 1)
        out_cells[cell] = data
    return colors, out_cells


# The keyboard set: every key the SDK names (rex/ui keybinds), in each of the
# cell shapes, reduced together; prompts.cpp picks the bound key's picture.
KEY_SHAPES = {'kSquare': (39, 40), 'kTall': (21, 44), 'kWide': (44, 24)}
KEY_FILES = {
    **{c: c for c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'},
    **{f'F{i}': f'F{i}' for i in range(1, 13)},
    **{f'Numpad{i}': str(i) for i in range(10)},
    'Escape': 'Esc', 'Return': 'Enter', 'NumpadEnter': 'Enter', 'Space': 'Space', 'Tab': 'Tab',
    'Backspace': 'Backspace', 'Delete': 'Del', 'Insert': 'Insert', 'Home': 'Home', 'End': 'End',
    'PageUp': 'Page_Up', 'PageDown': 'Page_Down', 'Left': 'Arrow_Left', 'Right': 'Arrow_Right',
    'Up': 'Arrow_Up', 'Down': 'Arrow_Down', 'Shift': 'Shift', 'Control': 'Ctrl', 'Alt': 'Alt',
    'CapsLock': 'Caps_Lock', 'NumLock': 'Num_Lock', 'PrintScreen': 'Print_Screen',
    'Backtick': 'Tilda', 'Minus': 'Minus', 'Plus': 'Plus', 'NumpadPlus': 'Plus',
    'NumpadMinus': 'Minus', 'NumpadStar': 'Asterisk', 'NumpadSlash': 'Slash', 'Slash': 'Slash',
    'Semicolon': 'Semicolon', 'Quote': 'Quote', 'LBracket': 'Bracket_Left',
    'RBracket': 'Bracket_Right', 'Comma': 'Mark_Left', 'Period': 'Mark_Right', 'LMB': 'Mouse_Left', 'RMB': 'Mouse_Right', 'MMB': 'Mouse_Middle',
}


def key_path(name):
    f = KEY_FILES.get(name)
    return f'keys/{f}_Key_Dark.png' if f else 'keys/Blank_Black_Normal.png'


def make_keyboard(lines):
    names = sorted(KEY_FILES) + ['?']  # '?': a key without a picture
    pictures = []
    for name in names:
        path = key_path(name) if name != '?' else 'keys/Blank_Black_Normal.png'
        for shape, (w, h) in KEY_SHAPES.items():
            pictures.append((name, shape, fit(path, w, h, upright=shape == 'kTall')))
    sheet_w = sum(p.width for _, _, p in pictures)
    sheet = Image.new('RGBA', (sheet_w, 44), (0, 0, 0, 0))
    x = 0
    places = []
    for _, _, p in pictures:
        sheet.paste(p, (x, 0))
        places.append(x)
        x += p.width
    q = sheet.quantize(colors=62, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
    palette = q.getpalette('RGBA')[:62 * 4]
    colors = [tuple(palette[i * 4:i * 4 + 4]) for i in range(len(palette) // 4)]
    visible = [i for i, c in enumerate(colors) if c[3] >= 12]
    src, qq = sheet.load(), q.load()
    lines.append('// The keyboard set: its colours, then each key in each shape (square, tall, wide).')
    lines.append('constexpr uint8_t kKeyboardColors[][4] = {')
    for c in colors:
        lines.append(f'    {{{c[0]}, {c[1]}, {c[2]}, {c[3]}}},')
    lines.append('};')
    lines.append('static_assert(sizeof(kKeyboardColors) / 4 <= 62);')
    for (name, shape, p), x0 in zip(pictures, places):
        data = []
        for y in range(p.height):
            for x in range(p.width):
                px = src[x0 + x, y]
                if px[3] < 12:
                    data.append(0)
                else:
                    k = qq[x0 + x, y]
                    if colors[k][3] < 12:
                        k = min(visible, key=lambda i: sum((a - b) ** 2 for a, b in zip(colors[i], px)))
                    data.append(k + 1)
        ident = 'Unknown' if name == '?' else name
        lines.append(f'constexpr uint8_t kKey{ident}{shape[1:]}[] = {{')
        for i in range(0, len(data), 32):
            lines.append('    ' + ', '.join(str(v) for v in data[i:i + 32]) + ',')
        lines.append('};')
    lines.append('struct KeyPictures {')
    lines.append('  const char* name;  // the SDK\'s ("" the key without a picture)')
    lines.append('  const uint8_t* square;')
    lines.append('  const uint8_t* tall;')
    lines.append('  const uint8_t* wide;')
    lines.append('};')
    lines.append('constexpr KeyPictures kKeys[] = {')
    for name in names:
        ident = 'Unknown' if name == '?' else name
        shown = '' if name == '?' else name
        lines.append(f'    {{"{shown}", kKey{ident}Square, kKey{ident}Tall, kKey{ident}Wide}},')
    lines.append('};')
    for shape, (w, h) in KEY_SHAPES.items():
        lines.append(f'constexpr uint32_t {shape}W = {w}, {shape}H = {h};')
    lines.append('')


def main():
    lines = ['// Made by tools/make_prompts.py from Xelu\'s controller prompts (CC0,',
             '// assets/prompts/License-Xelu.txt): do not edit.', '']
    order = list(CELLS)
    lines.append('constexpr PromptCell kPromptCells[] = {')
    for cell in order:
        x, y, w, h = CELLS[cell]
        lines.append(f'    {{{x}, {y}, {w}, {h}}},  // {cell}')
    lines.append('};')
    lines.append('')
    for name, cells in SETS.items():
        colors, data = make_set(name, cells)
        lines.append(f'// {name}: its colours (RGBA; index 1 is the first), then each cell\'s indices.')
        lines.append(f'constexpr uint8_t {name}Colors[][4] = {{')
        for c in colors:
            lines.append(f'    {{{c[0]}, {c[1]}, {c[2]}, {c[3]}}},')
        lines.append('};')
        for cell in order:
            values = data[cell]
            lines.append(f'constexpr uint8_t {name}{cell}[] = {{')
            for i in range(0, len(values), 32):
                lines.append('    ' + ', '.join(str(v) for v in values[i:i + 32]) + ',')
            lines.append('};')
        for cell in order:
            x, y, w, h = CELLS[cell]
            lines.append(f'static_assert(sizeof({name}{cell}) == {w} * {h});')
        lines.append(f'static_assert(sizeof({name}Colors) / 4 <= 62);')
        lines.append(f'constexpr const uint8_t* {name}Cells[] = {{' +
                     ', '.join(f'{name}{cell}' for cell in order) + '};')
        lines.append('')
    make_keyboard(lines)
    path = os.path.join(ROOT, 'src', 'gpu', 'prompt_sets.inc')
    open(path, 'w').write('\n'.join(lines))
    print('wrote', path)


if __name__ == '__main__':
    main()
