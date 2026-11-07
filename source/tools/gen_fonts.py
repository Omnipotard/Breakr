#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""BREAKR's screen fonts (SIL OFL 1.1, assets/fonts/): capitals, digits and signs, anti-aliased on 16 levels (4 bits a
pixel, the left pixel in the high nibble: the platform's felucca_font_t, gfx.c). A narrow grotesque for what is read
(Barlow Condensed: more fits on 240 px, as on a rack's panel) and a pixel face for the tiny marks under the controls.

  FONT_BS  Barlow Condensed SemiBold 14 px   labels                  (box 13 px)
  FONT_BM  Barlow Condensed Bold 19 px       values, titles          (box 16 px)
  FONT_BL  Barlow Condensed ExtraBold 42 px  big numbers (tempo)     (digits and signs only)
  FONT_PX  Silkscreen 8 px, 1 bit            what a control does     (box 7 px)
  LOGO     BREAKR, chopped in slices that slip, as a break cut up

  tools/gen_fonts.py   -> firmware/gen/brk_fonts.h   (needs Pillow)
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SRC = Path(__file__).resolve().parents[1]
FONTS = SRC / "assets" / "fonts"
OUT = SRC / "firmware" / "gen" / "brk_fonts.h"
PAD = 2

ALL = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
SPECS = [
    ("FONT_BS", "barlow-condensed-latin-600-normal.woff", 14, 1, ALL, False),
    ("FONT_BM", "barlow-condensed-latin-700-normal.woff", 19, 1, ALL, False),
    ("FONT_BL", "barlow-condensed-latin-800-normal.woff", 42, 1, " %+,-./0123456789:", False),
    ("FONT_PX", "silkscreen-latin-400-normal.woff", 8, 0, ALL, True),
]


def build(name, file, size, track, chars, pixel):
    f = ImageFont.truetype(str(FONTS / file), size)
    asc, desc = f.getmetrics()
    cap = f.getbbox("H", anchor="ls")[1]        # negative: top of a capital over the baseline
    top = asc + cap - 1                          # the box: a row over the capitals .. 3 rows under the baseline
    h = -cap + 1 + max(3, size // 5)
    first, last = ord(chars[0]), ord(chars[-1])
    adv, bw, off, data = [], [], [], bytearray()
    for code in range(first, last + 1):
        c = chr(code)
        if c not in chars:                       # not in this font: an empty glyph
            off.append(len(data))
            data += bytes(h)
            adv.append(4)
            bw.append(1)
            continue
        a = int(round(f.getlength(c))) + track
        w = a + 2 * PAD + 4
        im = Image.new("L", (w, asc + desc + 4), 0)
        ImageDraw.Draw(im).text((PAD, asc), c, font=f, fill=255, anchor="ls")
        im = im.crop((0, top, w, top + h))
        px = im.load()
        right = 0
        for x in range(w):
            if any(px[x, y] for y in range(h)):
                right = x + 1
        gw = max(1, right)
        bpr = (gw + 1) // 2
        off.append(len(data))
        for y in range(h):
            row = bytearray(bpr)
            for x in range(gw):
                v = (15 if px[x, y] >= 96 else 0) if pixel else min(15, (px[x, y] + 8) // 17)
                row[x // 2] |= v << 4 if x % 2 == 0 else v
            data += row
        adv.append(a)
        bw.append(gw)
    lines = [f"static const uint8_t {name}_ADV[] = {{{', '.join(map(str, adv))}}};",
             f"static const uint8_t {name}_BW[] = {{{', '.join(map(str, bw))}}};",
             f"static const uint16_t {name}_OFF[] = {{{', '.join(map(str, off))}}};",
             f"static const uint8_t {name}_DATA[{len(data)}] = {{{', '.join(map(str, data))}}};",
             f"static const felucca_font_t {name} = {{{h}, {PAD}, {first}, {last}, {name}_ADV, {name}_BW, {name}_OFF, {name}_DATA}};"]
    return lines, len(data) + 3 * len(adv) + len(adv), h


def logo():
    """the name, big, for the start screen: one 4-bit picture, a band cut out of it and slipped sideways (a break chopped)"""
    f = ImageFont.truetype(str(FONTS / "barlow-condensed-latin-800-normal.woff"), 72)
    l, t, r, b = f.getbbox("BREAKR", anchor="ls")
    w, h = r - l + 2 + 8, b - t + 2
    src = Image.new("L", (w, h), 0)
    ImageDraw.Draw(src).text((1 - l, 1 - t), "BREAKR", font=f, fill=255, anchor="ls")
    top, bot = src.getbbox()[1], src.getbbox()[3]
    c1, c2 = top + (bot - top) * 40 // 100, top + (bot - top) * 62 // 100   # the band cut out and slipped
    im = Image.new("L", (w, h), 0)
    im.paste(src.crop((0, 0, w, c1)), (0, 0))
    im.paste(src.crop((0, c1 + 2, w - 7, c2)), (7, c1 + 2))
    im.paste(src.crop((0, c2 + 2, w, h)), (0, c2 + 2))
    px = im.load()
    bpr = (w + 1) // 2
    data = bytearray()
    for y in range(h):
        row = bytearray(bpr)
        for x in range(w):
            v = min(15, (px[x, y] + 8) // 17)
            row[x // 2] |= v << 4 if x % 2 == 0 else v
        data += row
    return [f"#define LOGO_W {w}u", f"#define LOGO_H {h}u",
            f"static const uint8_t LOGO[{len(data)}] = {{{', '.join(map(str, data))}}};"], len(data)


def main():
    out = ["/* SPDX-License-Identifier: GPL-3.0-only AND OFL-1.1 */",
           "/* BREAKR's screen fonts: made by tools/gen_fonts.py from Barlow Condensed (Copyright 2017 The Barlow Project",
           " * Authors) and Silkscreen (Copyright 2001 The Silkscreen Project Authors), SIL Open Font License 1.1:",
           " * assets/fonts/BarlowCondensed-LICENSE.txt, Silkscreen-LICENSE.txt. Do not edit. */"]
    for spec in SPECS:
        lines, size, h = build(*spec)
        out += lines
        print(f"{spec[0]}: {spec[2]} px, box {h} px, {size} B")
    lg, n = logo()
    out += lg
    print(f"LOGO: {n} B")
    OUT.write_text("\n".join(out) + "\n")
    print(f"-> {OUT}")


if __name__ == "__main__":
    main()
