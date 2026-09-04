---
description: Stop sending sessions to a Clawdmeter display. Use when the user runs /clawd:remove, or asks to unpair, disconnect, or stop feeding their Clawdmeter or SmallTV display.
---

# Unpair a Clawdmeter display

Two things to undo, and the order matters: stop the agent first, because it is a
running process and it would otherwise rewrite the config file you are about to
delete.

## 1. Stop and unregister the agent

From this plugin's `bin/` (or from `~/.clawd/bin/`, where `-Install` copied it —
either works, they are the same script):

- Windows: `powershell.exe -NoProfile -ExecutionPolicy Bypass -File "<bin>/clawd-agent.ps1" -Uninstall`
- macOS, Linux: `sh "<bin>/clawd-agent.sh" --uninstall`

That kills the running agent and removes the login item — the HKCU `Run` value on
Windows, the launchd plist on macOS, the systemd user unit on Linux. Nothing is
left to start at the next logon.

**Do not skip this and just delete `~/.clawd/`.** The login item would survive,
start an agent at every logon, and that agent would exit on a missing config —
harmless, but a mystery for whoever finds it later.

## 2. Delete the pairing

Remove `~/.clawd/`. It holds the device address, one flat line per session, the
agent's own state clock and its log — nothing worth keeping once the pairing is
gone.

The plugin's hooks stay installed and keep firing, but `clawd-report` exits
immediately when there is no `ip` in the config, so they cost nothing. To stop
them running at all, uninstall the plugin:

```bash
claude plugin uninstall clawd@clawdmeter
```

## 3. Tell them what the device will do

It keeps its WiFi credentials and the last board it received, and goes on showing
those rows — dimmed, with `LAST SEEN 18:42` under them — because that is the
honest thing for a device that has lost its sender. It will not blank, and it
will not claim to be live.

On a power cycle it restores that same board from flash and shows it as
`LAST KNOWN`. Only a unit that has never been fed at all shows `no contact` and
the pairing command.

To pair a different machine, run `/clawd:setup` there. Nothing needs clearing on
the device first: the first payload from the new machine replaces the board and
the remembered sender in one go.

## If they want the device truly blank

`POST /api/factory` wipes the config, the stored board and the remembered sender,
then reboots into setup-AP mode. That is the right call before handing the unit
to someone else — the board on flash carries their session names, and the
remembered sender carries an address on their network.

## If they had the usage meters too

Older versions of this plugin wrote a `statusLine` into `~/.claude/settings.json`
pointing at a copied `~/.clawd/clawd-push.*`. If that entry is still there, it is
dead weight — remove the `statusLine` key (or restore whatever it replaced, if
they remember) and mention it, since it never worked in the desktop app and does
nothing now.
