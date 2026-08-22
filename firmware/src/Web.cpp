// Web.cpp — HTTP config UI, REST API, agent bootstrap, and the OTA endpoint.
//
// /update deliberately accepts ANY image that fits: this firmware, upstream
// smalltv-mod, ESPHome, Tasmota, or GeekMagic's stock build. Recipients own
// their hardware. What it does NOT do is let an obvious mistake through
// silently — this board has no download-mode rescue, so a wrong file means
// opening the case and soldering. Hence the two guards in handleUpdateUpload:
// a size check against the free sketch space, and a magic-byte check that the
// upload is an ESP8266 image at all. Both explain themselves in the response.
//
// Derived from giovi321/smalltv-mod (WTFPL).
#include "Web.h"
#include "Platform.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "webui.h"
#include "agent_install.h"
#include "Net.h"
#include "Gfx.h"
#include "OtaUpdate.h"
#include "UsageClient.h"
#include "Clock.h"

// Defined in main.cpp — re-init the mode + force a repaint after a config change.
extern void appInvalidate();
extern const char* appResetReason();   // last reset reason (diagnostics)
extern void appApplyBrightness();      // re-resolve effective brightness now

static WebServerClass server(80);
static Settings*      S = nullptr;
static bool           g_reboot = false;
static uint32_t       g_rebootAt = 0;
#if WITH_SELFUPDATE
static bool           g_selfUpdate = false;   // GitHub self-update requested
#endif
static String         g_updateMsg;            // last self-update status/error
static String         g_uploadErr;            // why an /update upload was rejected
static bool           g_sawFirstChunk = false;

static void scheduleReboot(uint32_t inMs) {
  g_reboot = true;
  g_rebootAt = millis() + inMs;
}

// ---- optional web UI password ---------------------------------------------
// Off by default. When on, every handler below starts with this line and every
// page and endpoint needs the credentials. Digest, not basic, so the password is
// never sent over a plain-HTTP LAN. Two deliberate exceptions:
//   - the captive-portal probes, which a phone fires before anyone can type a
//     password and which only ever redirect,
//   - /api/usage, the agent's push endpoint, which has no way to carry
//     credentials and only writes the numbers on the screen.
// Returns true when the request may proceed; on false it has already answered.
static bool requireAuth() {
  if (!S->auth.enabled || !S->auth.pass.length()) return true;
  if (server.authenticate(S->auth.user.c_str(), S->auth.pass.c_str())) return true;
  server.requestAuthentication(HTTPAuthMethod::DIGEST_AUTH, AUTH_REALM,
                               "Authentication required");
  return false;
}

