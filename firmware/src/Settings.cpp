// Settings.cpp — LittleFS-backed configuration.
//
// The one invariant worth stating: settingsApplyJson applies ONLY the keys that
// are present. POST /api/config is therefore a partial merge, so a body of just
// {"brightness":50} cannot wipe WiFi. Blank passwords likewise mean "keep the
// stored one", so the web UI can round-trip settings it was never allowed to read.
//
// Derived from giovi321/smalltv-mod (WTFPL).
#include "Settings.h"
#include "Platform.h"   // platformChipId() for the unique default hostname
#include <LittleFS.h>

static const char* CONFIG_PATH = "/config.json";

// ===========================================================================
// Meter slice
// ===========================================================================
void UsageSettings::setDefaults() {
  usageUrl   = "";
  pollSec    = DEFAULT_USAGE_POLL_SEC;
  showMascot = true;
}

void UsageSettings::toJson(JsonObject o) const {
  o["usageUrl"]   = usageUrl;
  o["pollSec"]    = pollSec;
  o["showMascot"] = showMascot;
}

void UsageSettings::fromJson(JsonObjectConst o) {
  if (o["usageUrl"].is<const char*>())  usageUrl = o["usageUrl"].as<String>();
  if (o["pollSec"].is<int>())           pollSec = constrain((int)o["pollSec"], 10, 3600);
  if (o["showMascot"].is<bool>())       showMascot = o["showMascot"];
}

// ===========================================================================
// Clock / night mode slice
// ===========================================================================
static uint16_t hhmmToMin(const char* s, uint16_t fallback) {
  if (!s || !s[0]) return fallback;
  int h = 0, m = 0;
  if (sscanf(s, "%d:%d", &h, &m) != 2) return fallback;
  if (h < 0 || h > 23 || m < 0 || m > 59) return fallback;
  return (uint16_t)(h * 60 + m);
}
static String minToHhmm(uint16_t v) {
  if (v > 1439) v = 0;
  char b[6];
  snprintf(b, sizeof(b), "%02u:%02u", (unsigned)(v / 60), (unsigned)(v % 60));
  return String(b);
}

void ClockSettings::setDefaults() {
  tz            = DEFAULT_TZ_NAME;
  tzPosix       = DEFAULT_TZ_POSIX;
  nightEnabled  = DEFAULT_NIGHT_ENABLED;
  nightStartMin = DEFAULT_NIGHT_START_MIN;
  nightEndMin   = DEFAULT_NIGHT_END_MIN;
  nightLevel    = DEFAULT_NIGHT_LEVEL;
}

void ClockSettings::toJson(JsonObject o) const {
  o["tz"]           = tz;
  o["tzPosix"]      = tzPosix;
  o["nightEnabled"] = nightEnabled;
  o["nightStart"]   = minToHhmm(nightStartMin);
  o["nightEnd"]     = minToHhmm(nightEndMin);
  o["nightLevel"]   = nightLevel;
}

void ClockSettings::fromJson(JsonObjectConst o) {
  if (o["tz"].is<const char*>())          tz = o["tz"].as<String>();
  if (o["tzPosix"].is<const char*>())     tzPosix = o["tzPosix"].as<String>();
  if (o["nightEnabled"].is<bool>())       nightEnabled = o["nightEnabled"];
  if (o["nightStart"].is<const char*>())  nightStartMin = hhmmToMin(o["nightStart"], nightStartMin);
  if (o["nightEnd"].is<const char*>())    nightEndMin   = hhmmToMin(o["nightEnd"], nightEndMin);
  if (o["nightLevel"].is<int>())          nightLevel = constrain((int)o["nightLevel"], 0, 100);
}

// ===========================================================================
// Web UI password slice
// ===========================================================================
void AuthSettings::setDefaults() {
  enabled = false;
  user = DEFAULT_AUTH_USER;
  pass = "";
}

void AuthSettings::toJson(JsonObject o, bool includeSecrets) const {
  // Never enable in the saved config without a password to check against; a
  // blank one would lock the page with a credential nobody can supply.
  o["enabled"] = enabled && pass.length() > 0;
  o["user"]    = user;
  o["passSet"] = pass.length() > 0;
  if (includeSecrets) o["pass"] = pass;
}

void AuthSettings::fromJson(JsonObjectConst o) {
  if (o["user"].is<const char*>()) user = o["user"].as<String>();
  // Blank keeps the stored password, as everywhere else in this file.
  if (o["pass"].is<const char*>()) {
    String p = o["pass"].as<String>();
    if (p.length()) pass = p;
  }
  if (o["enabled"].is<bool>()) enabled = o["enabled"];
  if (user.length() >= MAX_AUTH_USER_LEN) user.remove(MAX_AUTH_USER_LEN - 1);
  if (pass.length() >= MAX_AUTH_PASS_LEN) pass.remove(MAX_AUTH_PASS_LEN - 1);
  if (!user.length()) user = DEFAULT_AUTH_USER;
  // Same guard as toJson, for a config imported by hand.
  if (!pass.length()) enabled = false;
}

