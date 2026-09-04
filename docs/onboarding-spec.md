# Onboarding spec: from plugged-in to working

**Status: mostly implemented as of 0.5.0 (2026-09-04).** Written 2026-08-23
against firmware 0.2.0; the walk below is how it stands now.

Every unit ships with no saved network, so every recipient walks the same path:
setup hotspot, then their own WiFi, then the sender.

```
1 plug in            SETUP MODE screen, persistent            works
2 join hotspot       captive portal pops                      works
3 portal opens       Clawdmeter tab carries the pair command  works (0.3.0)
4 WiFi tab           scan, tap, type, save                    works
5 save + reboot      toast, then the hotspot vanishes         SURVIVABLE (see below)
6 device rejoins     ALMOST DONE screen, does not expire      works (0.3.0)
7 install sender     /clawd:setup, one command, no restart    works (0.5.0)
```

**What closed stage 6, and why 5 stopped mattering.** The commissioning screen
now holds the whole remaining step and never expires: `ALMOST DONE`, then
`/clawd:setup a1b2` as the literal thing to type, then the device's own
`<host>.local` and IP. So the phone losing the portal at the reboot is no longer
a cliff — everything it would have told the recipient is on the glass, and stays
there until the first payload lands.

**Stage 5 is still worth improving, and is now the only stage that is.** Keeping
the AP up while the station associates (ESP8266 supports `WIFI_AP_STA`) would let
the portal show a real success page with the new address and the pairing command,
instead of the connection dying under the user. That is polish rather than
capability: it costs a change in `Net.cpp`, `handlePostConfig` and the web UI, and
buys a smoother thirty seconds. It is the next thing to do here, not a blocker.

**Stage 7 was the blocked one and is now the shortest.** `/clawd:setup` writes one
config file and installs one background agent — no Claude Code restart, nothing in
`settings.json`, no binary to fetch from a release that does not exist. See
`plugin/README.md`.

Five changes close 3, 5 and 6. They are ordered by dependency: item 0 is
groundwork the rest assume.

---

## 0. Shorten the default names

Nothing else fits until this lands.

`Settings::setDefaults()` composes both names from a 4-hex chip suffix
([`Settings.cpp:166-169`](../firmware/src/Settings.cpp)), giving
`Clawdmeter-Setup-a111` (21 ch) and `clawdmeter-a111` (15 ch). The panel renders a
6x8 font at integer scale into 232 usable px, so per-character cost is 6 px at
size 1, 12 at size 2, 18 at size 3:

| string | ch | @1 | @2 | @3 |
|---|--:|--:|--:|--:|
| `Clawdmeter-Setup-a111` | 21 | 126 | 252 | 378 |
| `http://clawdmeter-a111.local` | 28 | 168 | — | — |
| `Clawdmeter-a111` *(proposed)* | 15 | 90 | 180 | 270 |
| `clawd-a111.local` *(proposed)* | 16 | 96 | 192 | 288 |

`gfxFitSize` returns the largest size that fits and **degrades silently to 1**, so
today the AP SSID and the post-setup URL both render at 6x8 on a 1.54" panel.

In [`config.h`](../firmware/src/config.h):

```c
#define DEFAULT_AP_SSID   "Clawdmeter"   // was "Clawdmeter-Setup"
#define DEFAULT_HOSTNAME  "clawd"        // was "clawdmeter"
```

Both then render at size 2 (16 px tall), the same size as the labels above them
and legible across a table. `.local` stays in the URL; `http://` comes off the
screen — browsers do not need it, and it costs 7 characters.

A name saved in `config.json` still overrides both, so already-configured units
keep their current names. This is also a win on the sticker.

**Also add a floor to the fit helper** so a long custom name is caught rather than
silently shrunk to unreadable — in [`Gfx.h`](../firmware/src/Gfx.h):

```c
// Largest size in [minSize, maxSize] that fits maxW, or 0 if even minSize does not.
uint8_t gfxFitSizeMin(const char* s, int maxW, uint8_t maxSize, uint8_t minSize);
```

Callers that get 0 truncate or drop the line instead of drawing 6 px text.

---

## 1. A commissioning screen that never expires

**The problem.** After the first successful boot the address is on screen for
`delay(3500)` ([`main.cpp:168`](../firmware/src/main.cpp)) and then the mascot
takes over forever. Those 3.5 s land exactly when the user is head-down in their
phone's WiFi settings getting back onto their home network.

