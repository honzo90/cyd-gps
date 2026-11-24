#!/usr/bin/env python3
"""Polish letters for the CYD screens, built from LVGL's own Montserrat fonts (no download, no tools).

LVGL's built-in Montserrat fonts have ASCII only. This script reads them (the C files in
.pio/libdeps/<env>/lvgl/src/font), composes the 18 Polish letters from their base letters plus a
drawn accent (acute, ogonek, stroke) or the font's own '.' (dot above), and writes src/font_pl.c:
one small LVGL font per size holding only these letters, with the Montserrat font of the same
size as its fallback for everything else. Use font_pl_14 etc. wherever Montserrat was used.

Montserrat is under the SIL Open Font License 1.1; the generated glyphs are a derivative under
the same licence.

Run: python3 tools/fonts/make_pl_fonts.py   (after a `pio run`, which fetches LVGL)
"""
import math
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SIZES = [14, 20, 32, 48]
SS = 4  # supersampling for the drawn accents

# letter -> (base letter, accent)
LETTERS = {
    'ą': ('a', 'ogonek'), 'ć': ('c', 'acute'), 'ę': ('e', 'ogonek'), 'ł': ('l', 'stroke'),
    'ń': ('n', 'acute'), 'ó': ('o', 'acute'), 'ś': ('s', 'acute'), 'ź': ('z', 'acute'), 'ż': ('z', 'dot'),
    'Ą': ('A', 'ogonek'), 'Ć': ('C', 'acute'), 'Ę': ('E', 'ogonek'), 'Ł': ('L', 'stroke'),
    'Ń': ('N', 'acute'), 'Ó': ('O', 'acute'), 'Ś': ('S', 'acute'), 'Ź': ('Z', 'acute'), 'Ż': ('Z', 'dot'),
}


def find_font_dir():
    for d in sorted((ROOT / '.pio' / 'libdeps').glob('*/lvgl/src/font')):
        return d
    sys.exit('LVGL sources not found: run `pio run` first')


