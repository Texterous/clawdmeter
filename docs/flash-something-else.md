# Flashing something else

This is your device. Nothing in this firmware tries to keep you on it — `/update`
takes any ESP8266 image that fits, unsigned, with no allowlist. This page is the
honest version of how to leave, including the parts that can go wrong.

## The one thing to understand first

The SmallTV-Ultra's USB-C port is **power only**. There is no USB-serial chip, so
the board never appears as a COM port and `esptool` has nothing to talk to. That
means:

- Every install is over WiFi, through a web `/update` endpoint.
- **There is no download-mode rescue.** On an ESP32 board a bad flash is always
  recoverable over USB. Here it is not. A truly broken image means opening the case
  and soldering a 3.3 V UART adapter to the pads (GPIO0 to GND on power-up).

That is why `/update` runs two checks before writing anything:

1. **Size.** The declared `Content-Length` must fit the free sketch space.
2. **Shape.** The first byte must be `0xE9` or `0xEA`, the ESP8266 flash header.
   An ESP32 image, a `.zip`, or an accidentally selected `config.json` is refused
   here, before a single byte reaches flash.

Both refusals come back as plain text explaining what happened. Neither touches the
running firmware: Arduino OTA writes to the spare half of flash and only commits on
the next boot, so a rejected or interrupted upload leaves you exactly where you were.

## The free-space ceiling

ESP8266 OTA caps an uploaded image at the free flash *above* the running sketch,
not at the partition size. With the ~508 KB slim image running you have roughly
530 KB of headroom — so **upstream's 705 KB `smalltv-mod-firmware.bin` cannot be
uploaded directly.** You will get the size refusal.

The way through is to step down via the loader, which is small enough to fit under
any of these ceilings:

```
current firmware  --/update-->  loader (317 KB)  --/update-->  anything up to ~1 MB
```

The loader is a WiFi client plus an OTA page and nothing else, so once it is running
almost the whole sketch region is free.

Build one with your network baked in so it never needs an access point (see
[`provision/`](../provision/) for the `provision_local.h` mechanism):

```bash
cd firmware
pio run -e loader
# .pio/build/loader/firmware.bin
```

Without baked credentials the loader opens an open AP called `SmallTV-Loader` at
`192.168.4.1`. That works, but joining it costs your machine its internet
connection for the duration, which is why baking credentials is worth the extra
minute.

## Going back to GeekMagic stock

Two things to know before you start.

**You cannot back up your unit's own factory image.** That needs UART. So "back to
stock" means back to the newest build GeekMagic publishes, not the one your unit
shipped with:

- [`GeekMagicClock/smalltv-ultra`](https://github.com/GeekMagicClock/smalltv-ultra)
  → `Ultra-V9.0.31/FW-Smalltv-Ultra-V9.0.31.bin` (509,792 B)

**It is a full-flash image, not an OTA image.** The first bytes are the ESP8266
bootloader, meant to be written at offset `0x0`. An OTA endpoint writes to the app
region instead, so uploading it through `/update` is not guaranteed to produce a
booting device. If it does not come up, the recovery is UART.

If you want stock back with certainty, do it over UART and write the file at `0x0`:

```bash
esptool --chip esp8266 --port <PORT> write_flash 0x0 FW-Smalltv-Ultra-V9.0.31.bin
```

## Other firmware

- **[upstream smalltv-mod](https://github.com/giovi321/smalltv-mod)** — a superset
  of this one: stock ticker, plane radar, Home Assistant screens. Its ESP8266 image
  is 705 KB, so go via the loader.
- **[ESPHome](https://devices.esphome.io/devices/geekmagic-ultra/)** — has a
  documented config for this board.
- **Tasmota** — works; you will need to configure the ST7789 pins yourself. They
  are in [`firmware/src/Board.h`](../firmware/src/Board.h).

## If it will not boot

Symptoms and what they mean:

| What you see | What it is |
|---|---|
| Backlight on, screen black | The sketch started but the panel init failed. Usually a colour-order or rotation setting; try a factory reset. |
| Backlight never comes on | The sketch is not reaching `gfxBegin()`. Bad image or a crash before the display. |
| Screen shows "Crashed" plus an address | Safe mode. The web server is up and `/update` works — upload a known-good image. This is the designed recovery path and does not need a soldering iron. |
| No WiFi, no AP, nothing | The image is not running at all. UART. |

Safe mode is worth stressing: after an exception the firmware deliberately skips
the render path and brings up only WiFi and the web server, so a crash caused by a
bad setting or a bad build is still recoverable over the air.

## UART, as a last resort

Open the case, find the pads, wire a 3.3 V adapter — **not 5 V** — to TX, RX and
GND, pull GPIO0 to GND while applying power to enter download mode, then use
`esptool` normally. The pin map is in
[`firmware/src/Board.h`](../firmware/src/Board.h). Upstream's
[flashing guide](https://giovi321.github.io/smalltv-mod/getting-started/flashing/)
covers the physical detail.