**The idea.** The address only matters until the device has been fed once. So make
that an explicit device state rather than bolting a footer onto the mascot: while
the unit has never received usage, show a screen whose whole job is the address.
Once the first payload lands, today's behaviour resumes unchanged — and the mascot
goes back to meaning "your laptop is asleep", which is what it was designed for.

### State

A new persisted flag, `commissioned`, in [`Settings.h`](../firmware/src/Settings.h)
top-level:

```c
bool commissioned;   // a usage payload has been received at least once
```

- `setDefaults()`: `false`.
- `toJson`: emit it, so `/api/status` consumers and support can see it.
- `fromJson`: **ignore it.** `settingsApplyJson` is the shared path for
  `/api/config` and `/api/import`, and a provisioning import must not be able to
  mark a unit commissioned.

Persisted rather than RAM-only on purpose: a unit that has been on someone's desk
for a month should not show a setup screen every time it is unplugged and plugged
back in with the laptop shut. One flash write per device lifetime.

### Where it is set

[`UsageClient.cpp:86-89`](../firmware/src/meter/UsageClient.cpp) — `applyUsageDoc`
is the single commit point for both the pushed and the pulled payload, so hooking
it here covers both. Hooking `handleUsagePush` instead would leave a pull-mode
device stuck on the setup screen forever.

`applyUsageDoc` has no `Settings&`, so add a hook in the shape main.cpp already
uses for `appInvalidate()` / `appResetReason()`:

```c
// main.cpp
void appMarkCommissioned() {
  if (g_settings.commissioned) return;      // once, ever
  g_settings.commissioned = true;
  saveSettings(g_settings);
}
```

Called right after `d.lastOkMs = millis();`. The early return keeps it off the hot
path after the first payload.

### Where it renders

In `loop()` in [`main.cpp:186`](../firmware/src/main.cpp), as a third early-return
branch alongside the existing `g_safeMode` and `NET_AP` ones — this is a device
lifecycle state, not a meter concern, so both display modes stay untouched:

```c
if (!g_settings.commissioned) {
  gfxCommission(g_settings.hostname.c_str(), netIP().c_str());
  delay(5);
  return;
}
```

`gfxCommission` needs the idle mascot for its header, which `Gfx.cpp` does not
include. Put it in a new `meter/Commission.{h,cpp}` and have it repaint only on
first entry plus its own dot animation.

### Geometry (240x240, content x=8..232)

| y | content | size | colour |
|--:|---|--:|---|
| 4 | idle mascot at (6,4), scale 2 → 40x40 | — | palette |
| 12 | `CLAUDE` at x=56 (108 px) | 3 | `C_WHITE` |
| 52 | rule `fillRect(8,52,224,2)` | — | `C_BARBG` |
| 68 | `Open in a browser:` (216 px) | 2 | `C_DIM` |
| 94 | `<hostname>.local` (192 px) | 2 | `C_UGREEN` |
| 122 | `or` | 1 | `C_DIM` |
| 138 | `<ip>` (≤180 px) | 2 | `C_DIM` |
| 176 | rule `fillRect(8,176,224,2)` | — | `C_BARBG` |
| 196 | `Waiting for data` + 1–3 dots | 2 | `C_DIM` |

Bottom edge 212, leaving a 28 px margin. The header reuses the exact
mascot-plus-title construction from `drawUsage`
([`UsageMode.cpp:89-94`](../firmware/src/meter/UsageMode.cpp)) so the screen reads
as the same family.

Showing **both** the `.local` name and the IP is deliberate: mDNS is documented as
unreliable from at least one laptop on this project, and the IP is the fallback
that always works.

The dots animate so the screen does not look hung: repaint only
`fillRect(0,192,240,20,C_BLACK)` and redraw the line every 600 ms. `Waiting for
data...` is 19 ch = 228 px at size 2, which fits 232 with 4 px to spare — if the
wording changes, re-check that number.

### Two pieces of dead code this supersedes

- **Delete** the `!u.valid` branch at
  [`UsageMode.cpp:97`](../firmware/src/meter/UsageMode.cpp). It is unreachable:
  `drawUsage` is only called when `usageFresh()` is true, and `usageFresh()`
  already requires `valid`.