// ---------------------------------------------------------------------------
static void sendJson(JsonDocument& doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

static void handleRoot() {
  if (!requireAuth()) return;
  server.sendHeader("Cache-Control", "no-cache");
  server.sendHeader("Content-Encoding", "gzip");   // webui.h is generated, gzip'd
  server.send_P(200, "text/html", (PGM_P)WEBUI_HTML_GZ, WEBUI_HTML_GZ_LEN);
}

// ---- agent bootstrap installers -------------------------------------------
// Served from the device because the machine that needs them may be joined to
// the setup AP, with no route to the internet and therefore no way to reach a
// GitHub release. These are a few KB of shell that fetch the real agent once
// the machine is back online. Not behind the password: they are public scripts
// with no device state in them, and the whole point is that they are reachable
// with one command before anything is configured.
static void handleInstallPs1() {
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/plain", (PGM_P)AGENT_INSTALL_PS1_GZ, AGENT_INSTALL_PS1_GZ_LEN);
}

static void handleInstallSh() {
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/plain", (PGM_P)AGENT_INSTALL_SH_GZ, AGENT_INSTALL_SH_GZ_LEN);
}

static void handleGetConfig() {
  if (!requireAuth()) return;
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  settingsToJson(*S, root, /*includeSecrets=*/false);
  root["chip"] = "esp8266";
  // The slim image has no TLS, so the System page hides the self-update button
  // rather than offering one that can only fail.
  root["selfUpdate"] = (bool)WITH_SELFUPDATE;
  sendJson(doc);
}

static void handleStatus() {
  if (!requireAuth()) return;
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["fw"] = FW_NAME;
  o["version"] = FW_VERSION;
  o["repo"] = REPO_URL;
  o["label"] = DEVICE_LABEL;
  if (g_updateMsg.length()) o["updateMsg"] = g_updateMsg;
  o["mode"] = (netMode() == NET_AP) ? "ap" : "sta";
  o["connected"] = netConnected();
  o["ssid"] = netSSID();
  o["ip"] = netIP();
  o["host"] = S->hostname;
  o["rssi"] = netRSSI();
  o["heap"] = ESP.getFreeHeap();
  o["maxblk"] = platformMaxFreeBlock();     // largest contiguous block
  o["contstk"] = platformFreeContStack();   // primary stack headroom
  o["uptime"] = millis() / 1000;
  o["reset"] = appResetReason();
  o["synced"] = clockSynced();
  { String ts = clockTimeStr(); if (ts.length()) o["time"] = ts; }
  o["tz"]         = S->clock.tz;
  o["night"]      = clockNightActive();   // dimming now
  o["nightHeld"]  = clockNightHeld();     // in the window, waiting for a fresh sync
  o["clockFresh"] = clockTrusted();       // last NTP sync within the trust window

  // Meter health. This is the "is my thing working" answer, so it carries the
  // age of the last push rather than just the numbers: a stale 40% and a live
  // 40% look identical otherwise.
  const UsageData& u = usageGet();
  JsonObject m = o["meter"].to<JsonObject>();
  m["valid"] = u.valid;
  m["error"] = u.error;
  if (u.valid) {
    m["sessionPct"]      = u.sessionPct;
    m["sessionResetMin"] = u.sessionResetMin;
    m["weeklyPct"]       = u.weeklyPct;
    m["weeklyResetMin"]  = u.weeklyResetMin;
    m["status"]          = u.status;
    m["ageSec"]          = (millis() - u.lastOkMs) / 1000;
  }
  sendJson(doc);
}

// Fingerprint of everything network-identity related: the WiFi list and the
// hostname. Changing any of it needs a reboot, because the connection and the
// mDNS registration are established once at boot.
static String netFingerprint(const Settings& s) {
  String f((int)s.wifiCount);
  for (uint8_t i = 0; i < s.wifiCount; i++) {
    f += '\n';
    f += s.wifi[i].ssid;
    f += '\x01';
    f += s.wifi[i].pass;
  }
  f += '\n';
  f += s.hostname;
  return f;
}

static void handlePostConfig() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }

  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "bad json");
    return;
  }

  String oldNet = netFingerprint(*S);

  settingsApplyJson(*S, doc.as<JsonObjectConst>());
  saveSettings(*S);

  // Live apply (no reboot needed for these)
  clockReapply(*S);         // re-arm SNTP iff the timezone changed
  appApplyBrightness();     // respects night / auto / manual
  gfxApplyColors(*S);       // rotation, colour order/inversion, channel gain
  appInvalidate();          // re-init the mode + repaint

  bool wifiChanged = netFingerprint(*S) != oldNet;

  JsonDocument res;
  res["ok"] = true;
  res["reboot"] = wifiChanged;
  sendJson(res);

  if (wifiChanged) scheduleReboot(800);
}

static void handleScan() {
  if (!requireAuth()) return;
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n && i < 25; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["enc"] = !platformScanIsOpen(i);
  }
  WiFi.scanDelete();
  sendJson(doc);
}

static void handleReboot() {
  if (!requireAuth()) return;
  server.send(200, "application/json", "{\"ok\":true}");
  scheduleReboot(400);
}

