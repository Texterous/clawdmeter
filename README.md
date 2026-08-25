# Clawdmeter

Firmware that turns a €13 GeekMagic **SmallTV-Ultra** into a desk display for your
live Claude Code sessions, with a web UI in Texterous' design language. Built to be
handed out at a hackathon: one screen, five settings pages, and a provisioning path
that scales to a boxful of units.

Plug it in, join its hotspot, pick your WiFi. Install the plugin, run
`/clawd:setup` and type the four characters on the glass. Your sessions appear.
That is the whole of it — no Python, no daemon, no account, no card in the box.

<!-- TODO: photo of a unit showing the meter, once the batch arrives -->

```
Status · Wifi · Display · Clawdmeter · System
```

## What it does

A Claude Code plugin on your machine works out what each of your sessions is doing
and POSTs it to the device — [`plugin/`](plugin/) in this repository:

```bash
claude plugin marketplace add Texterous/clawdmeter
claude plugin install clawd@clawdmeter
```

Then `/clawd:setup` and the four characters on the device screen. No Python, no
credential, no address to look up. There is no Clawdmeter agent binary; see
[agent/](agent/) for why, and for why the Python daemon is no longer the
recommended sender. There is one screen:

**The session board** — one row per live Claude session on your machine: a state
dot, the session name, and how long it has been that way. Green is working, amber
is waiting for you, red is stuck on a permission prompt, and the footer tallies
them in the same three colours. Six rows fit; the header reports the true count
when there are more.

Under it, **your 5-hour and 7-day windows**, as two bars — but only when the sender
actually has them. Those figures reach a `statusLine` and nothing else, and a
`statusLine` never runs in the Claude Code desktop app, so most senders have
nothing to give: the footer then shows two rows of nothing instead of two
confident zeroes.

They used to be a screen of their own, with an animated mascot. Selecting it from
the desktop app gave you a dead panel, which is not a setting worth having — and
folding the windows into the footer returned 37 KB of flash, most of it the mascot
sprite sheet. `tools/gen_mascot.py` still builds a frame header from any sprite
sheet if a creature is ever wanted back.

That is the whole feature set, on purpose.

The device never talks to Anthropic itself and holds no credentials. The session
board is assembled entirely on your machine — only a name, a state letter and a
minute count ever reach the device.

## Hardware