- **Wire up `showMascot`.** It is persisted with a UI checkbox and read by no
  renderer (only [`Settings.cpp:21,27,33`](../firmware/src/Settings.cpp)). Give it
  a real meaning: it controls the *idle* screen once commissioned — mascot when
  on, a plain address-and-status card when off. That serves people who want the
  numbers and not the animation, and removes a setting that currently lies.

---

## 2. Put the post-setup address on the setup screen

**The problem.** `gfxApInfo` prints the AP SSID and `192.168.4.1` only
([`Gfx.cpp:196`](../firmware/src/Gfx.cpp)), even though `gfxStaInfo` fourteen lines
below takes and prints the hostname. So the address the user needs *next* exists
only in the web UI they are about to lose access to.

`.local` deliberately is **not** offered as the address to open now. The captive
DNS answers `*` with 192.168.4.1, but Windows and Android route `.local` to mDNS
rather than DNS, so it would fail for some users. It is framed as "after setup".

Signature gains the hostname:

```c
void gfxApInfo(const char* ssid, const char* pass, const char* ip, const char* host);
```

The caller at [`main.cpp:160`](../firmware/src/main.cpp) already has
`g_settings.hostname` in hand.

### Geometry — open network (the default, `DEFAULT_AP_PASS ""`)

| y | content | size | colour |
|--:|---|--:|---|
| 8 | `SETUP MODE` (180 px) | 3 | `C_YELLOW` |
| 40 | `Join WiFi:` | 2 | `C_GRAY` |
| 60 | `<ssid>` (180 px) | fit 3 | `C_WHITE` |
| 86 | `(open network)` | 1 | `C_GRAY` |
| 106 | rule | — | `C_DGRAY` |
| 118 | `Then open:` | 2 | `C_GRAY` |
| 140 | `192.168.4.1` (198 px) | 3 | `C_GREEN` |
| 176 | rule | — | `C_DGRAY` |
| 188 | `After setup:` | 1 | `C_GRAY` |
| 204 | `<host>.local` (192 px) | fit 2 | `C_GRAY` |

Bottom edge 220. The IP moves up to size 3 — it is what people type first.

### Geometry — password set

Two extra lines, so everything below shifts by 14 px: `Password:` at 84 (size 1),
`<pass>` at 96 (fit 2), rule 120, `Then open:` 130, IP 150, rule 182,
`After setup:` 192, `<host>.local` 206. Bottom edge 222.

If a custom SSID or password makes that overflow, drop the `After setup:` block
first — anyone who set a hotspot password can find the UI again.

---

## 3. Stop the portal telling people to install the agent

**The problem.** `renderStatus` calls `renderMeter(s.meter)` at
[`webui.html:581`](../firmware/web/webui.html) *before* computing
`const ap = s.mode === 'ap'` on the next line. So the first card on the captive
portal reads:

> **Waiting for the agent.** Nothing has been pushed to this device yet. Open the
> Clawdmeter tab for the one-line installer.

In setup mode the next step is the WiFi tab. That command cannot work from the
hotspot — no uplink — and has no binary behind it yet either.

The vestige is visible: `renderStatus` computes
`const up = s.connected || s.mode === 'ap'` and throws it away with `void up;` at
[`webui.html:606`](../firmware/web/webui.html). The branch was planned, never
wired.

### Changes

1. Move the `ap` binding above the `renderMeter` call; pass it in as
   `renderMeter(s.meter, ap)`. Delete `up` and the `void up;` line.

2. In `renderMeter`, when `ap` is true, replace the agent message with the actual
   next step:

   > **Set up WiFi first.** This device is running its own hotspot, so it has no
   > internet yet. Open the **Wifi** tab, pick your network and save — the device
   > reboots and joins it. Then reconnect this phone or laptop to that same
   > network and come back to `http://<host>.local`.

   `<host>` from `s.host`, which `/api/status` already returns.

3. **Open on the WiFi tab in AP mode.** Extract the nav click body into
   `selectTab(name)` and call it once from `load()`:

   ```js
   let booted = false;
   // ...after renderStatus(s)
   if (!booted) { booted = true; if (s.mode === 'ap') selectTab('wifi'); }
   ```

   The `booted` guard must live in `load()`, not `renderStatus` — `poll()` calls
   `renderStatus` every 5 s and would otherwise yank the tab away mid-typing.

---

## 4. A real handoff instead of a toast

