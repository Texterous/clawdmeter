# Clawdmeter

Firmware that turns a €13 GeekMagic **SmallTV-Ultra** into a desk display for your
Claude Code usage, with a web UI in Texterous' design language. Built to be handed
out at a hackathon: one screen, five settings pages, and a provisioning path that
scales to a boxful of units.

<!-- TODO: photo of a unit showing the meter, once the batch arrives -->

```
Status · Wifi · Display · Clawdmeter · System
```

## What it does

A small agent on your machine reads your Claude Code rate-limit state and POSTs it
to the device. There are two screens, picked in the web UI:

- **Usage meters** — the 5-hour and 7-day windows, the countdown to each reset, and
  an animated mascot whose mood tracks your burn rate.
- **Session board** — one row per live Claude session on your machine: a state dot,
  the session name, and how long it has been that way. Green is working, amber is
  waiting for you, red is stuck on a permission prompt. Six rows fit; the header
  reports the true count when there are more.

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

## Two images, and why

```
pio run -e ultra_slim     # ~508 KB · no TLS · installs on a factory-fresh unit
pio run -e ultra          # ~623 KB · adds HTTPS + GitHub self-update
pio run -e loader         # ~317 KB · two-step install fallback
```

The stock Ultra firmware reserves most of the flash for image storage, so its OTA
updater rejects anything much over ~512 KB. `ultra_slim` drops the TLS stack
entirely — usage is *pushed* to the device, so nothing outbound needs TLS — which
saves 115 KB and is what makes a one-step install possible at all. It is fully
functional: a unit can be handed out having only ever seen the slim image.

`ultra` adds HTTPS so recipients get fixes without touching a laptop. It is
installed *over* the slim image through the device's own `/update`, which is not
subject to stock's limit.

Every call site that could pull BearSSL into the link sits behind `WITH_TLS` in
[`firmware/src/config.h`](firmware/src/config.h). One unguarded reference — even
behind a runtime `if` — drags the whole stack back in and costs the slim build the
115 KB it exists to save. Treat that macro as load-bearing.

## Build

```bash
cd firmware
pio run -e ultra_slim
```

`webui.h` and `agent_install.h` are generated from `web/` before every build by
`tools/pre_build.py`, so a stale UI cannot ship. Both generators are deterministic:
unchanged sources produce byte-identical headers.

To bake WiFi credentials into a batch image, write `firmware/src/provision_local.h`
(gitignored — see [provision/](provision/)). A freshly flashed unit then rejoins the
venue network by itself and nobody touches a setup screen on event day.

## Install

```bash
curl -F "firmware=@firmware.bin" http://<device>/update
```

or use the System tab. Then open `http://<hostname>.local` and set up WiFi.

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
| `GET /agent/install.ps1`, `/agent/install.sh` | agent bootstrap, served offline |
| `POST /update` | firmware upload |

Push contract:

```bash
curl -X POST http://<device>/api/usage \
  -d '{"s":42,"sr":118,"w":63,"wr":4200,"st":"allowed","ok":true,
       "sess":[{"n":"stoplicht-72","s":"w","t":14}],"ns":1}'
```

`s`/`w` are the 5-hour and 7-day percentages, `sr`/`wr` the minutes until each
resets, `st` a status string. The original six keys are byte-compatible with
[`giovi321/clawdmeter-daemon`](https://github.com/giovi321/clawdmeter-daemon), so
that daemon drives this firmware unchanged.

`sess` and `ns` feed the session board and are optional: each row is `n` (name,
≤12 chars), `s` (`w`orking / `b`locked / `a`waiting) and `t` (minutes in that
state), with `ns` the live session count when it exceeds the six rows sent. A
payload without them still drives the meters — the board just says the daemon is
too old, which is a different thing from "nothing is running".

## Repository layout

```
firmware/     PlatformIO project — three envs, one board
  web/        webui.html + the agent bootstrap scripts (sources of truth)
  tools/      code generators, run automatically before each build
provision/    batch flashing and verification for a giveaway run
agent/        the one-command usage agent
docs/         flashing, recovery, and the event runbook
```

## Credits

Derived from **[giovi321/smalltv-mod](https://github.com/giovi321/smalltv-mod)**
(WTFPL) — the display core, WiFi/AP handling, settings persistence, the boot-time
OTA path that lets an image exceed the free-flash limit, and the usage meter all
began there. Pin maps trace back to the ESPHome and Tasmota communities. Mascot
frames come from [ClaudePix](https://claudepix.vercel.app); see
[docs/mascot-licence.md](docs/mascot-licence.md).

Not affiliated with GeekMagic or Anthropic. MIT — see [LICENSE](LICENSE).
