# Provisioning a batch

There are two kinds of batch and they want opposite things from the firmware.
Pick one before you build anything.

| | **Giveaway** | **Staffed** |
|---|---|---|
| Who ends up holding it | strangers, on networks you will never see | you, on a network you control |
| Venue WiFi baked in | **no** — there is none to bake | yes |
| After flashing, the unit | opens its own setup hotspot | rejoins your network by itself |
| Named `clawd-01…30` | no, each keeps its chip suffix | yes, `-Unit` |
| `batch.json` settings applied | no (nothing can reach it) | yes |
| Reachable by `verify.ps1` | no | yes |
| Command | `.\flash.ps1 -Giveaway -Ip <ip>` | `.\flash.ps1 -Ip <ip> -Unit 07` |

The manual step is the same either way and it is exactly one per unit: a
factory-fresh SmallTV has no entry point except stock's own access point, so
somebody has to join it and point the unit at the bench WiFi. About a minute.
Everything after that is scripted, which is the part that matters — each network
change costs the flashing machine its internet connection.

---

## Giveaway batch

The whole design follows from one fact: **there is no venue network to pre-seed,
and no printed card in the box.** Every unit must come up on its own hotspot and
tell its recipient what to do, from the panel and from its own web UI.

### Build with provisioning compiled out

```powershell
cd ../firmware
pio run -e ultra_giveaway
mkdir ../dist -Force
cp .pio/build/ultra_giveaway/firmware.bin ../dist/clawdmeter-ultra-giveaway.bin
```

`ultra_giveaway` is the slim build with `-D NO_PROVISION`, which suppresses the
`provision_local.h` include. Building the env is the safeguard: a batch cannot be
built wrong by forgetting to move a gitignored file out of the way first.

If a credential does get baked in, thirty units all try to join a network that is
not at the venue, none of them falls back to anything a recipient can use, and it
is not fixable once they are in people's hands. So it is checked twice:

- **Before the upload.** `flash.ps1 -Giveaway` reads the SSID out of
  `provision_local.h` and searches the image bytes for it. `HAS_PROVISION` puts
  that SSID into the image as a string literal, so if it is in there, the image is
  provisioned. It refuses rather than warns.
- **After the upload.** A giveaway unit must go *silent* on the LAN — it has no
  saved network, so it opens its own hotspot and is no longer on yours. If it
  answers `/api/status` again, the image had a network baked in after all and the
  script fails loudly, naming what it joined.

### Flash

```powershell
.\flash.ps1 -Giveaway -Ip 192.168.1.57
```

That is the whole per-unit job: one upload, then read the panel. There is nothing
to import — the unit is on its own hotspot and cannot be reached over the LAN — so
giveaway units ship on firmware defaults. Anything in `batch.json` is for a staffed
batch only.

### Read the panel before you box it

```
SET ME UP
Join this WiFi:
Clawd-a1b2
```

Note those four hex characters. They come from the chip id, they are stable, and
they are the unit's identity — the SSID it broadcasts, and (lowercased, as
`clawd-a1b2.local`) its hostname. **This is the one check nothing downstream can
do for you.** Four hex characters give a ~0.66% chance of one colliding pair
somewhere in a batch of thirty, and a duplicate is silent in every other way; the
flashing bench, with each unit showing its own name, is the only place two the same
are visible. If you see a repeat, give one of the pair a different `apSsid` and
`hostname` through its own web UI before it goes out.

### There is no sticker

Deliberately. The device is its own label: the panel shows the hotspot name at text
size 3 (18×24 px glyphs — 10 characters at 6×3 px per column is 180 px of the 232 px
usable width, legible across a table), and the setup portal's first card names the
unit you actually reached so a recipient can confirm it before typing their home
WiFi password into it. A sticker would be a second source of truth that cannot be
corrected after it is printed, for a name the device already displays.

That confirm step matters more than it sounds. Setting up the wrong unit out of
thirty identical hotspots writes your home WiFi password into a stranger's device.

---

## Staffed batch

### Once per batch

