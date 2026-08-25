#!/usr/bin/env python3
"""Regenerate src/webui.h from web/webui.html.

The web UI ships as a gzip-compressed byte array in PROGMEM and is served with
`Content-Encoding: gzip` (see handleRoot in src/Web.cpp). web/webui.html is the
source of truth — edit it, then rerun:

    python tools/gen_webui.py

Runs automatically before every build (see tools/pre_build.py), so a stale
webui.h cannot ship.

Comments are stripped on the way in (see strip_comments). The HTML is the only
documentation this project has for the portal's copy, and every claim in it had to
be checked against the daemon's source — so it is heavily commented on purpose,
and the device should not pay flash for prose no browser reads. The slim image has
about 100 bytes of room under the CI gate; the comments are 2.4 KB of it.

Python 3 stdlib only. Output is deterministic (gzip header mtime=0, and the strip
is a pure function of the source), so re-running without changes leaves webui.h
byte-identical and the diff empty.

Derived from giovi321/smalltv-mod (WTFPL).
"""
import gzip
import pathlib
import re
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


def strip_comments(text: str) -> str:
    """Drop comment text, keeping every line break so line numbers still match.

    Deliberately conservative, because this page is the only instructions a
    recipient gets and a clever regex that eats one character of real markup would
    be discovered by them, not by us:

    * `//` comments are removed only when the line STARTS with them (leading
      whitespace allowed) and only inside the single <script> block. That cannot
      touch a `//` inside a string, a template literal or a URL, since none of
      those can be the first thing on a line here — asserted below rather than
      assumed. A trailing `// like this` is left alone; it is not worth the risk.
    * HTML comments go whole. There are no `<!--` sequences inside the script, so
      no JS string can be clipped.

    Each removed line becomes an empty line, so a browser console reporting
    "webui.html:812" still points at line 812 of web/webui.html. Runs of newlines
    cost almost nothing once gzipped (63 B for all of them).
    """
    try:
        s_lo = text.count("\n", 0, text.index("<script>")) + 1
        s_hi = text.count("\n", 0, text.index("</script>")) + 1
    except ValueError:                      # no script block: nothing to strip
        s_lo = s_hi = 0

    out = []
    for i, line in enumerate(text.split("\n"), 1):
        if s_lo <= i <= s_hi and re.match(r"^\s*//", line):
            out.append("")
        else:
            out.append(line)
    stripped = "\n".join(out)

    # Keep the line count identical here too, so the numbers above hold.
    stripped = re.sub(r"[ \t]*<!--.*?-->[ \t]*",
                      lambda m: "\n" * m.group(0).count("\n"),
                      stripped, flags=re.S)
    return stripped


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
    src = raw.decode("utf-8")

    # The one assumption strip_comments makes, checked on every run rather than
    # trusted: no template literal may contain a line that starts with `//`, or
    # removing that line would silently change a command the portal prints.
    for lit in re.findall(r"`[^`]*`", src, flags=re.S):
        if re.search(r"^\s*//", lit, flags=re.M):
            print("error: a template literal contains a //-leading line; "
                  "strip_comments would corrupt it", file=sys.stderr)
            return 1

    served = strip_comments(src).encode("utf-8")
    blob = gzip.compress(served, compresslevel=9, mtime=0)
    text = emit(blob)

    # Only write when the content actually changed, so the file mtime does not
    # churn and trigger needless rebuilds.
    if OUT.exists() and OUT.read_text(encoding="utf-8") == text:
        print(f"webui.h up to date ({len(blob)} B gzip)")
        return 0

    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{SRC.name}: {len(raw)} B -> {len(served)} B without comments "
          f"-> gzip {len(blob)} B ({100 * len(blob) / len(raw):.1f}%) -> {OUT.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