class Font:
    """The ASCII part of an LVGL fmt_txt font C file (bpp 4, uncompressed)."""

    def __init__(self, path):
        src = path.read_text()
        bm = src[src.index('glyph_bitmap[] = {'):]
        bm = bm[:bm.index('};')]
        bm = re.sub(r'/\*.*?\*/', '', bm, flags=re.S)
        self.bitmap = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', bm)]
        self.dsc = [tuple(int(v) for v in m) for m in re.findall(
            r'\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), '
            r'\.ofs_x = (-?\d+), \.ofs_y = (-?\d+)', src)]
        self.line_height = int(re.search(r'\.line_height = (\d+)', src).group(1))
        self.base_line = int(re.search(r'\.base_line = (\d+)', src).group(1))
        assert re.search(r'\.bpp = 4', src) and re.search(r'\.bitmap_format = 0', src), 'needs bpp 4, plain'

    def glyph(self, ch):
        """{x, y (baseline coordinates, y up)}: alpha 0..15, plus advance (1/16 px)."""
        bi, adv, w, h, ox, oy = self.dsc[ord(ch) - 32 + 1]  # cmap 0: U+0020.. -> glyph id 1..
        px = {}
        for i in range(w * h):
            nib = bi * 2 + i
            b = self.bitmap[nib // 2]
            a = (b >> 4) if nib % 2 == 0 else (b & 15)
            if a:
                r, c = divmod(i, w)
                px[(ox + c, oy + h - 1 - r)] = a
        return px, adv

    def top(self, ch):
        _, _, _, h, _, oy = self.dsc[ord(ch) - 32 + 1]
        return oy + h  # first row above the glyph

    def box(self, ch):
        _, _, w, h, ox, oy = self.dsc[ord(ch) - 32 + 1]
        return ox, oy, w, h


def draw_polyline(px, pts, width):
    """Antialiased thick polyline (float baseline coords, y up) blended into px (max)."""
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    x0, x1 = math.floor(min(xs) - width), math.ceil(max(xs) + width)
    y0, y1 = math.floor(min(ys) - width), math.ceil(max(ys) + width)
    r = width / 2
    for gy in range(y0, y1 + 1):
        for gx in range(x0, x1 + 1):
            hit = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = gx + (sx + 0.5) / SS
                    y = gy + (sy + 0.5) / SS
                    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
                        dx, dy = bx - ax, by - ay
                        t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
                        if (x - ax - t * dx) ** 2 + (y - ay - t * dy) ** 2 <= r * r:
                            hit += 1
                            break
            if hit:
                a = round(15 * hit / (SS * SS))
                if a:
                    # pixel (gx, gy) covers [gx, gx+1) x [gy, gy+1): row index = gy
                    px[(gx, gy)] = max(px.get((gx, gy), 0), a)


def compose(font, size, ch):
    base, accent = LETTERS[ch]
    px, adv = font.glyph(base)
    upper = base.isupper()
    ox, oy, w, h = font.box(base)
    top = font.top(base)
    stem = max(1.2, size * 0.085)  # Montserrat Medium stem width
    cx = ox + w / 2
    if accent == 'acute':
        gap = 1 if upper else max(1, round(size * 0.07))
        hgt = max(2.2, size * (0.16 if upper else 0.22))  # the line height grows to fit (see emit)
        yb = top + gap
        draw_polyline(px, [(cx - hgt * 0.35, yb + stem * 0.3), (cx + hgt * 0.55, yb + hgt)], stem * 0.95)
    elif accent == 'dot':
        dot, _ = font.glyph('.')
        dx0 = min(x for x, _ in dot)
        dx1 = max(x for x, _ in dot)
        dy0 = min(y for _, y in dot)
        gap = 1 if upper else max(1, round(size * 0.07))
        shift_x = round(cx - (dx0 + dx1 + 1) / 2)
        shift_y = top + gap - dy0
        for (x, y), a in dot.items():
            k = (x + shift_x, y + shift_y)
            px[k] = max(px.get(k, 0), a)
    elif accent == 'ogonek':
        # hook under the right part of the letter, below the baseline
        right = ox + w - stem * (0.6 if base in 'ae' else 0.9)
        d = size * 0.20
        pts = [(right, 0.4), (right - d * 0.55, -d * 0.45), (right - d * 0.35, -d * 0.95), (right + d * 0.15, -d * 1.05)]
        draw_polyline(px, pts, stem * 0.85)
    elif accent == 'stroke':
        stem_x = ox + stem / 2 + (0.1 if upper else 0)
        if not upper:
            stem_x = ox + w / 2
        ymid = (top * 0.45) if upper else top * 0.5
        d = size * 0.13
        draw_polyline(px, [(stem_x - d, ymid - d * 0.55), (stem_x + d, ymid + d * 0.55)], stem * 0.85)
    # bounding box of all pixels
    xs = [x for x, _ in px]
    ys = [y for _, y in px]
    bx0, bx1, by0, by1 = min(xs), max(xs), min(ys), max(ys)
    bw, bh = bx1 - bx0 + 1, by1 - by0 + 1
    rows = []
    for y in range(by1, by0 - 1, -1):  # top row first
        rows.append([px.get((x, y), 0) for x in range(bx0, bx1 + 1)])
    return dict(adv=adv, w=bw, h=bh, ox=bx0, oy=by0, rows=rows)


def pack(rows):
    nibs = [a for r in rows for a in r]
    if len(nibs) % 2:
        nibs.append(0)
    return [(nibs[i] << 4) | nibs[i + 1] for i in range(0, len(nibs), 2)]


def emit(fonts):
    out = ['/* Polish letters for the CYD UI, generated by tools/fonts/make_pl_fonts.py - do not edit.',
           ' * Composed from LVGL\'s Montserrat (SIL Open Font License 1.1); each font falls back to the',
           ' * Montserrat of the same size for every other character. */',
           '#include "font_pl.h"', '']
    letters = sorted(LETTERS, key=ord)
    for size, font in fonts:
        glyphs = [compose(font, size, ch) for ch in letters]
        data, dsc = [], []
        for g in glyphs:
            dsc.append((len(data), g))
            data += pack(g['rows'])
        # capitals with an accent reach above Montserrat's ascent: grow the line so nothing is clipped
        ascent = max(font.line_height - font.base_line, max(g['oy'] + g['h'] for g in glyphs))
        line_height = ascent + font.base_line
        out.append(f'/* ---- {size} px (line height {font.line_height} -> {line_height}) ---- */')
        out.append(f'static const uint8_t bitmap_{size}[] = {{')
        for i in range(0, len(data), 16):
            out.append('    ' + ', '.join(f'0x{b:02x}' for b in data[i:i + 16]) + ',')
        out.append('};')
        out.append(f'static const lv_font_fmt_txt_glyph_dsc_t dsc_{size}[] = {{')
        out.append('    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},')
        for (bi, g), ch in zip(dsc, letters):
            out.append(f'    {{.bitmap_index = {bi}, .adv_w = {g["adv"]}, .box_w = {g["w"]}, .box_h = {g["h"]}, '
                       f'.ofs_x = {g["ox"]}, .ofs_y = {g["oy"]}}}, /* U+{ord(ch):04X} {ch} */')
        out.append('};')
        if size == SIZES[0]:
            start = ord(letters[0])
            out.append('static const uint16_t unicode_list[] = {' +
                       ', '.join(f'0x{ord(c) - start:x}' for c in letters) + '};')
            out.append(f'static const lv_font_fmt_txt_cmap_t cmaps[] = {{{{.range_start = 0x{start:x}, '
                       f'.range_length = 0x{ord(letters[-1]) - start + 1:x}, .glyph_id_start = 1, '
                       f'.unicode_list = unicode_list, .glyph_id_ofs_list = NULL, .list_length = {len(letters)}, '
                       '.type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}};')
        out.append(f'static const lv_font_fmt_txt_dsc_t fdsc_{size} = {{.glyph_bitmap = bitmap_{size}, '
                   f'.glyph_dsc = dsc_{size}, .cmaps = cmaps, .kern_dsc = NULL, .kern_scale = 0, .cmap_num = 1, '
                   '.bpp = 4, .kern_classes = 0, .bitmap_format = 0};')
        out.append(f'const lv_font_t font_pl_{size} = {{.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt, '
                   f'.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt, .line_height = {line_height}, '
                   f'.base_line = {font.base_line}, .subpx = LV_FONT_SUBPX_NONE, .underline_position = -1, '
                   f'.underline_thickness = 1, .dsc = &fdsc_{size}, .fallback = &lv_font_montserrat_{size}}};')
        out.append('')
    return '\n'.join(out)


def main():
    d = find_font_dir()
    fonts = [(s, Font(d / f'lv_font_montserrat_{s}.c')) for s in SIZES]
    (ROOT / 'src' / 'font_pl.c').write_text(emit(fonts))
    (ROOT / 'src' / 'font_pl.h').write_text(
        '// Montserrat + Polish letters (src/font_pl.c, generated by tools/fonts/make_pl_fonts.py).\n'
        '#pragma once\n#include <lvgl.h>\n\n#ifdef __cplusplus\nextern "C" {\n#endif\n' +
        ''.join(f'extern const lv_font_t font_pl_{s};\n' for s in SIZES) +
        '#ifdef __cplusplus\n}\n#endif\n')
    print('wrote src/font_pl.c, src/font_pl.h')


if __name__ == '__main__':
    main()
