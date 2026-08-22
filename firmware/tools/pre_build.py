"""PlatformIO pre-build hook: regenerate the PROGMEM blobs before compiling.

Wired up by `extra_scripts = pre:tools/pre_build.py` in platformio.ini. Its only
job is to make it impossible to ship a stale generated header — editing
web/webui.html and forgetting to rerun the generator is the obvious way to spend
an hour wondering why a UI change did not appear on the device.

The splash bitmap is deliberately NOT regenerated here: gen_logo.py needs an SVG
rasteriser that is not a build dependency, so src/splash/logo.h is committed and
only refreshed by hand when the brand mark changes.

All generators are deterministic and only rewrite a file whose content actually
changed, so this adds no rebuild churn.
"""
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821 — injected by SCons

TOOLS = Path(env.subst("$PROJECT_DIR")) / "tools"  # noqa: F821

for script in ("gen_webui.py", "gen_agent.py"):
    path = TOOLS / script
    if not path.exists():
        print(f"pre_build: {script} missing, skipping")
        continue
    # sys.executable is PlatformIO's own interpreter, which is guaranteed to
    # exist here; the generators are stdlib-only so that is enough.
    result = subprocess.run([sys.executable, str(path)],
                            capture_output=True, text=True)
    out = (result.stdout or "").strip()
    if out:
        print(f"pre_build: {out}")
    if result.returncode != 0:
        err = (result.stderr or "").strip()
        # Fail the build rather than compiling against a stale header.
        raise SystemExit(f"pre_build: {script} failed:\n{err}")
