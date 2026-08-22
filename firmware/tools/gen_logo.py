#!/usr/bin/env python3
"""Regenerate src/splash/logo.h from assets/texterous_logo.svg.

Unlike gen_webui.py and gen_agent.py this is NOT part of the build. Rasterising
an SVG needs a real renderer, and the firmware build should not carry that
dependency for an asset that changes about once a year. src/splash/logo.h is
committed; run this by hand when the brand mark changes.

    pip install cairosvg pillow
    python tools/gen_logo.py [--width 44] [--height 74] [--threshold 110]

Output is a 1-bit bitmap, MSB first, 1 = ink, padded to whole bytes per row --
the format drawMark() in Gfx.cpp reads with pgm_read_byte.
"""
import argparse
import io
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "assets" / "texterous_logo.svg"
OUT = ROOT / "src" / "splash" / "logo.h"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=44)
    ap.add_argument("--height", type=int, default=74)
    # Alpha cutoff. The mark has thin strokes at this size; too high a threshold
    # eats them, too low fattens the silhouette into a blob.
    ap.add_argument("--threshold", type=int, default=110)
    ap.add_argument("--preview", action="store_true", help="print ASCII art and exit")
    a = ap.parse_args()

    try:
        import cairosvg
        from PIL import Image
    except ImportError:
        print("needs cairosvg and pillow:  pip install cairosvg pillow", file=sys.stderr)
        return 1

    png = cairosvg.svg2png(url=str(SRC), output_width=a.width, output_height=a.height)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    alpha = img.split()[3]

    if a.preview:
        for y in range(0, a.height, 2):
            print("".join("#" if alpha.getpixel((x, y)) > a.threshold else "."
                          for x in range(a.width)))
        return 0

    row_bytes = (a.width + 7) // 8
    blob = bytearray()
    for y in range(a.height):
        for b in range(row_bytes):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                if x < a.width and alpha.getpixel((x, y)) > a.threshold:
                    byte |= 0x80 >> bit
            blob.append(byte)

    rows = ["  " + ", ".join(f"0x{blob[y * row_bytes + i]:02x}" for i in range(row_bytes)) + ","
            for y in range(a.height)]
    OUT.write_text(f"""// GENERATED FILE -- do not edit by hand.
// Produced by tools/gen_logo.py from assets/texterous_logo.svg.
//
// The Texterous brand mark as a 1-bit bitmap for the boot splash: {a.width}x{a.height} px,
// {row_bytes} bytes per row, MSB first, 1 = ink. {len(blob)} bytes in PROGMEM.
//
// Unlike webui.h and agent_install.h this is NOT regenerated on every build --
// rasterising an SVG needs a dependency the firmware build should not carry. It
// is committed, and refreshed by hand when the mark changes.
#pragma once
#include <Arduino.h>

#define LOGO_W {a.width}
#define LOGO_H {a.height}
#define LOGO_ROW_BYTES {row_bytes}

static const uint8_t TEXTEROUS_LOGO[] PROGMEM = {{
{chr(10).join(rows)}
}};
""", encoding="utf-8", newline="\n")
    print(f"{OUT.name}: {a.width}x{a.height}, {len(blob)} B")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
