// Clawdmeter — Texterous firmware for the GeekMagic SmallTV-Ultra (ESP-12F / ESP8266)
//
// One screen: Claude Code usage, pushed to POST /api/usage by the companion
// agent. Shared plumbing (WiFi, web UI, OTA, display core, settings) lives at
// src root; the meter itself is a self-contained DisplayMode under src/meter.
//
// The mode registry below has exactly one entry. It stays a registry so adding a
// second screen later does not mean restructuring this file.
//
// Derived from giovi321/smalltv-mod (WTFPL). See ../../README.md.
#include <Arduino.h>
#include "Platform.h"
#include "config.h"
#include "Settings.h"
#include "Net.h"
#include "Gfx.h"
#include "Web.h"
#include "OtaUpdate.h"
#include "Mode.h"
#include "Clock.h"
#include "UsageMode.h"
#include "SessionsMode.h"
#include "Commission.h"
#include "UsageClient.h"

// ---- mode registry --------------------------------------------------------
static DisplayMode* kModes[] = { &g_usageMode, &g_sessionsMode };
static const size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

static DisplayMode* activeMode(const Settings& s) {
  for (size_t i = 0; i < kModeCount; i++)
    if (kModes[i]->modeConst() == s.mode) return kModes[i];
  return kModeCount ? kModes[0] : nullptr;   // unknown mode -> first compiled one
}

static Settings g_settings;
static String   g_resetReason;        // why the chip last reset (diagnostics)
static bool     g_safeMode = false;   // last reset was an exception -> stay out of it
static int8_t   g_shownMode = -1;     // NetMode the screen reflects; -1 = stale/unknown
static bool     g_commissionDue = false;  // a payload landed; persist the flag from loop()
static char     g_epcStr[16] = "";
static char     g_addrStr[16] = "";
static int      g_lastBr = -1;        // last effective brightness written (-1 = none)
#if HAS_LDR
static uint32_t g_lastAutoBr = 0;
static uint8_t  g_ldrCache   = DEFAULT_BRIGHTNESS;   // last LDR reading (2 s cadence)
#endif

// Single brightness resolver: night mode overrides auto-brightness overrides the
// manual level. Only writes the PWM when the effective target changes.
static uint8_t appEffectiveBrightness() {
  if (clockNightActive()) return g_settings.clock.nightLevel;
#if HAS_LDR
  if (g_settings.autoBrightness) {
    if (millis() - g_lastAutoBr > 2000) {
      g_lastAutoBr = millis();
      int raw = analogRead(LDR_PIN);
      g_ldrCache = (uint8_t)constrain(raw * 100 / ADC_MAX, 5, 100);
    }
    return g_ldrCache;
  }
#endif
  return g_settings.brightness;
}

void appApplyBrightness() {
  uint8_t t = appEffectiveBrightness();
  if ((int)t != g_lastBr) {
    g_lastBr = t;
    gfxSetBrightness(t, g_settings.backlightInverted);
  }
}

// Exposed to the web portal (/api/status) so the last reset reason is visible.
const char* appResetReason() { return g_resetReason.c_str(); }

// Called by the web portal after settings are applied: re-init the mode and
// force a repaint so a change takes effect immediately.
//
// The setup and commissioning screens are painted from loop() rather than by a
// mode, so invalidating the registry alone used to leave them untouched — and
// handlePostConfig calls gfxApplyColors() just before this, which re-issues MADCTL
// with the new rotation. Changing Rotation while either screen is up therefore
// left its existing pixels being scanned out transposed, with no repaint path
// short of a power cycle: on those two screens the address is the only copy the
// recipient has, so they could erase it from inside the UI. Rotation is not part
// of netFingerprint (correctly — it needs no reboot), so nothing else catches it.
// Dropping g_shownMode to -1 makes loop()'s entry branch repaint whichever of the
// two is showing.
void appInvalidate() {
  for (size_t i = 0; i < kModeCount; i++) kModes[i]->invalidate(g_settings);
  commissionInvalidate();
  g_shownMode = -1;
}

