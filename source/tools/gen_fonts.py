#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""BREAKR's screen fonts, from Inter (SIL OFL 1.1, assets/fonts/): capitals, digits and signs, anti-aliased on
16 levels (4 bits a pixel, the left pixel in the high nibble: the platform's felucca_font_t, gfx.c).

  FONT_BS  Inter SemiBold 12 px      labels, hints          (box 12 px)
  FONT_BM  Inter Bold 16 px          values, titles         (box 16 px)
  FONT_BL  Inter Display ExtraBold   big numbers (tempo)    (box 30 px, digits and signs only)

  tools/gen_fonts.py   -> firmware/gen/brk_fonts.h   (needs Pillow)
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

SRC = Path(__file__).resolve().parents[1]
FONTS = SRC / "assets" / "fonts"
OUT = SRC / "firmware" / "gen" / "brk_fonts.h"
PAD = 2

SPECS = [
    ("FONT_BS", "Inter-SemiBold.otf", 12, 0, " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"),
    ("FONT_BM", "Inter-Bold.otf", 16, 1, " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"),
    ("FONT_BL", "InterDisplay-ExtraBold.otf", 36, 1, " %+,-./0123456789:"),
]


def build(name, file, size, track, chars):
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
                v = min(15, (px[x, y] + 8) // 17)
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
    """the name, big, for the start screen: one 4-bit picture"""
    f = ImageFont.truetype(str(FONTS / "InterDisplay-ExtraBold.otf"), 46)
    l, t, r, b = f.getbbox("BREAKR", anchor="ls")
    w, h = r - l + 2, b - t + 2
    im = Image.new("L", (w, h), 0)
    ImageDraw.Draw(im).text((1 - l, 1 - t), "BREAKR", font=f, fill=255, anchor="ls")
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
           "/* BREAKR's screen fonts: made by tools/gen_fonts.py from Inter (Copyright 2016-2023 The Inter Project",
           " * Authors, SIL Open Font License 1.1: assets/fonts/Inter-LICENSE.txt). Do not edit. */"]
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
