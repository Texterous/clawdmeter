# Provisioning a batch

The manual step is unavoidable and it is exactly one per unit: a factory-fresh
SmallTV has no entry point except stock's own access point, so somebody has to join
it and point the unit at the venue WiFi. About a minute. Everything after that is
scripted over the LAN with no further network changes, which is the part that
matters — each network change costs the flashing machine its internet connection.

## Once per batch

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

## Per unit

1. Power it, join its `SmallTV-xxxx` access point, point it at the venue WiFi.
2. Note the address it gets, then:

```powershell
.\flash.ps1 -Ip 192.168.1.57 -Unit 07
```

That uploads the slim image, waits for the unit to rejoin on its own, imports unit
07's settings from `batch.json`, installs the full image on top, and verifies.

Add `-SkipFull` to leave a unit on the slim build.

## Before the event

```powershell
.\verify.ps1 -Units 1..30
```

One table: version, address, signal, heap, uptime, and meter freshness per unit.
Units that do not answer are listed as DOWN rather than omitted — a tidy table with
rows quietly missing reads as "all fine" when it is not.

"up but never pushed to" is the expected state for every unit until a recipient
installs the agent. It is not a fault.

## Sticker per unit

Each unit should ship with its own hostname and a QR to the docs:

```
clawdmeter-07.local
```

That name is the durable handle. DHCP moves addresses; the mDNS name does not, and
it is what the agent discovers by.

## Budget

Roughly 2–3 minutes hands-on per unit. Run trays of eight on a powered USB hub and
pipeline the AP joins against the scripted parts. Order 2–3 spares beyond headcount:
this board has no USB recovery mode, so a unit that goes wrong is a soldering job,
not a retry.