static void handleFactory() {
  if (!requireAuth()) return;
  factoryReset(*S);
  saveSettings(*S);
  server.send(200, "application/json", "{\"ok\":true}");
  scheduleReboot(400);
}

// Full settings backup: stream the persisted config.json verbatim. It includes
// the WiFi passwords — same trust domain as typing them into this page.
static void handleExport() {
  if (!requireAuth()) return;
  File f = LittleFS.open("/config.json", "r");
  if (!f) { server.send(404, "text/plain", "no config saved yet"); return; }
  server.sendHeader("Content-Disposition", "attachment; filename=clawdmeter-config.json");
  server.streamFile(f, "application/json");
  f.close();
}

// Restore a backup: apply everything, persist, reboot (WiFi/hostname may change).
// Also the provisioning hook — flash.ps1 POSTs a per-unit settings JSON here.
static void handleImport() {
  if (!requireAuth()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "bad json");
    return;
  }
  settingsApplyJson(*S, doc.as<JsonObjectConst>());
  saveSettings(*S);
  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  scheduleReboot(800);
}

static void handleRefresh() {
  if (!requireAuth()) return;
  usageForceRefresh();
  server.send(200, "application/json", "{\"ok\":true}");
}

#if WITH_SELFUPDATE
static void handleCheckUpdate() {
  if (!requireAuth()) return;
  OtaLatest r = otaCheckLatest(*S);
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["current"] = FW_VERSION;
  o["ok"] = r.ok;
  o["latest"] = r.tag;
  o["newer"] = r.newer;
  if (!r.ok) o["error"] = r.error;
  sendJson(doc);
}

// Trigger the self-update. The actual (blocking) download runs from the loop so
// this response returns first; on success the device reboots into the new image.
static void handleSelfUpdate() {
  if (!requireAuth()) return;
  g_selfUpdate = true;
  g_updateMsg = "starting...";
  server.send(200, "application/json", "{\"ok\":true}");
}
#endif

// Push endpoint: the agent POSTs the usage payload here. Body is the
// {s,sr,w,wr,st,ok} contract. Unauthenticated on purpose — see requireAuth.
static void handleUsagePush() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  bool ok = usageApply(server.arg("plain"));
  server.send(ok ? 200 : 400, "application/json",
              ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ---- firmware upload ------------------------------------------------------
static void handleUpdateDone() {
  if (!requireAuth()) return;
  server.sendHeader("Connection", "close");
  if (g_uploadErr.length()) {
    // Rejected by one of the guards below. Say what and why — the person on the
    // other end is trying to install something and deserves a reason.
    server.send(400, "text/plain", g_uploadErr.c_str());
    g_uploadErr = "";
    return;
  }
  bool ok = !Update.hasError();
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : platformUpdateError().c_str());
  if (ok) scheduleReboot(1200);
}