// ===========================================================================
// Panel colour slice
// ===========================================================================
void DisplaySettings::setDefaults() {
  colorOrder = DEFAULT_COLOR_ORDER;
  invert     = DEFAULT_COLOR_INVERT;
  rGain = gGain = bGain = DEFAULT_COLOR_GAIN;
}

void DisplaySettings::toJson(JsonObject o) const {
  o["colorOrder"] = (colorOrder == COLOR_ORDER_RGB) ? "rgb"
                  : (colorOrder == COLOR_ORDER_BGR) ? "bgr" : "auto";
  o["invert"] = invert;
  o["rGain"]  = rGain;
  o["gGain"]  = gGain;
  o["bGain"]  = bGain;
}

void DisplaySettings::fromJson(JsonObjectConst o) {
  if (o["colorOrder"].is<const char*>()) {
    String c = o["colorOrder"].as<String>();
    colorOrder = c.equalsIgnoreCase("rgb") ? COLOR_ORDER_RGB
               : c.equalsIgnoreCase("bgr") ? COLOR_ORDER_BGR : COLOR_ORDER_AUTO;
  }
  if (o["invert"].is<bool>()) invert = o["invert"];
  if (o["rGain"].is<int>()) rGain = constrain((int)o["rGain"], MIN_COLOR_GAIN, MAX_COLOR_GAIN);
  if (o["gGain"].is<int>()) gGain = constrain((int)o["gGain"], MIN_COLOR_GAIN, MAX_COLOR_GAIN);
  if (o["bGain"].is<int>()) bGain = constrain((int)o["bGain"], MIN_COLOR_GAIN, MAX_COLOR_GAIN);
}

// ===========================================================================
// Top level
// ===========================================================================
void Settings::setDefaults() {
  wifiCount = 0;
  for (uint8_t i = 0; i < MAX_WIFI_NETS; i++) {
    wifi[i].ssid = "";
    wifi[i].pass = "";
  }
#if HAS_PROVISION
  // Batch image: seed the event network so a freshly flashed unit rejoins by
  // itself and nobody has to touch a setup screen on the day. A recipient adds
  // their own network later through the Wifi page; this row can be deleted there.
  wifi[0].ssid = PROVISION_SSID;
  wifi[0].pass = PROVISION_PASS;
  wifiCount = 1;
#endif
  // Both names carry the chip suffix so units are distinguishable out of the
  // box: the hostname because several on one network would otherwise collide on
  // mDNS, and the AP because a giveaway batch that falls back to setup mode
  // would otherwise put thirty identical open networks in the air, with no way
  // for anyone to tell which unit they are configuring. A name saved in
  // config.json overrides either.
  //
  // %04x, not String(v, HEX): that helper does not zero-pad, so the suffix used
  // to be 1-4 characters and every unit in a batch had a differently sized name.
  // A fixed width makes the panel layout provably safe for every unit, and makes
  // "the four characters after the dash" something a person can be told to check.
  char unitId[5];
  snprintf(unitId, sizeof(unitId), "%04x", (unsigned)(platformChipId() & 0xFFFF));
  apSsid   = String(DEFAULT_AP_SSID) + "-" + unitId;
  apPass   = DEFAULT_AP_PASS;
  hostname = String(DEFAULT_HOSTNAME) + "-" + unitId;

  mode = DEFAULT_MODE;
  commissioned = false;   // nothing has ever been pushed to this unit
  httpTimeout = DEFAULT_HTTP_TIMEOUT;

  brightness = DEFAULT_BRIGHTNESS;
  autoBrightness = false;
  backlightInverted = TFT_BL_DEFAULT_INVERTED;
  rotation = 0;

  usage.setDefaults();
  clock.setDefaults();
  display.setDefaults();
  auth.setDefaults();
}

// ---------------------------------------------------------------------------
bool settingsBegin() {
  if (LittleFS.begin()) return true;
  // First boot on a fresh chip: format then mount.
  if (LittleFS.format() && LittleFS.begin()) return true;
  return false;
}

bool loadSettings(Settings& s) {
  s.setDefaults();
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  settingsApplyJson(s, doc.as<JsonObjectConst>());
  // Read straight from the file rather than through settingsApplyJson: that path
  // is shared with POST /api/config and /api/import, and a provisioning import
  // or a restored backup must not be able to declare a unit commissioned and skip
  // the one screen that tells a recipient what to do. Only our own flash copy may.
  s.commissioned = doc["commissioned"] | false;
  return true;
}

bool saveSettings(const Settings& s) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  settingsToJson(s, root, /*includeSecrets=*/true);

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