Write the credential header the images bake in:

```powershell
.\flash.ps1 -Credentials -Ssid "HackathonWiFi" -Password "..."
```

That produces `firmware/src/provision_local.h`, which is **gitignored**. A header
rather than `-D` flags for two reasons: an SSID containing a space cannot survive
PlatformIO's flag splitting, and a credential in `platformio.ini` ends up committed.

Then build:

```powershell
cd ../firmware
pio run -e ultra_slim -e ultra -e loader
mkdir ../dist -Force
cp .pio/build/ultra_slim/firmware.bin ../dist/clawdmeter-ultra-slim.bin
cp .pio/build/ultra/firmware.bin      ../dist/clawdmeter-ultra.bin
cp .pio/build/loader/firmware.bin     ../dist/rollback-loader.bin
```

Keep `rollback-loader.bin`. It is the step-down path for any image too large to
upload directly, and having it built *before* you need it is the difference between
a two-minute recovery and an evening.

### Per unit

1. Power it, join its `SmallTV-xxxx` access point, point it at the bench WiFi.
2. Note the address it gets, then:

```powershell
.\flash.ps1 -Ip 192.168.1.57 -Unit 07
```

That uploads the slim image, waits for the unit to rejoin on its own, imports unit
07's settings from `batch.json`, installs the full image on top, and verifies.
Add `-SkipFull` to leave a unit on the slim build.

**`-Unit` is mandatory.** It used to default to `01`, so a run that forgot the flag
named every unit in the batch the same thing — thirty units answering to one mDNS
name, with each recipient's pushes landing on whichever one won the probe race.

The import now sets **`apSsid` as well as `hostname`**, so unit 07 broadcasts
`Clawd-07` and answers to `clawd-07.local`. Setting only the hostname left the unit
answering to one name while advertising the chip-suffix hotspot it was born with,
and the name a person has to match is the one on the air. `/api/import` already
accepted `apSsid`; this was a host-side omission, not a firmware gap.

`-Unit` is capped at six characters, and the reason is pixel width rather than
taste. The panel renders the SSID at up to text size 3 against a 232 px content
budget, at 6 px per character per size step: `Clawd-` + 6 characters = 12 × 6 × 3 =
216 px and fits; thirteen characters would be 234 px and would silently drop the one
string a recipient has to match down to size 2.

### Before the event

```powershell
.\verify.ps1 -Units 1..30
.\verify.ps1 -Hosts 192.168.1.57,192.168.1.58   # by address
```

One table: version, address, signal, heap, uptime, and meter freshness per unit,
plus a duplicate-name audit.

Two things it reports that it did not used to:

- **`UNREACH`, not `DOWN`.** `startMdns()` runs on station-mode success only, so a
  unit on its own setup hotspot has no `.local` name *and* is on a different
  network — it cannot answer here by either handle. "Dead" and "waiting on its own
  hotspot" are indistinguishable from this side, and calling both `DOWN` sent
  people hunting a fault that was not there. (It follows that a giveaway batch is
  not verifiable with this script at all. Check those at the bench, off the panel.)
- **Duplicate hostnames.** Otherwise completely silent: `MDNS.begin()` is called
  with no host-probe callback, so the loser of a probe keeps the name in its own
  settings, never announces it, and every push aimed at that name lands on the
  winner. The recipient of the loser gets a blank screen forever. Three signals are
  checked — a unit answering to a name that is not its own, one address answering
  for several names, and one hostname claimed from several addresses. The last is
  only visible if you reach both by address, which is why `-Hosts` now takes
  addresses as well as names.

"Up but never pushed to" is the expected state for every unit until someone runs
the sender at it. It is not a fault.

## Budget

Roughly 2–3 minutes hands-on per unit for a staffed batch; a giveaway unit is
faster because the upload is the last step. Run trays of eight on a powered USB hub
and pipeline the AP joins against the scripted parts. Order 2–3 spares beyond
headcount: this board has no USB recovery mode, so a unit that goes wrong is a
soldering job, not a retry.
