# clawd — the Clawdmeter plugin

Shows your live Claude Code sessions on a [Clawdmeter](https://github.com/Texterous/clawdmeter)
desk display. One row per session: working, waiting for you, or blocked on a
permission prompt.

```bash
claude plugin marketplace add Texterous/clawdmeter
claude plugin install clawd@clawdmeter
```

Then `/clawd:setup` and the 4 characters your device shows.

## How it works

Every Claude Code session has this plugin, so **every session reports itself**.
A hook fires, and the event says what that session is doing:

| Hook event | State |
|---|---|
| `PostToolUse`, `UserPromptSubmit` | working |
| `Stop`, `SessionStart` | waiting for you |
| `Notification` (`permission_prompt`) | **blocked** |

Each session writes one flat line to `~/.clawd/sessions/<id>`, then the board is
rebuilt from those lines and POSTed to the device.

That is the whole design, and it is why there is **nothing to install**: no
Python, no `jq`, no Node, no credentials, no background process. Plain `sh` and
`curl` on macOS and Linux, PowerShell on Windows.

It also beats inferring state from transcripts, which is what the old Python
daemon did — it guessed "blocked" from thirty seconds of file silence and could
not tell a permission prompt from a slow build. A hook is told.

## Works in the desktop app

This is the reason for the hook design. Claude Code's `statusLine` — the only
thing that ever receives rate-limit figures — **never runs in the desktop app**,
only in a terminal. Hooks are not UI, so they run everywhere.

The trade is that the hooks are never given `rate_limits`, so this plugin fills the
**session board** and leaves the 5h/7d bars in its footer empty. Those two bars are
the only place those figures live now — the separate usage screen is gone, because
from the desktop app it could never be anything but blank.

## What it will not do

- **Work through AP or client isolation.** If the access point refuses
  client-to-client traffic, nothing here helps. Use a phone hotspot for both.
- **Find a unit outside its own /24.** On a /16 or /20 the sweep can miss it; the
  device prints its IP on screen, so pass that to `/clawd:setup` instead.
- **Fill the 5h/7d bars.** They sit under the board and stay empty here. See above.
- **Update while Claude Code is closed.** Hooks fire on activity. The device allows
  30 minutes of quiet before it says it has lost contact, so ordinary thinking time
  never trips it.

## When the device moves

Its address is a DHCP lease, and the unit roams between networks. When the stored
one stops answering, the hook asks `clawd-find` where the unit went — `<host>.local`
first, then a sweep of this machine's /24 — and rewrites `ip` in the config. That
retry is throttled to once every five minutes, so an absent device costs a probe
and nothing more.

If it cannot be found at all, the panel falls to `waiting...` and prints the
command and the code needed to pair again. That is the only manual step left.

## Files it touches

| Path | What |
|---|---|
| `~/.clawd/config` | the device address, name and code |
| `~/.clawd/sessions/*` | one flat line per session: `name\|state\|since` |
| `~/.clawd/push.stamp` | the board last confirmed on the device |

**Nothing in `~/.claude/`.** The hooks ship inside the plugin and reference it
directly, so updating or uninstalling needs no follow-up. `/clawd:remove` deletes
the one directory.

Stale entries expire after 15 minutes, so a session that dies without a
`SessionEnd` drops off the board by itself.

A POST goes out when the board actually **changes**, not on a timer, plus one
every five minutes to keep the device's freshness clock ticking. That is what
makes "you stopped and it still says working" impossible: the state transition is
never the one a throttle drops.

## Checking it from outside

`GET /api/status` on the device reports the board, so you never have to read the
glass to know whether it is working:

```json
"meter": { "boardValid": true, "usageValid": false, "rows": 3, "live": 3 }
```