// Called from UsageClient's single parse commit point, on both the pushed and the
// pulled path, so the first payload of a unit's life retires the commissioning
// screen for good. Only ever sets a RAM flag: the LittleFS write belongs in
// loop(), out of the HTTP response, exactly as webPortalLoop defers the update.
//
// Deliberately does NOT call appInvalidate(): that runs UsageMode::invalidate ->
// usageInit -> clear(), which would wipe the very payload that just arrived.
// Nothing is needed — needRender_ is still true because no mode has serviced yet,
// so the first stats paint happens on the next loop pass by itself.
void appMarkCommissioned() {
  if (g_settings.commissioned) return;   // once ever, then off the hot path
  g_commissionDue = true;
}

static uint32_t g_splashAt = 0;   // when the brand mark first went up

// Boot progress. Repaints only the caption strip so the mark stays put for the
// whole boot; gfxBoot here would fillScreen and wipe it on the first call.
static void bootProgress(const char* msg) {
  if (g_safeMode) gfxBoot("Crashed", msg);
  else            gfxSplashCaption(msg);
}

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(FW_NAME " " FW_VERSION);

  // Capture why we (re)booted. On a reboot loop this is the key clue, and this
  // board exposes no UART without opening the case — so it also goes on screen
  // below. The ESP8266 keeps the crash PC (epc1) for addr2line decoding.
  PlatformReset pr = platformResetInfo();
  Serial.print("[boot] reset reason: ");
  Serial.println(pr.reason);

  if (pr.wasCrash) {
    g_safeMode = true;                   // crashed last boot -> stay out of that path
    strlcpy(g_epcStr,  pr.epc,  sizeof(g_epcStr));
    strlcpy(g_addrStr, pr.addr, sizeof(g_addrStr));
    char rich[80];
    snprintf(rich, sizeof(rich), "%s epc %s addr %s", pr.reason.c_str(),
             g_epcStr[0] ? g_epcStr : "-", g_addrStr[0] ? g_addrStr : "-");
    g_resetReason = rich;
  } else {
    g_resetReason = pr.reason;
  }

  Serial.println("[boot] settings");
  settingsBegin();
  loadSettings(g_settings);

  Serial.println("[boot] display");
  gfxBegin(g_settings);
  if (g_safeMode) {
    gfxBoot("Crashed", FW_VERSION);
  } else {
    gfxSplash(DEVICE_LABEL " " FW_VERSION);
    g_splashAt = millis();
  }

  Serial.println("[boot] net");
  netBegin(g_settings, bootProgress);
  // Arm SNTP now that WiFi is up, but only if night mode needs it — SNTP costs
  // heap this chip would rather keep. clockReapply arms it iff needed. Skipped
  // after a crash so a fault in here cannot boot-loop before the web server
  // starts; the device then comes up in safe mode, OTA-recoverable.
  if (!g_safeMode) clockReapply(g_settings);

#if WITH_SELFUPDATE
  // A GitHub update queued from the web UI runs now, before the meter claims
  // heap: the download needs a 16 KB TLS buffer that only fits at boot. On
  // success it reboots into the new image.
  if (otaBootRequested()) {
    Serial.println("[boot] github update");
    gfxBoot(DEVICE_LABEL, "updating...");
    otaBootUpdate(g_settings);
    gfxBoot(DEVICE_LABEL, "update failed");   // still here -> failed; details in the UI
    delay(1200);
  }
