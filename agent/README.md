# Clawdmeter agent

**There is no Clawdmeter agent, and the device no longer pretends there is one.**

This directory is the record of that decision, not a placeholder for a build.

## What was here, and why it went

The plan was a self-contained per-platform binary, shipped as a GitHub release
asset, installed by a one-liner the device served from its own flash
(`GET /agent/install.ps1`, `GET /agent/install.sh`). It was never built. The
installers were, and they shipped, and they could not work:

```
GET https://api.github.com/repos/Texterous/clawdmeter/releases        -> 200, []
GET https://api.github.com/repos/Texterous/clawdmeter/releases/latest -> 404
```

An empty array rather than a 404 on the first call means the repo is reachable and
simply has no releases — so this was not an auth artefact and not transient. Both
scripts resolve `releases/latest` before they do anything else, so both died on
that 404, and `install.ps1` reported it as *"Could not read the release list"*,
which reads as a network fault rather than as "nothing has ever been released".

The web UI handed those two one-liners to the recipient as their **first action**,
under the heading "One command". So the first thing a person did with the device
was run something that could only fail, with no fallback and — with no printed card
in the box — nothing else to try.

They are removed rather than stubbed. A stub is a second, worse install story that
a curious recipient can still find, and it keeps paying ~3 KB of flash for a script
whose only job would be to apologise. The device's own web UI carries the real
instructions instead: it is already in front of the recipient (they had to open it
to join WiFi), it can be read on a phone and acted on later on a laptop, and it can
be corrected without reflashing thirty units.

Removed together: `firmware/web/install.ps1`, `firmware/web/install.sh`,
`firmware/src/agent_install.h`, `firmware/tools/gen_agent.py`, and the two
`/agent/install.*` routes in `Web.cpp`.

## What actually drives the firmware

**[giovi321/clawdmeter-daemon](https://github.com/giovi321/clawdmeter-daemon)**,
unchanged. The `/api/usage` contract is byte-compatible on purpose. It needs Python
3.10 or newer, a venv, and a Claude credential — which is exactly the packaging
problem the agent was meant to solve, and the reason the honest answer is to
document the daemon well rather than to ship a binary that does not exist.

The recipient-facing version of this lives in the device's web UI (Clawdmeter tab).
Keep the two in step; this is the copy that gets checked against the source.

1. Python 3.10+. On Windows, tick **Add python.exe to PATH** in the installer.
2. Download the repo (green **Code** button → **Download ZIP**) and unzip it.
3. `.\install.bat` (Windows) or `./install.sh` (macOS, Linux). Both create a `.venv`
   and install the dependencies into it.
4. Authorise it: `claude setup-token`, then put the printed value in
   `CLAUDE_CODE_OAUTH_TOKEN`. On Windows, `setx` needs a **new** terminal
   afterwards or the variable is not there.
5. Run it at one device:

```bash
.\start-daemon.bat --push-to <device-ip-or-host> --no-discover              # Windows
./start-daemon.sh --push-to <device-ip-or-host> --no-discover               # macOS, Linux
.venv\Scripts\python clawdmeter_daemon.py --no-tray --push-to <d> --no-discover   # console
.venv/bin/python clawdmeter_daemon.py --no-tray --push-to <d> --no-discover       # console
```

Two details in those lines are load-bearing, and both were wrong here before:

**No `--no-tray` on the two `start-daemon` forms.** `start-daemon.bat` runs
`start "" .venv\Scripts\pythonw.exe clawdmeter_daemon.py --tray %*` and
`start-daemon.sh` runs `nohup "$PY" clawdmeter_daemon.py --tray "$@" >/dev/null
2>&1 &`. Both detach with no console, so `--no-tray` (`use_tray = not
args.no_tray`, `clawdmeter_daemon.py:1652`) leaves a process with no tray icon, no
window and no output — nothing to look at and nothing to quit but Task Manager. The
tray icon *is* the feedback for those two. `--no-tray` belongs on the third and
fourth forms, which keep a window and print why they cannot reach the device.

**The leading `.\` on Windows.** These commands carry arguments, so they have to be
pasted into a terminal rather than double-clicked, and the terminal on Windows
10/11 is PowerShell — which does not resolve a bare command name from the current
directory (`start-daemon.bat …` → *"is not recognized as a name of a cmdlet…"*). A
relative path that already contains a separator, like `.venv\Scripts\python`, needs
no prefix.

### `--no-discover` is not optional

Leaving it off puts your usage on **every** Clawdmeter on the network, including
other people's. In a room of thirty giveaway units that is thirty screens.

This is stronger than "do not use bare `--push`". `--push-to` alone fans out too:

```python
# clawdmeter_daemon.py
if self.cfg.get("discover", True):        # :1035 — defaults to TRUE
    disc = Discovery()
...
urls = list(_static)                      # --push-to targets
if _disc is not None:
    for u in _disc.urls(): ...            # + everything mDNS found
```

`--push-to` sets the target list and selects the push transport (`:1625-1628`); it
does **not** disable discovery. Only `--no-discover` clears the flag
(`:1629-1630`). The firmware advertises `_clawdmeter._tcp` with the usage path in
its TXT record (`Net.cpp`), and `/api/usage` is unauthenticated by design
(`Web.cpp`), so every unit the daemon can see it will write to.

Never document `--push`. Never document `--push-to` without `--no-discover`.

### The other thing that catches people

A fresh install defaults to **serve**, not push:

```python
initial = chosen or cfg.get("transport")
if not initial:
    initial = "push" if cfg.get("push_url") else "serve"   # :1643-1646
```

With no config and no `CLAWDMETER_PUSH_URL`, that is `serve` — the daemon starts,
reports success, listens on `:8787`, and the panel stays blank. A transport flag on
the command line is what makes the difference, which is why every line above
carries one.

### And the failure nobody diagnoses

Guest WiFi with AP/client isolation blocks the push silently: the daemon POSTs into
the void and the screen never changes. It is the single most likely event-day
failure. The answer is a phone hotspot with both the laptop and the device on it.

## If someone does build the agent

The three requirements have not changed, and they are all about the first ninety
seconds:

1. **One command, no runtime install.** A self-contained binary per platform. Port
   the credential logic from `clawdmeter_daemon.py` — in particular it refreshes
   through Claude Code with no manual OAuth token, which is the step people
   otherwise get wrong.
2. **A single explicit target, discovery off by default.** The inverse of the
   daemon's default, for the reason above. Prompt for an address; do not fan out.
3. **Loud, specific failures.** Name AP isolation and suggest the hotspot. A
   spinner that never resolves is worse than an error.

Do not reinstate the served installers unless releases actually exist.

## Contract

POST to `http://<device>/api/usage`, unauthenticated:

```json
{"s": 42.0, "sr": 118, "w": 63.0, "wr": 4200, "st": "allowed", "ok": true}
```

| | |
|---|---|
| `s`, `w` | 5-hour and 7-day utilisation, percent |
| `sr`, `wr` | minutes until each window resets |
| `st` | status string, e.g. `allowed`, `allowed_warning`, `rejected` |
| `ok` | false marks a failed read; the device keeps the last good values and flags an error |

The device treats a reading as stale after `pollSec * 2000 + USAGE_STALE_GRACE_MS`
(`UsageMode.cpp`), which at the default `pollSec` of 30 is **80 s**, not the ~20 s
this file used to claim — `USAGE_STALE_GRACE_MS` is 20,000 on its own but it is a
grace period added to two poll intervals, not the whole budget. The daemon's
default push interval is well inside that. Past it the device shows the idle mascot
(or, with the mascot turned off, the last numbers under a `STALE` header).
