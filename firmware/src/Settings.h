// Settings.h — persisted configuration (LittleFS /config.json)
//
// Shared device/network fields live at the top level; the meter owns a nested
// slice. config.json mirrors this: { ..shared.., "usage":{...}, "clock":{...} }.
//
// settingsApplyJson applies ONLY the keys present, so POST /api/config is a
// partial merge — a one-key body cannot clobber WiFi credentials. That property
// is load-bearing for the provisioning scripts; there is a test for it.
//
// Derived from giovi321/smalltv-mod (WTFPL).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"

// One saved WiFi station network. The device keeps up to MAX_WIFI_NETS and
// joins the strongest visible one at boot (hidden SSIDs are tried last).
struct WifiCred {
  String ssid;
  String pass;
};

// ---- Meter slice ----------------------------------------------------------
// usageUrl is optional and only used if someone wants the device to PULL from
// an agent instead of being pushed to. The shipped agent pushes, which is why
// the slim image needs no TLS.
struct UsageSettings {
  String   usageUrl;      // optional pull endpoint, e.g. http://192.168.1.10:8787/
  uint16_t pollSec;       // pull period (ignored when usageUrl is empty)
  bool     showMascot;    // mascot animation vs. bare gauges

  void setDefaults();
  void toJson(JsonObject o) const;
  void fromJson(JsonObjectConst o);   // applies only the keys present
};

// ---- Clock / night mode slice (device-wide) --------------------------------
struct ClockSettings {
  String   tz;            // IANA display name, e.g. "Europe/Amsterdam"
  String   tzPosix;       // POSIX TZ rule fed to SNTP
  bool     nightEnabled;
  uint16_t nightStartMin; // minutes since local midnight (0..1439)
  uint16_t nightEndMin;
  uint8_t  nightLevel;    // 0..100, 0 = backlight off

  void setDefaults();
  void toJson(JsonObject o) const;
  void fromJson(JsonObjectConst o);
};

// ---- Web UI password slice (device-wide) -----------------------------------
// Off by default: the settings page is open on the LAN. Turning it on puts every
// page and endpoint behind HTTP digest auth, with the exceptions in Web.cpp.
// Like the other secrets here the password reaches the config file and the
// settings export, never the web API.
struct AuthSettings {
  bool   enabled;
  String user;
  String pass;

  void setDefaults();
  void toJson(JsonObject o, bool includeSecrets) const;
  void fromJson(JsonObjectConst o);
};

// ---- Panel colour slice (device-wide) --------------------------------------
// Same firmware, different panels: units of one variant render the same RGB565
// value differently, and a few have red and blue swapped in the controller.
struct DisplaySettings {
  uint8_t colorOrder;   // COLOR_ORDER_AUTO / _RGB / _BGR (auto = Board.h default)
  bool    invert;       // flip the panel inversion bit (washed-out / negative panels)
  uint8_t rGain;        // per-channel gain in percent, 50..150, 100 = untouched
  uint8_t gGain;
  uint8_t bGain;

  void setDefaults();
  void toJson(JsonObject o) const;
  void fromJson(JsonObjectConst o);
};

// ---- Top-level settings ----------------------------------------------------
struct Settings {
  // --- WiFi station networks (the device joins one of these) ---
  WifiCred wifi[MAX_WIFI_NETS];
  uint8_t  wifiCount;

  // --- Access point (config / fallback hotspot) ---
  String apSsid;
  String apPass;        // empty => open network
  String hostname;      // mDNS name => http://<hostname>.local

  // --- Active feature. One mode today; kept so a second can be added. ---
  uint8_t mode;

  // A usage payload has arrived at least once. Until it has, the screen shows the
  // commissioning address instead of an empty meter — the recipient's remaining
  // step is on their computer, and this is the only place they can be told so.
  // Written by loop() on the first payload, read straight from the file by
  // loadSettings; settingsApplyJson deliberately ignores it (see Settings.cpp).
  bool commissioned;

  // --- Shared HTTP / display ---
  uint16_t httpTimeout;       // ms
  uint8_t  brightness;        // 0..100 %
  bool     autoBrightness;    // use LDR on A0
  bool     backlightInverted; // active-low backlight
  uint8_t  rotation;          // 0..3 screen orientation

  // --- Slices ---
  UsageSettings   usage;
  ClockSettings   clock;
  DisplaySettings display;   // panel colour correction
  AuthSettings    auth;      // optional web UI password

  void setDefaults();
};

// Persistence
bool settingsBegin();                       // mount LittleFS
bool loadSettings(Settings& s);             // false => defaults applied
bool saveSettings(const Settings& s);
void factoryReset(Settings& s);             // wipe file + defaults

// JSON <-> struct. `includeSecrets=false` masks passwords for the web API.
void settingsToJson(const Settings& s, JsonObject root, bool includeSecrets);
void settingsApplyJson(Settings& s, JsonObjectConst root); // partial update allowed
