---
description: Pair this machine with a Clawdmeter desk display so it shows your live Claude Code sessions. Use when the user runs /clawd:setup, mentions pairing or setting up a Clawdmeter or SmallTV display, or says their Clawdmeter panel is blank or stuck on "ALMOST DONE" or "waiting...".
---

# Set up a Clawdmeter display

Pairing is one file. The plugin's hooks do the rest, and nothing is added to the
user's Claude Code settings.

One caveat to get right: **plugin hooks only start firing after a restart.** If
the plugin was installed or updated in this session, the installer already said
so. Step 4 checks whether they are live rather than assuming, so follow it.

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

Each line is `<ip> <host>`. Match the host ending in the user's code — or read
`code` straight out of `/api/status`, which is the same string the device prints.
With several units and no match, list them and ask: a hackathon room can hold
thirty, and the code is what tells them apart. With none, work through
*Nothing found*.

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

**Write `host`, not just `ip`.** The address is a DHCP lease on a device that
roams; with `host` present the hooks relocate the unit by themselves when it
moves and rewrite `ip`. With only `ip`, the pairing dies the next time the router
hands out a different one.

Nothing else needs configuring. Firmware 0.3.1 and later allow 30 minutes of
quiet on a pushed unit, so there is no stale window to widen — earlier versions
needed a `pollSec` POST here and dropped to "waiting..." after 80 seconds without
it. If `/api/status` reports a version below 0.3.1, offer the OTA rather than
working around it.

## 3. Check whether the hooks are live, and prove it

Delete `~/.clawd/sessions/` if it exists, then run any tool call. If the hooks
are live, that directory reappears with a line in it.

Confirm the device is on the session board while you are here — that is what the
plugin feeds. `mode` should be `sessions` in `/api/config`; units ship that way.
If this one is on `usage`, offer to switch it: the usage meters need rate-limit
figures the hooks are never given (see *What this cannot show*).

**Empty means the hooks have not loaded yet** — the plugin was installed in this
session. Say so and stop there:

> Paired. **Restart Claude Code once** and the board starts filling in. After
> that it stays live — no further restarts.

**Populated means it is working.** Read the device back:

```
curl -s -m 3 http://<address>/api/status
```

The `meter` object reports the board, so verify rather than assume:

- `boardValid: true` — the device has a board
- `rows` / `live` — sessions drawn and sessions known
- `usageValid: false` — expected; the hooks carry no rate-limit figures

> Paired. Your device is showing **N** session(s). It updates as you work.

If `~/.clawd/sessions/` has files but `boardValid` is false, the payload is
reaching the device and the board is not — that is a bug worth reporting, not a
setup problem.

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