**The problem.** A WiFi change returns `reboot: true` and schedules a reboot in
800 ms ([`Web.cpp:186-194`](../firmware/src/Web.cpp)). The UI's entire response is
a 2.6 s toast, `Saved — rebooting to apply`
([`webui.html:716`](../firmware/web/webui.html)). Then the hotspot vanishes and the
user is holding a disconnected phone with no idea what to do next.

Replace it with a persistent card. Markup, at the top of the Status section:

```html
<div id="handoff" class="card" style="display:none">
  <h2 class="eyebrow">Almost done</h2>
  <p class="note">Saved. The device is rebooting and joining <b id="hoSsid"></b>.</p>
  <ol class="note">
    <li>Reconnect this phone or laptop to <b id="hoSsid2"></b>.</li>
    <li>Open <code id="hoUrl"></code></li>
  </ol>
  <small class="hint">The same address is on the device screen, with its IP
    underneath. Use the IP if <code>.local</code> does not resolve.</small>
</div>
```

In `save()` ([`webui.html:707`](../firmware/web/webui.html)):

```js
if (r.reboot) { showHandoff(patch); clearInterval(pollTimer); return; }
```

`showHandoff` fills the SSID, sets `hoUrl` to `http://<hostname>.local` from the
submitted `patch.hostname` (falling back to `ST.host`), reveals the card, and
calls `selectTab('status')`.

Two details:

- **Hoist the poll handle.** `setInterval(poll, 5000)` is currently a bare call at
  the end of the file. It needs to be `pollTimer = setInterval(...)` so the
  handoff can stop it — otherwise the card is buried under
  `Could not reach the device` toasts the moment the AP drops.
- **Name the network honestly.** `netBegin` joins the strongest *visible* saved
  network, not row 1. Name it only when `patch.wifi.length === 1`; otherwise say
  "one of your saved networks".

Optionally widen `scheduleReboot(800)` to ~1500 ms at
[`Web.cpp:194`](../firmware/src/Web.cpp) for render margin. The response is already
sent before the reboot is scheduled, so 800 ms is probably enough — but the cost of
being wrong is the user seeing nothing at all.

---

## Verification

Bench-test on one unit, factory-reset between runs:

- [ ] `/api/factory`, reboot: setup screen shows SSID at size 2, IP at size 3, and
      the `.local` name under "After setup:". Read it from across a table.
- [ ] Join the hotspot on an **iPhone** and an **Android** phone: portal pops,
      opens on the WiFi tab, first card says "Set up WiFi first".
- [ ] Save a network: the handoff card appears and survives the AP dropping — no
      unreachable-device toasts on top of it.
- [ ] Device rejoins: commissioning screen stays up **indefinitely** with hostname,
      IP, and animating dots. Leave it an hour.
- [ ] `curl -X POST http://<ip>/api/usage -d '{"s":42,"sr":118,"w":63,"wr":4200,"st":"allowed","ok":true}'`
      → stats screen within one loop pass.
- [ ] Power-cycle: comes straight back to stats/mascot, **not** the commissioning
      screen. Confirms `commissioned` persisted.
- [ ] `GET /api/export` shows `"commissioned": true`; `POST /api/import` of a blob
      containing `"commissioned": true` onto a fresh unit leaves it false.
- [ ] Set a pull URL instead of pushing: a successful pull also commissions.
- [ ] Set a 30-character hostname: no text overflows the panel edge.
- [ ] Set a hotspot password: the password-case layout does not collide.

## Out of scope

Still blocking a giveaway, tracked elsewhere:

- **The agent does not exist** ([`agent/README.md`](../agent/README.md),
  milestone 7). Every path above ends at a one-line installer with no binary
  behind it. This is the critical path; the work here only makes sure people reach
  it.
- **Guest-WiFi client isolation** will silently kill the push at a venue. Nothing
  on screen distinguishes "no agent" from "agent blocked". Needs a diagnosis, and
  the commissioning screen is the natural place to say so.
- **Mascot licence unresolved** ([`mascot-licence.md`](mascot-licence.md)).
- **The printed card.** No on-device change solves the phone-to-laptop handoff:
  someone who sets up on their phone needs the URL on their laptop. Paper does not
  time out. [`provision/README.md`](../provision/README.md) assumes sequential
  `clawdmeter-07` names, but without provisioning the names are chip-derived
  (`clawd-a111`) — either set a friendly hostname during the one unavoidable
  per-unit flash, or print the chip name.
</content>
</invoke>
