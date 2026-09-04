# clawd — the Clawdmeter plugin

Shows your live Claude Code sessions on a [Clawdmeter](https://github.com/Texterous/clawdmeter)
desk display. One row per session: working, waiting for you, or blocked on a
permission prompt.

```bash
claude plugin marketplace add Texterous/clawdmeter
claude plugin install clawd@clawdmeter
```

Then `/clawd:setup` and the 4 characters your device shows. No Claude Code
restart, and nothing written to your `settings.json`.

## How it works

An **agent** — one small background process, started at logon — owns the display.
Once a second it works out what every live Claude Code session is doing and posts
a board to the device.

It gets the sessions from `~/.claude/sessions/*.json`, which Claude Code writes
for every window (pid, session id, cwd, name), and it decides what each one is
doing from that session's transcript: Claude Code appends to the transcript every
few seconds through a turn and goes quiet the moment the turn ends, so "written
in the last 25 seconds" means the model holds the turn. No parsing, no
credentials, no API calls.

The plugin's **hooks** are still installed, and they still matter — but as a
precision layer rather than the transport:

| Hook event | State |
|---|---|
| `PostToolUse`, `UserPromptSubmit` | working |
| `Stop`, `SessionStart` | waiting for you |
| `Notification` (`permission_prompt`) | **blocked** |

Each session writes one flat line to `~/.clawd/sessions/<id>`, and the agent
prefers that line over its own inference whenever it is under 90 seconds old,
because a hook is *told* what happened. **Blocked** is the state that needs it: a
session stalled on a permission prompt looks exactly like a session thinking hard
from the outside, and nothing but the hook can see the difference.

When the agent is running, the hooks stop pushing and only write their line — two
senders would fight over the device's address and halve the value of the
heartbeat.

### Why an agent, and not just hooks

Hooks only fire while Claude Code is open. That left three holes, all of which
people actually hit:

- **A closed laptop.** Nothing fires all evening, so the panel went stale at
  half past six and stayed there.
- **A power-cycled device.** Nothing fires until the next tool call, which on a
  shut laptop is never — so a device that had been working showed `waiting...` on
  every startup.
- **A ten-second budget.** A hook has to return fast, and re-finding a moved unit
  does not fit in it.

The agent holds a 15-second heartbeat, which is also what lets the device tell
"quiet" from "broken" — it declares the interval in the payload (`hb`) and the
device treats three missed beats as silence, instead of guessing at half an hour.

## Neither end needs to find the other twice

Both ends of this link move. The device re-rolls its DHCP lease on most boots,
and the laptop roams between /24s on the same SSID (measured: laptop on
`10.94.13.251`, device on `10.94.14.114`, one network).

So each side learns the other's address for free:

- The **device** takes the agent's address from the source IP of every push, and
  the agent's port from the payload. On boot it asks for a board
  (`GET /refresh`), which is why a power cycle now costs about a second of stale
  screen instead of a heartbeat.
- The **agent** takes the device's address from that same request. A unit that
  came back on a new lease repairs the pairing by announcing itself.

Only when both of those fail does anything sweep the network, and then
`clawd-find` alternates a fast /24 pass with a `-Wide` /20 pass — because on a /20
the narrow sweep cannot succeed at all. Every sweep runs twice internally: a
single pass demonstrably misses a unit that is present and answering.

## Works in the desktop app

Claude Code's `statusLine` — the only thing that ever receives rate-limit figures
— **never runs in the desktop app**, only in a terminal. Neither the agent nor the
hooks are UI, so both run everywhere.

The trade is that neither is given `rate_limits`, so this plugin fills the
**session board** and leaves the 5h/7d bars in its footer empty. Those two bars are
the only place those figures live now — the separate usage screen is gone, because
from the desktop app it could never be anything but blank.

## What it will not do

- **Work through AP or client isolation.** If the access point refuses
  client-to-client traffic, nothing here helps. Use a phone hotspot for both.
- **Find a unit outside its own /20.** `-Wide` covers 4064 addresses. On a /16 the
  unit can still sit outside that; the device prints its IP on screen, so pass
  that to `/clawd:setup` instead.
- **Fill the 5h/7d bars.** They sit under the board and stay empty here. See above.
- **Show anything while the machine is off.** The device then shows the last board
  it received, dimmed, with the time it was received — see below.

## What the device shows when nothing is arriving

Never a blank screen, and never the word "waiting" — that word described the
device's own state machine rather than anything the reader could do, and it was
what greeted people on every power cycle.

| Situation | Screen |
|---|---|
| Fed, sessions running | the board, live |
| Fed, nothing running | `nothing running` |
| Machine asleep or Claude Code closed | the same rows, dimmed, `LAST SEEN 18:42` |
| Just power-cycled | the board restored from flash, `LAST KNOWN 22:41` |
| Never fed at all | `no contact`, the pairing command, and its own IP |

The timestamps are the *sender's* clock, carried in the payload (`ts`/`tzo`). The
device has no working NTP path and no time zone anyone set, and the machine it is
talking to knows the answer — so the answer comes with the data.

## Files it touches

| Path | What |
|---|---|
| `~/.clawd/config` | the device address, name and code |
| `~/.clawd/bin/` | the agent's own copy of itself, plus the finder |
| `~/.clawd/sessions/*` | one flat line per session from the hooks: `name\|state\|since` |
| `~/.clawd/agent.state` | per-session state and when it started, so "12m" means that |
| `~/.clawd/agent.pid` | the running agent, and how the hooks know to stay quiet |
| `~/.clawd/agent.log` | every push, address change and failure, capped at 64 KB |

**Nothing in `~/.claude/`.** One login item is registered — an HKCU `Run` value on
Windows, a launchd agent on macOS, a systemd user unit on Linux — and
`/clawd:remove` takes it out again.

The agent copies itself to `~/.clawd/bin/` rather than being launched from the
plugin directory, because a plugin's install path is version-pinned
(`~/.claude/plugins/cache/<marketplace>/<plugin>/<version>/`) and a login item
pointing into it would break on the next update. Re-run `/clawd:setup` after a
plugin update to refresh that copy.

## Checking it from outside

`GET /api/status` on the device reports the board, so you never have to read the
glass to know whether it is working:

```json
"meter": {
  "boardValid": true, "usageValid": false, "rows": 3, "live": 3,
  "fresh": true, "restored": false, "staleSec": 65,
  "hb": 15, "sender": "10.94.13.251:8788"
}
```

`fresh` is the one-field answer to "is it live right now". `restored: true` means
the rows on the glass came out of flash at boot rather than over the network.

On the sending side, `clawd-agent.ps1 -Report` (or `clawd-agent.sh --report`)
prints the config, whether the agent is running, whether it is registered at
logon, and the exact payload it would send.

## macOS and Linux

Same agent, one deliberate difference: **no listener**. The Windows version opens
a tcp port so a rebooted device can ask for a push and be live about a second
later; doing that in POSIX `sh` would mean depending on `nc -l`, whose flags
differ between the BSD, GNU and busybox netcats. The heartbeat covers the same
ground at lower resolution — a boot costs up to 15 seconds of restored board
instead of about one — so the payload omits `p` and the device does not knock on
a port nobody is behind.
