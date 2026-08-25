---
description: Pair this machine with a Clawdmeter desk display so it shows your live Claude Code sessions. Use when the user runs /clawd:setup, mentions pairing or setting up a Clawdmeter or SmallTV display, or says their Clawdmeter panel is blank or stuck on "ALMOST DONE" or "waiting...".
---

# Set up a Clawdmeter display

Pairing is one file. The plugin's hooks do the rest — they are already loaded in
this session, so the board starts filling in on the next tool call with no
restart and nothing added to the user's settings.

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

Otherwise sweep the local /24 (about 2.5 seconds) with the finder in this
plugin's `bin/`:

- Windows: `powershell.exe -NoProfile -ExecutionPolicy Bypass -File "<bin>/clawd-find.ps1"`
- macOS, Linux: `sh "<bin>/clawd-find.sh"`

Each line is `<ip> <host>`. Match the host ending in the user's code. With
several units and no match, list them and ask — a hackathon room can hold thirty,
and the code is what tells them apart. With none, work through *Nothing found*.

Confirm before writing:

> Found **clawd-a1b2** at 192.168.1.47, firmware 0.3.0. Pair with this one?

## 2. Write the config

`~/.clawd/config`, `key=value` lines, **UTF-8 with no BOM**:

```
ip=192.168.1.47
host=clawd-a1b2
code=a1b2
```

That is the whole of it. Nothing goes into `~/.claude/settings.json`, and no
script is copied anywhere — the hooks ship inside the plugin and reference it
directly, so an update or an uninstall needs no follow-up.

## 3. Widen the device's stale window

```
curl -s -m 5 -X POST -H 'Content-Type: application/json' \
     -d '{"usage":{"pollSec":900}}' http://<address>/api/config
```

This matters. The device calls a reading stale after `pollSec * 2 + 20` seconds,
so the default 30 leaves only **80 seconds** — and hooks only fire when the user
does something. Without this, the board drops to "waiting..." after a minute and
a half of thinking time. At 900 the tolerance is **30 minutes**, which reads as
"you have not touched Claude in a while" rather than as a fault.

Also confirm the device is on the session board, since that is what the plugin
feeds: `mode` should be `sessions` in `/api/config`. Units ship that way; if this
one is on `usage`, offer to switch it, because the usage meters need rate-limit
figures the hooks are never given (see *What this cannot show*).

## 4. Prove it works

Do something that fires a tool call, then read the device back:

```
curl -s -m 3 http://<address>/api/status
```

The `meter` object reports the board directly, so verify rather than assume:

- `boardValid: true` — the device has a board
- `rows` / `live` — sessions drawn and sessions known
- `usageValid: false` — expected; the hooks carry no rate-limit figures

Then say plainly what they will see:

> Paired. Your device is showing **N** session(s). It updates as you work.

If `boardValid` is false, the device has data but no board — the payload is
getting through and the board is not. Check `~/.clawd/sessions/` has files in it.

## Nothing found

Be specific about what you are ruling out; a spinner that never resolves is worse
than a named error.

1. **Is the device on WiFi?** If the screen shows a setup AP name like
   `Clawd-a1b2` rather than an IP, it has not joined a network yet.
2. **Same network?** A laptop on ethernet and the device on WiFi will not meet.
3. **A network bigger than a /24.** The sweep covers 254 addresses. On a /16 or
   /20 the unit can sit outside it — the screen prints its IP, so ask for that.
4. **AP or client isolation.** If the device is definitely on the same SSID and
   still unreachable, the access point is refusing to forward client-to-client
   traffic. Nothing here can fix that: recommend a phone hotspot for both. This
   is the most likely failure at a venue and it fails silently.
5. **Auth is on.** A 401 from `/api/status` means portal auth is enabled.

## What this cannot show

The **5-hour and 7-day usage meters**, and the mascot that goes with them. Those
need `rate_limits`, which Claude Code only puts in a `statusLine` payload — and a
`statusLine` never runs in the desktop app, only in a terminal. The session board
needs no such thing, which is why it works in both.

So a unit paired this way shows sessions, everywhere, always. If the user
specifically wants the meters and works in a terminal, that is a separate setup
and worth saying so rather than half-installing something that silently does
nothing on their machine.
