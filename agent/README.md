# Clawdmeter agent

**There is no Clawdmeter agent binary, and there never was one.** The sender is a
Claude Code plugin: [`plugin/`](../plugin/) in this repository.

This directory is the record of how that was decided, not a placeholder for a
build.

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
scripts resolved `releases/latest` before they did anything else, so both died on
that 404, and `install.ps1` reported it as *"Could not read the release list"*,
which reads as a network fault rather than as "nothing has ever been released".

The web UI handed those two one-liners to the recipient as their **first action**,
under the heading "One command". So the first thing a person did with the device
was run something that could only fail.

Removed in two passes. First the routes (`firmware/src/Web.cpp`), leaving the
scripts gutted into stubs that printed an apology. Then the stubs themselves,
along with `firmware/tools/gen_agent.py`, the generated `firmware/src/agent_install.h`,
its `.gitignore` entry, the `pre_build.py` loop entry, and the CI determinism check
that ran the generator — a generator regenerating a blob no translation unit
included, from two scripts whose own headers said they existed to feed it.

## What drives the firmware

**The `clawd` plugin — [`plugin/`](../plugin/).**

```bash
claude plugin marketplace add Texterous/clawdmeter
claude plugin install clawd@clawdmeter
```

Then `/clawd:setup` and the four characters on the device screen.

It works because Claude Code hands its `statusLine` command a JSON blob that
already contains the numbers:

```json
"rate_limits": {
  "five_hour": { "used_percentage": 45, "resets_at": 1738425600 },
  "seven_day": { "used_percentage": 23, "resets_at": 1738857600 }
}
```

That is this device's `/api/usage` contract, one subtraction apart. So the three
requirements this file used to set for a hypothetical agent are met without a
binary existing:

1. **One command, no runtime install.** No Python, no venv, no
   `claude setup-token`, no `CLAUDE_CODE_OAUTH_TOKEN`. Claude Code is already
   installed — it is the thing being measured.
2. **A single explicit target, discovery off by default.** There is no fan-out to
   switch off: `/clawd:setup` pairs one unit, matched by the code on its screen,
   and the plugin talks to that one. The old `--no-discover` footgun cannot be
   forgotten because it does not exist.
3. **Loud, specific failures.** `/clawd:setup` names AP isolation, a subnet wider
   than the /24 it sweeps, a missing Pro/Max subscription, and portal auth
   separately, rather than spinning.

### Why not the Python daemon

[`giovi321/clawdmeter-daemon`](https://github.com/giovi321/clawdmeter-daemon) still
works and the `/api/usage` contract is byte-compatible with it on purpose. But it
should not be the recommended path, for a reason that is not about packaging:

```python
API_HEADERS_TEMPLATE = {
    "anthropic-beta": "oauth-2025-04-20",
    "User-Agent": "claude-code/2.1.146",
}
```

It reads the OAuth token out of `~/.claude/.credentials.json` and then presents
itself to the API as Claude Code. Anthropic enforces that consumer OAuth tokens
are used by Claude Code and Claude.ai and nowhere else — enforcement deployed in
January 2026 — so this is a client-identity spoof pinned to a stale version
string. It works today. It is not something to put thirty units behind.

Reading the numbers from Claude Code itself is the only sanctioned route, and it
needs no credential at all.

## Contract

POST to `http://<device>/api/usage`, unauthenticated:

```json
{"s": 42.0, "sr": 118, "w": 63.0, "wr": 4200, "st": "allowed", "ok": true,
 "sess": [{"n": "stoplicht-19", "s": "b", "t": 3}], "ns": 4}
```

| | |
|---|---|
| `s`, `w` | 5-hour and 7-day utilisation, percent |
| `sr`, `wr` | minutes until each window resets |
| `st` | status string, e.g. `allowed`, `allowed_warning`, `rejected` |
| `ok` | false marks a failed read; the device keeps the last good values and flags an error |
| `sess` | session-board rows, already sorted and clipped by the sender: `n` name, `s` `w`orking / `b`locked / `a`waiting, `t` minutes |
| `ns` | live sessions on the host; `>= sess` length when the board overflows |

`sess`/`ns` are optional — a payload without them parses exactly as one from
before the board existed. A unit in **sessions** mode reads their absence as
"your sender does not send a board" and says so on the glass, so omit them only
when you mean that.

The device treats a reading as stale after `pollSec * 2000 + USAGE_STALE_GRACE_MS`
(`UsageMode.cpp`), which at the default `pollSec` of 30 is **80 s**. The plugin's
status line refreshes every 30 s, well inside it. Past it the device shows the idle
mascot, or the last numbers under a `STALE` header when the mascot is off.

## Pull mode, and the relay that is not built

The device can also **pull** from a URL (`usageUrl` in its web UI, plain HTTP in
the no-TLS images). Nothing here uses it, but it is why a hosted relay remains
possible without a firmware change: a server would answer `usageUrl` with this
same contract, and a laptop would POST to the server instead of the LAN.

That was considered and deferred. On a home network the plugin's own discovery is
simpler and involves no third party holding anyone's usage data. The decision is
reversible per unit — `POST /api/config` can set `usageUrl` on a device already in
someone's hands — so shipping without a relay forecloses nothing.
