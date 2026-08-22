#!/usr/bin/env python3
"""Regenerate src/webui.h from web/webui.html.

The web UI ships as a gzip-compressed byte array in PROGMEM and is served with
`Content-Encoding: gzip` (see handleRoot in src/Web.cpp). web/webui.html is the
source of truth — edit it, then rerun:

    python tools/gen_webui.py

Runs automatically before every build (see tools/pre_build.py), so a stale
webui.h cannot ship.

Python 3 stdlib only. Output is deterministic (gzip header mtime=0), so
re-running without changes leaves webui.h byte-identical and the diff empty.

Derived from giovi321/smalltv-mod (WTFPL).
"""
import gzip
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "web" / "webui.html"
OUT = ROOT / "src" / "webui.h"

HEADER = """\
// GENERATED FILE -- do not edit by hand.
// Produced by tools/gen_webui.py from web/webui.html (the source of truth).
// Regenerate after editing the HTML:  python tools/gen_webui.py
//
// Gzip-compressed single-page config UI, served from PROGMEM with
// `Content-Encoding: gzip` by handleRoot in Web.cpp.
#pragma once
#include <Arduino.h>

static const uint8_t WEBUI_HTML_GZ[] PROGMEM = {
"""


def emit(blob: bytes) -> str:
    lines = [HEADER]
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    lines.append("};\n")
    lines.append("static const size_t WEBUI_HTML_GZ_LEN = sizeof(WEBUI_HTML_GZ);\n")
    return "".join(lines)


def main() -> int:
    if not SRC.exists():
        print(f"error: {SRC} not found", file=sys.stderr)
        return 1
    raw = SRC.read_bytes()
    blob = gzip.compress(raw, compresslevel=9, mtime=0)
    text = emit(blob)

    # Only write when the content actually changed, so the file mtime does not
    # churn and trigger needless rebuilds.
    if OUT.exists() and OUT.read_text(encoding="utf-8") == text:
        print(f"webui.h up to date ({len(blob)} B gzip)")
        return 0

    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{SRC.name}: {len(raw)} B -> gzip {len(blob)} B "
          f"({100 * len(blob) / len(raw):.1f}%) -> {OUT.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
