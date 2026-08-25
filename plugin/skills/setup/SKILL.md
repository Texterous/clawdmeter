---
description: Pair this machine with a Clawdmeter desk display and start sending usage to it. Use when the user runs /clawd:setup, mentions pairing or setting up a Clawdmeter or SmallTV display, or says their Clawdmeter panel is blank or stuck on "ALMOST DONE".
---

# Set up a Clawdmeter display

Pair the device on the local network and install the status line that feeds it.
`$ARGUMENTS` may hold the 4-character code from the device screen (e.g. `a1b2`),
or an IP address, or `clawd-a1b2.local`. If it is empty, ask for the code:

> **What does your device show?** Look at the screen — it prints a name like
> `clawd-a1b2.local`. I need the last 4 characters before `.local` (here, `a1b2`).
> If the screen shows only an IP address, give me that instead.

Do not guess or invent a code. Wait for the answer.

## 1. Install the pusher to a stable path

Pick the platform's script from this plugin's `bin/` directory, and **copy it
into `~/.clawd/`**. Use the copy from here on — that copied path is `PUSHER`.

| Platform | Script | `PUSHER` | Invocation |
|---|---|---|---|
| Windows | `clawd-push.ps1` | `~/.clawd/clawd-push.ps1` | `powershell.exe -NoProfile -ExecutionPolicy Bypass -File "<PUSHER>"` |
| macOS, Linux | `clawd-push.sh` | `~/.clawd/clawd-push.sh` | `"<PUSHER>"` (make it executable) |

The copy is not incidental — do not skip it and point at the plugin directory.
Plugins install to a **version-pinned** path
(`~/.claude/plugins/cache/clawdmeter/clawd/0.1.0/bin/…`), so a status line
aimed there would break silently on the next version bump, and again if the
plugin is ever disabled or uninstalled. Copying decouples the two.

Re-running `/clawd:setup` refreshes the copy, which is how a plugin update
reaches the pusher. Mention that if the user is re-pairing after an update.

`${CLAUDE_PLUGIN_ROOT}` is a plugin-context variable and does **not** expand
inside the user's `settings.json`, so write an absolute path in step 4.

## 2. Find the device

If the user gave an IP or a `.local` name, skip the scan and verify it directly:

```
curl -s -m 3 --noproxy '*' http://<address>/api/status
```

Otherwise sweep the local /24 (about 2.5 seconds):

- Windows: `<invocation> -Scan`
- macOS, Linux: `<PUSHER> --scan`

Either form works from the plugin copy or the `~/.clawd/` copy; they are the
same script.

Each line of output is `<ip> <host>`. Match the host ending in the user's code.

**Handling the result:**

- **One match** — use it.
- **Several units, none matching the code** — list them and ask which is theirs.
  A hackathon room can hold thirty; the code is what disambiguates.
- **No matches** — do not retry the same scan. Work through *Nothing found* below.

Confirm the match before writing anything:

> Found **clawd-a1b2** at 192.168.1.47. Firmware 0.3.0. Pair with this one?

`/api/status` also returns `commissioned`. If it is already `true`, say so —
this unit has been fed before, so the user may be re-pairing rather than
setting up fresh.

## 3. Write the device config

Create `~/.clawd/` and write `~/.clawd/config` as `key=value` lines, one per
line, **UTF-8 with no BOM**:

```
ip=192.168.1.47
host=clawd-a1b2
code=a1b2
ver=0.2.0
```

`ver` is the plugin version you copied the pusher from. It is not read by the
pusher; it is there so a stale copy can be spotted by comparing it against the
plugin's own `version`.

Add a `chain=` line only in step 4, and only if there is something to chain.

## 4. Install the status line

Read `~/.claude/settings.json`. Look at `statusLine.command`.

- **Already ours** (the path contains `clawd-push`) — leave `chain` as it is.
- **Something else there** — this is the user's existing status line and it must
  keep working. Add it to `~/.clawd/config` as a single `chain=<their command>`
  line. The pusher runs it with the same stdin JSON and prints its output before
  our segment. Tell the user you did this.
- **Nothing there** — no `chain` line.

Then set, preserving every other key in the file:

```json
{
  "statusLine": {
    "type": "command",
    "command": "<invocation from step 1>",
    "refreshInterval": 30
  }
}
```

`refreshInterval` matters: without it the command only runs on session events, so
an idle session would let the panel go stale and drop to the idle mascot. 30 s
sits comfortably inside the device's 80 s stale window.

## 5. Confirm

The status line takes effect on the next render, and `rate_limits` only appears
**after the first API response of the session** — so tell the user plainly:

> Paired. Send one message and the panel will light up within about 30 seconds.

Do not claim the panel is already live. You have proven the device is reachable,
not that a reading has landed.

## Nothing found

Go through these in order. Be specific about which one you are ruling out —
a spinner that never resolves is worse than a named error.

1. **Is the device on WiFi yet?** If its screen still shows a setup AP name like
   `Clawd-a1b2` rather than an IP, it has not joined a network. They need to
   finish the portal step first.
2. **Same network?** Laptop on ethernet or a different SSID than the device is
   the most common cause. A phone hotspot with both on it settles it.
3. **A network bigger than a /24.** The sweep only covers the device's own /24.
   If their LAN is a /16 or /20, the unit can be outside it. The device screen
   prints its IP — ask for that and pass it directly.
4. **AP or client isolation.** If the device is definitely on the same SSID and
   still unreachable, the access point is refusing to forward traffic between
   clients. Nothing on this machine can fix that. Say so directly and recommend
   a phone hotspot for both devices. This is the single most likely failure at a
   venue, and it fails silently — name it rather than letting them keep retrying.
5. **Auth is on.** If `/api/status` returns 401, the unit has portal auth
   enabled. They can turn it off in the device's web UI, or give you the
   credentials to include.

## Requirements worth stating if they come up

The panel needs `rate_limits` in the status line JSON, and Claude Code only
provides that on a **Claude.ai Pro or Max** subscription. On an API-key setup the
device will sit at "ALMOST DONE" forever — that is the plan working correctly,
not a fault to debug.

The panel also only updates while Claude Code is running. Closing it lets the
reading go stale and the device falls back to its idle mascot.