#endif

  Serial.println("[boot] web");
  webPortalBegin(g_settings);

  Serial.println("[boot] meter");
  for (size_t i = 0; i < kModeCount; i++) kModes[i]->begin(g_settings);
  Serial.println("[boot] done");

  // Let the mark finish its minimum time on screen before anything replaces it.
  if (g_splashAt) {
    uint32_t shown = millis() - g_splashAt;
    if (shown < SPLASH_MIN_MS) delay(SPLASH_MIN_MS - shown);
  }

  if (netMode() == NET_AP) {
    g_shownMode = NET_AP;
    // netSSID(), not g_settings.apSsid: startAP falls back to the factory name
    // when softAP() refuses the configured one (empty, or over 32 chars), and
    // this screen is the recipient's only label for which of thirty hotspots is
    // theirs. The name on the glass has to be the one that is really on the air.
    gfxApInfo(netSSID().c_str(), g_settings.apPass.c_str(),
              netIP().c_str(), g_settings.hostname.c_str());
  } else if (g_safeMode) {
    // Last boot crashed: show the crash address and keep the web server up for
    // OTA recovery — do not enter the render path that crashed.
    gfxCrash(g_epcStr, g_addrStr, netIP().c_str());
  } else {
    g_shownMode = NET_STA;
    // Which network we joined and how to reach the web UI, long enough to read.
    // A unit that has never been fed skips it and goes straight to the
    // commissioning screen: that screen carries the same address and does not
    // expire, so the 3.5 s flash would only delay it — and 3.5 s is exactly how
    // long a recipient spends head-down in their phone's WiFi settings getting
    // back onto their own network. A commissioned unit keeps the flash; it is the
    // only time a settled device says where to find it.
    if (g_settings.commissioned) {
      gfxStaInfo(netSSID().c_str(), netIP().c_str(), g_settings.hostname.c_str());
      delay(3500);
    }
  }
}

void loop() {
  netLoop();
  webPortalLoop();

  if (webPortalRebootDue()) {
    delay(120);
    ESP.restart();
  }

  // The first usage payload retires the commissioning screen for good. The write
  // happens here rather than inside the push handler for the same reason
  // webPortalLoop defers the self-update: a LittleFS write belongs outside the
  // HTTP response, not in the middle of one.
  if (g_commissionDue) {
    g_commissionDue = false;
    g_settings.commissioned = true;
    saveSettings(g_settings);
  }

  if (g_safeMode) {
    delay(5);
    return;  // crashed last boot: web UI stays up for OTA recovery, no rendering
  }

  if (netMode() == NET_AP) {
    // Repainted on every ENTRY to AP mode, not just at boot. A unit that joined
    // and later lost its network falls back here, and a stale "ALMOST DONE, open
    // clawd-xxxx.local" over a hotspot that no longer routes there is the kind of
    // lie that ends a setup.
    if (g_shownMode != NET_AP) {
      g_shownMode = NET_AP;
      gfxApInfo(netSSID().c_str(), g_settings.apPass.c_str(),
                netIP().c_str(), g_settings.hostname.c_str());
    }
    delay(5);
    return;  // setup mode: AP info stays on screen
  }

  // --- STA mode: the meter fetches (or waits for a push) and renders itself ---
  if (g_shownMode != NET_STA) {
    g_shownMode = NET_STA;
    commissionInvalidate();                                  // repaint over the AP screen
    for (size_t i = 0; i < kModeCount; i++) kModes[i]->wake(g_settings);
  }

  // Night-mode state machine (NTP-trust gate), then the effective brightness
  // (night override / auto-brightness / manual level). Both run before the
  // commissioning branch below so night dimming reaches that screen too.
  clockService(g_settings);
  appApplyBrightness();

  if (!g_settings.commissioned) {
    // Pull mode has no other pump: usageService() is called from the meter's
    // service(), which the return below never reaches — so a unit configured to
    // PULL would wait here forever for a payload only a fetch can produce. Same
    // URL guard the two modes use, because usageService() has none of its own and
    // would flag an error every tick on a push-mode unit.
    if (g_settings.usage.usageUrl.length() >= 8) usageService(g_settings);

    // Data in the buffer means a payload arrived, by push or by the pull above.
    // appMarkCommissioned() is the precise signal and fires on the parse itself;
    // this is the backstop, because a screen that outlives the data it is waiting
    // for is the one failure a recipient has no way to diagnose. One bool read,
    // on a path that stops running after the first payload of a unit's life.
    if (!g_commissionDue && !usageGet().valid) {
      commissionService(g_settings);
      delay(5);
      return;  // never fed: the address is the whole screen, and it does not expire
    }
    g_commissionDue = true;   // fall through to the meter; the save lands next pass
  }

  DisplayMode* m = activeMode(g_settings);
  if (m) m->service(g_settings);

  delay(5);
}
