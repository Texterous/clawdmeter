# Flashing

## Before anything else

The SmallTV-Ultra's USB-C port is **power only**. There is no USB-serial chip. The
unit never appears as a COM port, so `esptool` over USB is not an option and every
install goes over WiFi. There is also no download-mode rescue: a genuinely broken
image means opening the case and soldering. See
[flash-something-else.md](flash-something-else.md) for the recovery detail.

## A factory-fresh unit

A new unit runs GeekMagic's stock firmware, which exposes its own OTA page.

1. Power it up. It opens an access point (`SmallTV-xxxx`). Join it.
2. Point it at your WiFi through the stock setup page.
3. Find its address on your network, then upload the slim image:

   ```bash
   curl -F "firmware=@clawdmeter-ultra-slim.bin" http://<device-ip>/update
   ```

4. It reboots into Clawdmeter. What happens next depends on the image:
   - built with baked credentials, it rejoins the same network by itself and you
     can open `http://clawd-xxxx.local`;
   - built without them (the giveaway image), it opens **its own** setup hotspot
     and the panel says `SET ME UP` with the hotspot name to join. That is the
     intended end state for a unit you are giving away — see
     [`provision/`](../provision/).

**Use the slim image for this step.** Stock reserves most of the flash for image
storage, so its updater will not take the full build.

## The two size limits, which are not the same number

Conflating these is what has made the headroom look bigger than it is. There are
two independent ceilings and the lower one binds:

| | bytes | what it is |
|---|--:|---|
| CI gate | **520,000** | `.github/workflows/build.yml` stats `firmware.bin` and **fails the build** above this. Self-imposed, and it is the one you hit first. |
| Stock OTA ceiling | **524,288** | 512 KiB. GeekMagic's stock updater refuses an upload much over this, so an image above it cannot be installed in one step on a factory-fresh unit at all. |

Measured sizes, firmware 0.4.0, all four `pio run` targets, built from a clean
checkout (no `provision_local.h`, which is what CI does):

| env | image | bytes | headroom to CI gate | headroom to stock ceiling |
|---|---|--:|--:|--:|
| `ultra_slim` | `clawdmeter-ultra-slim.bin` | **481,440** | 38,560 | 42,848 |
| `ultra_giveaway` | `clawdmeter-ultra-giveaway.bin` | **481,440** | 38,560 | 42,848 |
| `ultra` | `clawdmeter-ultra.bin` | 597,168 | — | over both |
| `loader` | `rollback-loader.bin` | 315,920 | — | — |

0.4.0 dropped the separate usage screen and the mascot sprite sheet that only it
drew: **−37,296 B**, which took the slim headroom from 1,264 B to 38,560 B. Before
spending any of that, note what it bought — a size gate this project was one screen
away from failing.

With `provision_local.h` present, `ultra_slim` is 48 B larger — the two baked
credential literals. `ultra_giveaway` never varies, because `-D NO_PROVISION` keeps
the header out whether or not it exists.

`ultra_giveaway` is the image that goes to recipients, and it is always the smaller
of the two gated builds — it is `ultra_slim` with the baked-credential string
literals compiled out. So CI gating `ultra_slim` is a valid conservative bound for
both, which is why the gate did not need a second threshold when that env was
added.

The 4,288 B between the CI gate and the stock ceiling is deliberate slack — leave
it unspent. Being installable in one web upload, with no tooling on the recipient's
side, *is* the distribution mechanism; raising the gate spends the only thing that
makes that true.

**`dist/` holds these images** as of 0.3.1, rebuilt from a clean checkout and
verified to contain no SSID or password string. It is worth re-checking rather than
assuming: for most of this project's life `dist/` held a 0.2.x build that predated
the onboarding work — no commissioning screen, the old `Clawdmeter-Setup` AP name,
the dead `/agent/install.*` routes, an `/api/export` that handed WiFi passwords to
any unauthenticated caller, **and the home SSID and password baked in as plain
strings**, because it had been built with `provision_local.h` in place. `dist/` is
gitignored so none of that was ever published, but flashing a batch from it would
have shipped one household's WiFi password to thirty strangers.

So: rebuild it, with the provisioning header moved out of the tree, and grep the
result before trusting it.

```bash
cd firmware
mv src/provision_local.h /tmp/ 2>/dev/null   # only ultra_giveaway is safe without this
pio run -e ultra_slim -e ultra -e ultra_giveaway -e loader
mv /tmp/provision_local.h src/ 2>/dev/null
mkdir -p ../dist
cp .pio/build/ultra_slim/firmware.bin      ../dist/clawdmeter-ultra-slim.bin
cp .pio/build/ultra/firmware.bin           ../dist/clawdmeter-ultra.bin
cp .pio/build/ultra_giveaway/firmware.bin  ../dist/clawdmeter-ultra-giveaway.bin
cp .pio/build/loader/firmware.bin          ../dist/rollback-loader.bin
for f in ../dist/*.bin; do echo "$f $(strings "$f" | grep -c 'De Boomhut')"; done   # all zero
```

`provision/flash.ps1` refuses to upload an image whose `FW_VERSION` string does not
match `firmware/src/config.h`, so a forgotten refresh now stops the run instead of
silently shipping the old firmware. Never overwrite the slim image with a giveaway
build or the other way round — they differ only in a string nothing on the device
prints, and the giveaway guard is what tells them apart.

## Fallback: the two-step loader install

If step 3 is refused for lack of space:

```
stock  --/update-->  loader (317 KB)  --/update-->  clawdmeter (any size)
```

Build the loader with your network baked in so it joins WiFi instead of opening an
access point — otherwise you lose your machine's internet for each unit, twice:

```bash
cd firmware
pio run -e loader
curl -F "firmware=@.pio/build/loader/firmware.bin" http://<device-ip>/update
# wait for it to rejoin, then
curl -F "firmware=@clawdmeter-ultra-slim.bin" http://<loader-ip>/update
```

See [`provision/`](../provision/) for the credential mechanism.

## Upgrading an existing unit

Straight through the System tab, or:

```bash
curl -F "firmware=@clawdmeter-ultra.bin" http://<device>/update
```

Neither ceiling above applies once Clawdmeter is running — they are stock's and
CI's, not the board's. What applies instead is free flash *above* the running
sketch: with the 481,440 B slim image up, `ESP.getFreeSketchSpace()` is 0x300000
(the LittleFS start at `_FS_start = 0x40500000`) minus the sketch rounded up to a
sector, so `/update` accepts about **2.5 MB**. The linker script caps any image
this project can build at `irom0_0_seg` = 1,044,464 B, well under that, so **every
slim → full upgrade fits and the loader step-down is never needed here.** It exists
only for step 3, where stock's updater is the one refusing.

The full image can also update itself from GitHub releases (System tab). The slim
image cannot — it has no TLS — so it hides that button rather than offering one
that can only fail.

## A batch

See [`provision/`](../provision/). Short version: one access-point join per unit to
get it onto the bench network, then the upload is scripted over the LAN. A batch
being given away is flashed with provisioning compiled **out**, so each unit ends
up on its own setup hotspot rather than rejoining yours — `flash.ps1 -Giveaway`
refuses an image that would do otherwise.
