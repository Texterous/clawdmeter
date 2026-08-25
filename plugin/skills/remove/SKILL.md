---
description: Stop sending sessions to a Clawdmeter display. Use when the user runs /clawd:remove, or asks to unpair, disconnect, or stop feeding their Clawdmeter or SmallTV display.
---

# Unpair a Clawdmeter display

There is almost nothing to undo. The plugin's hooks live inside the plugin and
touch no Claude Code settings, so unpairing is deleting one directory.

## 1. Delete the pairing

Remove `~/.clawd/`. It holds only the device address and one flat line per
session — nothing worth keeping once the pairing is gone.

The hooks stay installed and keep firing, but `clawd-report` exits immediately
when there is no `ip` in the config, so they cost nothing. To stop them running
at all, uninstall the plugin:

```bash
claude plugin uninstall clawd@clawdmeter
```

## 2. Tell them what the device will do

It keeps its WiFi credentials and its last board on screen, then falls to
"waiting..." once that reading goes stale — 30 minutes on a pushed unit. That
screen prints the unit's code and the pairing command, so it is the device saying
nothing is feeding it, not a fault.

To pair a different machine, run `/clawd:setup` there.

## If they had the usage meters too

Older versions of this plugin wrote a `statusLine` into `~/.claude/settings.json`
pointing at a copied `~/.clawd/clawd-push.*`. If that entry is still there, it is
dead weight — remove the `statusLine` key (or restore whatever it replaced, if
they remember) and mention it, since it never worked in the desktop app and does
nothing now.