| | |
|---|---|
| Board | GeekMagic SmallTV-**Ultra** — ESP-12F (ESP8266), 4 MB flash |
| Display | 1.54" 240×240 ST7789 IPS |
| Power | USB-C, **power only — there is no USB-serial chip** |
| Sourcing | [AliExpress](https://nl.aliexpress.com/item/1005006728045097.html), ~€13–27 |

That third row governs everything about installation: the unit never appears as a
COM port, so every flash is over WiFi. UART means opening the case and soldering,
and there is no download-mode rescue if a flash goes wrong. Read
[docs/flashing.md](docs/flashing.md) before your first install.

Other SmallTV variants are not supported. This is one firmware for one board.

## The images, and why

```
pio run -e ultra_slim      # 481,440 B · no TLS · installs on a factory-fresh unit
pio run -e ultra           # 597,168 B · adds HTTPS + GitHub self-update
pio run -e loader          # 315,920 B · two-step install fallback
pio run -e ultra_giveaway  # 481,440 B · ultra_slim with provisioning compiled out
```

The stock Ultra firmware reserves most of the flash for image storage, so its OTA
updater rejects anything much over 512 KiB. `ultra_slim` drops the TLS stack
entirely — usage is *pushed* to the device, so nothing outbound needs TLS — which
saves 115 KB and is what makes a one-step install possible at all. It is fully
functional: a unit can be handed out having only ever seen the slim image.

`ultra` adds HTTPS so recipients get fixes without touching a laptop. It is
installed *over* the slim image through the device's own `/update`, which is not
subject to stock's limit.

`ultra_giveaway` is `ultra_slim` with `-D NO_PROVISION`, and it exists because a
batch handed to strangers must **not** carry a network credential. A baked SSID is
a convenience for units you keep and a defect for units you give away: thirty of
them all try to join a network that is not at the venue, and none of them opens the
setup hotspot its recipient needs. Making that a build target rather than
"remember to move a gitignored file first" is the point. See
[provision/](provision/).

Every call site that could pull BearSSL into the link sits behind `WITH_TLS` in
[`firmware/src/config.h`](firmware/src/config.h). One unguarded reference — even
behind a runtime `if` — drags the whole stack back in and costs the slim build the
115 KB it exists to save. Treat that macro as load-bearing.

## Build

```bash
cd firmware
pio run -e ultra_slim
```

`webui.h` is generated from `web/webui.html` before every build by
`tools/pre_build.py`, so a stale UI cannot ship. The generator is deterministic:
an unchanged source produces a byte-identical header.

To bake WiFi credentials into a batch image, write `firmware/src/provision_local.h`
(gitignored — see [provision/](provision/)). A freshly flashed unit then rejoins
that network by itself. Do this only for units you are keeping; build
`ultra_giveaway` for the ones you are not.

## Install

```bash
curl -F "firmware=@firmware.bin" http://<device>/update
```

or use the System tab.

A unit with no saved network says so on its own screen — `SET ME UP`, the name of
the hotspot it is broadcasting, and the address to open once you have joined it. So
there is nothing to look up and nothing to keep: join that hotspot, pick your own
network in the portal, and the device puts the next address on its panel and leaves
it there until something has actually pushed to it.

## Your device, your firmware

`/update` accepts **any** ESP8266 image that fits: a Clawdmeter build, upstream
`smalltv-mod`, ESPHome, Tasmota, or GeekMagic's stock build. No signing, no
allowlist. Two guards run before anything is written — the image has to fit, and it
has to actually be an ESP8266 image — because a wrong file on this board means a
soldering iron, not `esptool`. Both explain themselves in the response.

See [docs/flash-something-else.md](docs/flash-something-else.md).

## HTTP API

| Route | |
|---|---|
| `GET /` | the web UI (gzipped, from PROGMEM) |
| `GET /api/status` | connection, device health, and meter freshness |
| `GET`/`POST /api/config` | settings; **POST is a partial merge** |
| `GET /api/scan` | WiFi scan |
| `POST /api/usage` | the push endpoint (unauthenticated by design) |
| `GET`/`POST /api/export`,`/api/import` | settings backup; also the provisioning hook |
| `POST /api/reboot`, `/api/factory`, `/api/refresh` | |
| `POST /update` | firmware upload |

Push contract:

```bash
curl -X POST http://<device>/api/usage \
  -d '{"s":42,"sr":118,"w":63,"wr":4200,"st":"allowed","ok":true,
       "sess":[{"n":"stoplicht-72","s":"w","t":14}],"ns":1}'
```

`s`/`w` are the 5-hour and 7-day percentages, `sr`/`wr` the minutes until each
resets, `st` a status string. These six keys are byte-compatible with
[`giovi321/clawdmeter-daemon`](https://github.com/giovi321/clawdmeter-daemon), so
that daemon still drives this firmware unchanged — but note that it authenticates
by presenting itself to the API as Claude Code, which is why [agent/](agent/) no
longer recommends it. If you do run it, `--push-to <device>` is not enough:
`/api/usage` is unauthenticated by design and the firmware advertises
`_clawdmeter._tcp`, so discovery left on writes to **every** unit it can see. Add
`--no-discover`. The plugin has no discovery to disable.

`sess` and `ns` feed the session board and are optional: each row is `n` (name,
≤12 chars), `s` (`w`orking / `b`locked / `a`waiting) and `t` (minutes in that
state), with `ns` the live session count when it exceeds the six rows sent. A
payload without them still drives the meters — the board then reads "UPDATE YOUR
SENDER", which is a different thing from "nothing is running".

## Repository layout

```
firmware/     PlatformIO project — four envs, one board
  web/        webui.html, the source of truth for the web UI
  tools/      code generators, run automatically before each build
provision/    batch flashing and verification — giveaway and staffed runs
plugin/       the clawd Claude Code plugin — the sender people install
agent/        why there is no agent binary, and the /api/usage contract
docs/         flashing, recovery, and the event runbook
```

## Credits

Derived from **[giovi321/smalltv-mod](https://github.com/giovi321/smalltv-mod)**
(WTFPL) — the display core, WiFi/AP handling, settings persistence, the boot-time
OTA path that lets an image exceed the free-flash limit, and the usage meter all
began there. Pin maps trace back to the ESPHome and Tasmota communities.

The mascot frames from [ClaudePix](https://claudepix.vercel.app) are **no longer in
this firmware** — they went with the screen they lived on, which also settles the
licensing question [docs/mascot-licence.md](docs/mascot-licence.md) was open on.

Not affiliated with GeekMagic or Anthropic. MIT — see [LICENSE](LICENSE).
