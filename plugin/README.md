# clawd — the Clawdmeter plugin

Sends your Claude Code usage to a [Clawdmeter](https://github.com/Texterous/clawdmeter)
desk display over your local network. No Python, no venv, no OAuth token, no
device IP to look up.

```bash
claude plugin marketplace add Texterous/clawdmeter
claude plugin install clawd@clawdmeter
```

Then `/clawd:setup` and type the 4 characters your device shows.

## How it gets the numbers

Claude Code hands its `statusLine` command a JSON blob on stdin that includes
the account's rate-limit state:

```json
"rate_limits": {
  "five_hour": { "used_percentage": 45, "resets_at": 1738425600 },
  "seven_day": { "used_percentage": 23, "resets_at": 1738857600 }
}
```

That is exactly the device's `/api/usage` contract, one subtraction apart. So
this plugin installs a status line that reads those four numbers and POSTs them
to the unit — with no API call of its own, no credential handling, and nothing
that spends quota in order to measure quota.

This is also the only sanctioned way to read these numbers. Anthropic enforces
that consumer OAuth tokens are used by Claude Code and Claude.ai and nowhere
else, so a separate process authenticating as you is living on borrowed time.
Claude Code reads its own rate limits and passes them along; we just forward
them to a screen.

## The session board

The device's second screen lists every live Claude Code session on your machine
and what each is doing — working, waiting for you, or stuck on a permission
prompt. The plugin assembles that from `~/.claude/sessions/` and the transcript
tails, and sends it as `sess`/`ns` alongside the usage numbers.

Neither of those paths is a documented interface, so the collector is defensive:
anything odd about one session drops that session, never the board, and never the
usage reading.

- **Windows** needs nothing extra.
- **macOS and Linux need `jq`.** Shell alone cannot parse a transcript line
  correctly, and half-parsing one with `sed` would put wrong states on the glass —
  so with no `jq` the board is omitted rather than guessed. A unit in sessions
  mode then says "no session data / UPDATE YOUR SENDER", which is the honest
  reading.
- Set `board=0` in `~/.clawd/config` to switch it off. Worth doing if your device
  is on the usage screen anyway: collecting the board is most of the render cost.

## Finding the device

`/clawd:setup` sweeps the local /24 for `/api/status` answering `fw:clawdmeter`
— about 2.5 seconds, no dependencies beyond `curl` — and matches the code you
typed against each unit's hostname. The code is what tells thirty units in one
room apart. You can also pass an IP or a `.local` name directly.

Once paired, the address is cached. If a push fails the pusher re-runs discovery
in a **detached** process, at most once a minute, so a DHCP lease change or an AP
reboot heals itself instead of going quietly dark. The sweep cost lands on the
next render, never on this one.

## What it will not do

- **Work through AP or client isolation.** If the access point refuses to
  forward client-to-client traffic, nothing here can help. Put both the laptop
  and the device on a phone hotspot.
- **Find a unit outside its own /24.** On a /16 or /20 the sweep can miss it.
  The device prints its IP on screen; pass that to `/clawd:setup` instead.
- **Show anything without a Pro or Max subscription.** `rate_limits` is absent
  on API-key setups, and the device will sit at "ALMOST DONE".
- **Update while Claude Code is closed.** The reading goes stale after 80 s and
  the device falls back to its idle mascot. For a Claude usage meter that is the
  intended behaviour.

## Files it touches

| Path | What |
|---|---|
| `~/.claude/settings.json` | the `statusLine` entry, with `refreshInterval: 30` |
| `~/.clawd/config` | device address, code, and any chained status line |
| `~/.clawd/clawd-push.*` | a copy of the pusher, so the status line survives plugin updates |

Read from, never written: `~/.claude/sessions/*.json` and `~/.claude/projects/**/*.jsonl`, for the session board only.

An existing status line is **chained**, not replaced: the pusher runs it with the
same stdin and prints its output before our own segment. `/clawd:remove` puts it
back and deletes `~/.clawd/`.

## The pusher directly

```
clawd-push            read stdin, push, print a status line
clawd-push --scan     sweep a /24 and print "<ip> <host>" for every unit found
clawd-push --discover re-resolve the configured unit and update the config
```

On Windows the flags are `-Scan` and `-Discover`, with an optional `-Prefix`.

Both scripts are written for the shell that is always present — POSIX `sh` with
a `sed` fallback when `jq` is missing, and Windows PowerShell 5.1 with no
ternaries, no null-coalescing and no `-Parallel`.

## Self-hosting instead

The device also accepts pushes from anything that can POST its contract, and can
pull from a URL you host (`usageUrl` in its web UI). This plugin is a
convenience, not a dependency — nothing about the unit requires it.
