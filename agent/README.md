# Clawdmeter agent

**Not built yet.** This is milestone 7. Until a release exists, the bootstrap
installers the device serves will report "no asset in the latest release" and point
at the releases page — they fail cleanly rather than hanging, but they cannot
install anything.

Meanwhile the firmware is driven by
**[giovi321/clawdmeter-daemon](https://github.com/giovi321/clawdmeter-daemon)**,
unchanged. The `/api/usage` contract is byte-compatible on purpose:

```bash
python clawdmeter_daemon.py --no-tray --push-to <device-ip> --no-discover
```

That is verified working against this firmware.

## What this needs to be

The daemon above is the right logic in the wrong packaging. It needs Python, a
venv, and a hand-typed IP. Hackathon attendees will not do that, so the agent has
three requirements and they are all about the first ninety seconds:

1. **One command, no runtime install.** A self-contained binary per platform, or an
   `npx @texterous/clawdmeter` entry point. Port the polling and credential logic
   from `clawdmeter_daemon.py` — in particular it refreshes through Claude Code with
   no manual OAuth token, which is the part people would otherwise get wrong.

2. **mDNS discovery by default.** Resolve `clawdmeter-*.local` rather than asking
   for an address. DHCP moves addresses; hostnames do not. Prompt only when nothing
   answers.

3. **Loud, specific failures.** Guest WiFi with AP/client isolation blocks the push
   and is the single most likely event-day failure. The agent must name that
   possibility and suggest a phone hotspot, not retry in silence. A spinner that
   never resolves is worse than an error.

## Release assets the installers expect

`web/install.ps1` and `web/install.sh` in the firmware tree look for these names on
the latest GitHub release:

```
clawdmeter-agent-windows-x64.exe
clawdmeter-agent-macos-x64      clawdmeter-agent-macos-arm64
clawdmeter-agent-linux-x64      clawdmeter-agent-linux-arm64
```

Keep those names stable, or update both scripts and regenerate
`firmware/src/agent_install.h`.

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

The device switches to the idle mascot after ~20 s without a push (see
`USAGE_STALE_GRACE_MS`), so push at least every 15 s to keep the stats on screen.