// The upload callback runs while the body streams in, ahead of handleUpdateDone,
// so the password has to be checked here too: guarding only the final handler
// would let an unauthenticated POST write a whole image to flash and merely lose
// the 200 at the end.
static void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (S->auth.enabled && S->auth.pass.length() &&
      !server.authenticate(S->auth.user.c_str(), S->auth.pass.c_str())) {
    if (up.status != UPLOAD_FILE_START) Update.end();
    return;
  }

  if (up.status == UPLOAD_FILE_START) {
    g_uploadErr = "";
    g_sawFirstChunk = false;
    WiFiUDP::stopAll();   // free UDP sockets so the OTA has max contiguous flash/heap
    uint32_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;

    // Guard 1: size. Content-Length bounds the image from above (it also covers
    // the multipart wrapper, so this only ever rejects genuinely-too-big files).
    if (server.hasHeader("Content-Length")) {
      uint32_t declared = (uint32_t)server.header("Content-Length").toInt();
      if (declared > maxSpace) {
        g_uploadErr = String("image too large: ") + declared + " B declared, "
                    + maxSpace + " B of free sketch space. Free space is what is "
                      "left above the running firmware, so a large image may need "
                      "the slim build installed first.";
        return;
      }
    }
    if (!Update.begin(maxSpace)) Update.printError(Serial);

  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (g_uploadErr.length()) return;

    // Guard 2: is this an ESP8266 image at all? Every one starts with the
    // 0xE9/0xEA flash header. An ESP32 image, a .zip, or an accidentally
    // selected config.json fails here instead of being written to flash — and
    // on this board a bricked flash means a soldering iron, not esptool.
    if (!g_sawFirstChunk) {
      g_sawFirstChunk = true;
      if (up.currentSize > 0 && up.buf[0] != 0xE9 && up.buf[0] != 0xEA) {
        g_uploadErr = String("not an ESP8266 firmware image (first byte 0x")
                    + String(up.buf[0], HEX) + ", expected 0xE9 or 0xEA). "
                      "Nothing was written.";
        Update.end();
        return;
      }
    }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);

  } else if (up.status == UPLOAD_FILE_END) {
    if (g_uploadErr.length()) return;
    if (!Update.end(true)) Update.printError(Serial);

  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.end();
  }
  yield();
}

// ---- captive portal -------------------------------------------------------
static void handleNotFound() {
  if (netMode() == NET_AP) {
    // Redirect everything to the config page so the captive portal pops.
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

// ---------------------------------------------------------------------------
void webPortalBegin(Settings& settings) {
  S = &settings;

#if WITH_SELFUPDATE
  // If the last boot ran a queued GitHub update and failed, surface why
  // (success reboots into the new image before we ever get here).
  g_updateMsg = otaTakeBootResult();
#endif

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/factory", HTTP_POST, handleFactory);
  server.on("/api/refresh", HTTP_POST, handleRefresh);
  server.on("/api/export", HTTP_GET, handleExport);
  server.on("/api/import", HTTP_POST, handleImport);
  server.on("/api/usage", HTTP_POST, handleUsagePush);   // the agent pushes here
#if WITH_SELFUPDATE
  server.on("/api/checkupdate", HTTP_GET, handleCheckUpdate);
  server.on("/api/selfupdate", HTTP_POST, handleSelfUpdate);
#endif
  server.on("/agent/install.ps1", HTTP_GET, handleInstallPs1);
  server.on("/agent/install.sh", HTTP_GET, handleInstallSh);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);

  // Content-Length is not collected by default; the /update size guard needs it.
  server.collectHeaders("Content-Length");

  // Common captive-portal probe endpoints
  server.on("/generate_204", handleNotFound);
  server.on("/gen_204", handleNotFound);
  server.on("/hotspot-detect.html", handleNotFound);
  server.on("/connecttest.txt", handleNotFound);
  server.onNotFound(handleNotFound);

  server.begin();
}

void webPortalLoop() {
  server.handleClient();

#if WITH_SELFUPDATE
  // Run the GitHub self-update outside the request handler so the browser gets
  // its response first.
  if (g_selfUpdate) {
    g_selfUpdate = false;
    // RAM-tight chip: verify there is something to install, then queue the
    // download for the next boot (otaBootUpdate in setup(), ~45 KB free) and
    // reboot. A failure there lands back in g_updateMsg via otaTakeBootResult.
    OtaLatest r = otaCheckLatest(*S);
    if (!r.ok)         g_updateMsg = "check failed: " + r.error;
    else if (!r.newer) g_updateMsg = "already up to date (" FW_VERSION ")";
    else if (otaRequestBootUpdate(r.tag.c_str())) {
      g_updateMsg = "updating...";
      scheduleReboot(400);
    } else {
      g_updateMsg = F("could not queue update (storage error)");
    }
  }
#endif
}

bool webPortalRebootDue() {
  return g_reboot && (int32_t)(millis() - g_rebootAt) >= 0;
}
