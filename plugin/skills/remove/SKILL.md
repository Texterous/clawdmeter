---
description: Stop sending usage to a Clawdmeter display and restore the previous status line. Use when the user runs /clawd:remove, or asks to unpair, disconnect, or stop feeding their Clawdmeter or SmallTV display.
---

# Remove the Clawdmeter status line

This exists because `/clawd:setup` edits the user's `~/.claude/settings.json`.
Anything that writes to a settings file owes them a clean way back out, and the
status line keeps running even if the plugin is disabled — so uninstalling the
plugin alone does not undo it.

## 1. Restore the status line

Read `~/.claude/settings.json` and check `statusLine.command`.

If it does **not** contain `clawd-push`, stop: something else owns the status
line and this skill has nothing to undo. Say so and leave the file alone.

If it is ours, read `chain` from `~/.clawd/config`:

- **A `chain` value exists** — that was their status line before we took over.
  Restore it as `statusLine.command`, and drop the `refreshInterval` we added
  unless they had one. Tell them which command you put back.
- **No `chain` value** — they had no status line before. Remove the `statusLine`
  key entirely rather than leaving an empty one behind.

Preserve every other key in the file exactly as it was.

## 2. Clean up

Delete `~/.clawd/`. It holds the copied pusher script, the device address, the
code, and any chained command — nothing worth keeping once the pairing is gone.

Mention that the device itself is untouched: it keeps its WiFi credentials and
will show its idle mascot, then fall back to "Waiting for data…" once the last
reading goes stale. To pair a different machine to it, run `/clawd:setup` there.

## 3. If they wanted to uninstall entirely

Removing the plugin as well:

```bash
claude plugin uninstall clawd@clawdmeter
```

Nothing on the device needs undoing for that. If they want the unit itself back
to a blank slate, that is `POST /api/factory` from its web UI, which also clears
its WiFi credentials — so only suggest it if that is genuinely what they want.
