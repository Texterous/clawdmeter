#!/usr/bin/env python3
"""Regenerate src/agent_install.h from web/install.ps1 and web/install.sh.

The device serves these two bootstrap scripts at GET /agent/install.ps1 and
GET /agent/install.sh. They exist because a machine joined to the setup AP has
no route to the internet, so the Clawdmeter page cannot simply link a GitHub
release — but it CAN hand over a few KB of shell that installs the agent as soon
as the machine is online again.

Keep them small. They live in flash on a 4 MB chip alongside the web UI, and
their whole job is to fetch something bigger from elsewhere.

    python tools/gen_agent.py

Runs automatically before every build (see tools/pre_build.py).
Python 3 stdlib only; deterministic (gzip mtime=0).
"""
import gzip
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "src" / "agent_install.h"

# (source file, C symbol prefix)
SOURCES = [
    (ROOT / "web" / "install.ps1", "AGENT_INSTALL_PS1"),
    (ROOT / "web" / "install.sh", "AGENT_INSTALL_SH"),
]

HEADER = """\
// GENERATED FILE -- do not edit by hand.
// Produced by tools/gen_agent.py from web/install.ps1 and web/install.sh.
// Regenerate after editing either script:  python tools/gen_agent.py
//
// Gzip-compressed agent bootstrap installers, served from PROGMEM with
// `Content-Encoding: gzip` by handleInstallPs1 / handleInstallSh in Web.cpp.
#pragma once
#include <Arduino.h>
"""


def emit(name: str, blob: bytes) -> str:
    out = [f"\nstatic const uint8_t {name}_GZ[] PROGMEM = {{\n"]
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        out.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
    out.append("};\n")
    out.append(f"static const size_t {name}_GZ_LEN = sizeof({name}_GZ);\n")
    return "".join(out)


def main() -> int:
    parts = [HEADER]
    total_raw = total_gz = 0
    for path, sym in SOURCES:
        if not path.exists():
            print(f"error: {path} not found", file=sys.stderr)
            return 1
        # Normalise to LF: the scripts are served to both PowerShell and sh, and
        # a CRLF-mangled shell script fails in ways that are tedious to debug
        # from the far side of an HTTP request.
        raw = path.read_bytes().replace(b"\r\n", b"\n")
        blob = gzip.compress(raw, compresslevel=9, mtime=0)
        parts.append(emit(sym, blob))
        total_raw += len(raw)
        total_gz += len(blob)

    text = "".join(parts)
    if OUT.exists() and OUT.read_text(encoding="utf-8") == text:
        print(f"agent_install.h up to date ({total_gz} B gzip)")
        return 0

    OUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"installers: {total_raw} B -> gzip {total_gz} B -> {OUT.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
