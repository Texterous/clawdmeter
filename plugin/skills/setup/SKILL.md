---
description: Pair this machine with a Clawdmeter desk display so it shows your live Claude Code sessions. Use when the user runs /clawd:setup, mentions pairing or setting up a Clawdmeter or SmallTV display, or says their Clawdmeter panel is blank, stuck on "ALMOST DONE", or showing "no contact".
---

# Set up a Clawdmeter display

Two steps: write one config file, install one background agent. **No Claude Code
restart, and nothing added to the user's `settings.json`.**

That is the point of the agent. It reads the live sessions out of
`~/.claude/sessions/*.json` and derives each one's state from its transcript's
mtime, so pairing does not depend on the plugin's hooks having loaded — which
they only do after a restart. The hooks still help when they are live (a hook is
the only thing that can see a permission prompt), and the agent prefers their
word when it is fresh. They are an upgrade, not a requirement.

`$ARGUMENTS` may hold the 4-character code from the device screen (e.g. `a1b2`),
an IP address, or `clawd-a1b2.local`. If it is empty, ask:

> **What does your device show?** The screen prints a name like
> `clawd-a1b2.local` — I need the last 4 characters (`a1b2`). If it shows only an
> IP address, give me that instead.

Do not guess a code. Wait for the answer.

## 1. Find the device

Given an IP or `.local` name, verify it directly:

```
curl -s -m 3 --noproxy '*' http://<address>/api/status
```

Otherwise sweep with the finder in this plugin's `bin/`:

- Windows: `powershell.exe -NoProfile -ExecutionPolicy Bypass -File "<bin>/clawd-find.ps1"`
- macOS, Linux: `sh "<bin>/clawd-find.sh"`

Each line is `<ip> <host>`. Match the host ending in the user's code — or read
`code` straight out of `/api/status`, which is the same string the device prints.
With several units and no match, list them and ask: a hackathon room can hold
thirty, and the code is what tells them apart.

**Nothing found? Add `-Wide` (or `--wide`) and try again before concluding
anything.** The plain sweep covers this machine's own /24; `-Wide` covers the
enclosing /20 in about 30 seconds. This is not a rare case — measured on the
network this was built on, the laptop was on `10.94.13.251` and the device on
`10.94.14.114`, three /24s apart on one SSID, where the narrow sweep can never
succeed. Both sweeps already run two passes internally, so a single empty result
is a real answer and does not need re-running by hand.

Confirm before writing:

> Found **clawd-a1b2** at 192.168.1.47, firmware 0.5.0. Pair with this one?

## 2. Write the config

`~/.clawd/config`, `key=value` lines, **UTF-8 with no BOM**:

```
ip=192.168.1.47
host=clawd-a1b2
code=a1b2
```

**Write `host`, not just `ip`.** The address is a DHCP lease on a device that
roams; with `host` present the agent relocates the unit by itself when it moves
and rewrites `ip`. With only `ip`, the pairing dies the next time the router
hands out a different one.

## 3. Install the agent

From this plugin's `bin/`:

- Windows: `powershell.exe -NoProfile -ExecutionPolicy Bypass -File "<bin>/clawd-agent.ps1" -Install`
- macOS, Linux: `sh "<bin>/clawd-agent.sh" --install`

It copies itself to `~/.clawd/bin/`, registers a login item (HKCU Run on
Windows, launchd on macOS, a systemd user unit on Linux), and starts immediately.
No elevation is needed on any of the three.

**Why it copies itself out of the plugin:** a plugin's install path is
version-pinned (`~/.claude/plugins/cache/<marketplace>/<plugin>/<version>/`), so
a login item pointing into it would break silently on the next plugin update and
again if the plugin were disabled.

## 4. Verify on the device, not on faith

```
curl -s -m 3 http://<address>/api/status
```

The `meter` object answers every question worth asking:

- `boardValid: true` — the device has a board
- `fresh: true` — it counts as live right now
- `rows` / `live` — sessions drawn and sessions known
- `restored: false` — these rows arrived, they are not last night's from flash
- `sender` — the address and port the device will call to ask for a push
- `hb: 15` — the heartbeat it is measuring silence against
- `usageValid: false` — expected; see *What this cannot show*

> Paired. Your device is showing **N** session(s), and it keeps showing them with
> Claude Code closed. Restarting either end is fine — it comes back by itself.

If `boardValid` is false a few seconds after the install, check
`~/.clawd/agent.log`: it records every push, every address change and every
reason a send failed.

## Nothing found

Be specific about what you are ruling out; a spinner that never resolves is worse
than a named error.

1. **Is the device on WiFi?** If the screen shows a setup AP name like
   `Clawd-a1b2` rather than an IP, it has not joined a network yet.
2. **Did you try `-Wide`?** See step 1. On a /20 or /16 the narrow sweep cannot
   reach it, and this is the single most likely reason for an empty result.
3. **Same network?** A laptop on ethernet and the device on WiFi will not meet.
4. **A network bigger than a /20.** The wide sweep covers 4064 addresses. On a
   /16 the unit can still sit outside it — the screen prints its IP, so ask.
5. **AP or client isolation.** If the device is definitely on the same SSID and
   still unreachable, the access point is refusing to forward client-to-client
   traffic. Nothing here can fix that: recommend a phone hotspot for both. This
   is the most likely failure at a venue and it fails silently.
6. **Auth is on.** A 401 from `/api/status` means portal auth is enabled.

## Re-running this

It is idempotent. `-Install` stops any running agent, refreshes the copy in
`~/.clawd/bin/`, and starts the new one — so it is also the way to update the
agent after a plugin update.

## What this cannot show

The **5-hour and 7-day bars** under the board. They need `rate_limits`, which
Claude Code only puts in a `statusLine` payload — and a `statusLine` never runs in
the desktop app, only in a terminal. So they stay empty, and the device leaves
those two rows blank rather than drawing zeroes.

Do not offer to "switch screens" to fix it. There is one screen; the windows are
a footer on it.