void factoryReset(Settings& s) {
  LittleFS.remove(CONFIG_PATH);
  s.setDefaults();
}

// ---------------------------------------------------------------------------
void settingsToJson(const Settings& s, JsonObject root, bool includeSecrets) {
  root["hostname"] = s.hostname;

  // WiFi networks. Passwords only reach the config file, never the web API.
  JsonArray wf = root["wifi"].to<JsonArray>();
  for (uint8_t i = 0; i < s.wifiCount; i++) {
    JsonObject e = wf.add<JsonObject>();
    e["ssid"]    = s.wifi[i].ssid;
    e["passSet"] = s.wifi[i].pass.length() > 0;
    if (includeSecrets) e["pass"] = s.wifi[i].pass;
  }
  root["apSsid"]    = s.apSsid;
  root["apPassSet"] = s.apPass.length() > 0;
  if (includeSecrets) root["apPass"] = s.apPass;

  root["mode"]              = (s.mode == MODE_SESSIONS) ? "sessions" : "usage";
  root["commissioned"]      = s.commissioned;
  root["httpTimeout"]       = s.httpTimeout;
  root["brightness"]        = s.brightness;
  root["autoBrightness"]    = s.autoBrightness;
  root["backlightInverted"] = s.backlightInverted;
  root["rotation"]          = s.rotation;

  s.usage.toJson(root["usage"].to<JsonObject>());
  s.clock.toJson(root["clock"].to<JsonObject>());
  s.display.toJson(root["display"].to<JsonObject>());
  s.auth.toJson(root["auth"].to<JsonObject>(), includeSecrets);
}

// Apply only the keys that are present (partial update friendly).
void settingsApplyJson(Settings& s, JsonObjectConst root) {
  if (root["hostname"].is<const char*>()) s.hostname = root["hostname"].as<String>();

  if (root["wifi"].is<JsonArrayConst>()) {
    // The list is authoritative when present (order = try priority, missing row
    // = deletion). A blank password keeps the stored one, matched by SSID so
    // rows survive reordering.
    WifiCred old[MAX_WIFI_NETS];
    uint8_t oldCount = s.wifiCount;
    for (uint8_t i = 0; i < oldCount; i++) old[i] = s.wifi[i];

    s.wifiCount = 0;
    for (JsonObjectConst e : root["wifi"].as<JsonArrayConst>()) {
      if (s.wifiCount >= MAX_WIFI_NETS) break;
      const char* ssid = e["ssid"] | "";
      if (!ssid[0]) continue;                // skip blank rows
      WifiCred& dst = s.wifi[s.wifiCount];
      dst.ssid = ssid;
      const char* pass = e["pass"] | "";
      dst.pass = pass;
      if (!pass[0])
        for (uint8_t i = 0; i < oldCount; i++)
          if (old[i].ssid == dst.ssid) { dst.pass = old[i].pass; break; }
      s.wifiCount++;
    }
  }
  if (root["apSsid"].is<const char*>()) s.apSsid = root["apSsid"].as<String>();
  // AP password: apply as-is when present (empty allowed => open AP).
  if (root["apPass"].is<const char*>()) s.apPass = root["apPass"].as<String>();

  // "mode" picks the screen. An unrecognised token leaves the current mode
  // alone, so a config exported from a build with more screens still imports.
  if (root["mode"].is<const char*>()) {
    const char* m = root["mode"];
    if      (!strcmp(m, "usage"))    s.mode = MODE_USAGE;
    else if (!strcmp(m, "sessions")) s.mode = MODE_SESSIONS;
  }
  // "commissioned" is emitted by settingsToJson but deliberately NOT read here.
  // This function serves POST /api/config and POST /api/import, so reading it
  // would let a provisioning script or a hand-written blob declare a unit
  // commissioned and skip the commissioning screen. loadSettings reads the key
  // directly from our own file instead.
  if (root["httpTimeout"].is<int>())        s.httpTimeout = constrain((int)root["httpTimeout"], 1000, 20000);
  if (root["brightness"].is<int>())         s.brightness = constrain((int)root["brightness"], 0, 100);
  if (root["autoBrightness"].is<bool>())    s.autoBrightness = root["autoBrightness"];
  if (root["backlightInverted"].is<bool>()) s.backlightInverted = root["backlightInverted"];
  if (root["rotation"].is<int>())           s.rotation = (uint8_t)(((int)root["rotation"]) & 3);

  if (root["usage"].is<JsonObjectConst>())   s.usage.fromJson(root["usage"].as<JsonObjectConst>());
  if (root["clock"].is<JsonObjectConst>())   s.clock.fromJson(root["clock"].as<JsonObjectConst>());
  if (root["display"].is<JsonObjectConst>()) s.display.fromJson(root["display"].as<JsonObjectConst>());
  if (root["auth"].is<JsonObjectConst>())    s.auth.fromJson(root["auth"].as<JsonObjectConst>());
}
