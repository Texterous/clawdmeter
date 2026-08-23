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

4. It reboots into Clawdmeter and — if the image was built with baked credentials —
   rejoins the same network by itself. Open `http://clawdmeter-XXXX.local`.

**Use the slim image for this step.** Stock reserves most of the flash for image
storage, so its updater rejects anything much over 512 KiB (524,288 B). The slim
build is 516,976 B — under it, but only by ~7 KB, so watch this number when the
image grows. The full build is 632,464 B and will not fit.

> The exact ceiling has not been measured on hardware yet. If step 3 is refused
> for lack of space, fall back to the loader below — it is 317 KB and upstream
> confirms that size passes.

## Fallback: the two-step loader install

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

Note the free-space ceiling: OTA caps an upload at the free flash *above* the
running sketch, not at the partition size. Slim → full is fine. Anything over
~530 KB while the slim image is running is not, and needs the loader step-down.

The full image can also update itself from GitHub releases (System tab). The slim
image cannot — it has no TLS — so it hides that button rather than offering one
that can only fail.

## A batch

See [`provision/`](../provision/). Short version: one access-point join per unit to
get it on the venue network, then everything after that is scripted over the LAN
with no further network changes.
