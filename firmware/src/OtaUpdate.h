// OtaUpdate.h — pull the latest release from GitHub and flash it over the air.
//
// Checks the repo's newest release via the GitHub API, compares its tag to
// FW_VERSION, and (on request) streams the firmware asset (UPDATE_ASSET in
// config.h) straight into the OTA partition via ESP8266httpUpdate. The write is
// atomic: a failed download leaves the running firmware untouched, so this
// cannot brick the device. Compiled only when WITH_SELFUPDATE (i.e. not NO_TLS).
//
// HTTPS on the ESP8266 is RAM-tight: github.com and its asset CDN do not
// negotiate small TLS fragments, so the download needs a full 16 KB BearSSL
// receive buffer that does not fit next to the running features (~26 KB free).
// This chip therefore updates AT BOOT: the web UI queues the request in
// LittleFS and reboots, otaBootUpdate() runs early in setup() with ~45 KB free,
// and the device reboots again into the new image. Manual OTA (upload the .bin
// in the System tab) stays as the fallback, and is the ONLY path in the slim
// build. That boot-time detour is also why this file is worth keeping verbatim:
// plain Update.begin() caps an image at the free flash above the running
// sketch, which is well under the partition size.
#pragma once
#include <Arduino.h>
#include "Settings.h"

struct OtaLatest {
  bool   ok = false;      // the check itself succeeded
  bool   newer = false;   // the latest release is newer than FW_VERSION
  String tag;             // e.g. "v2.1.0"
  String url;             // firmware asset download URL
  String error;           // human-readable reason when ok == false
};

OtaLatest otaCheckLatest(const Settings& s);   // query the GitHub API (blocking)

// Update-at-boot flow. The request and the failure message live in LittleFS so
// they survive the reboot. All four are no-op stubs in the slim (NO_TLS) build.
bool   otaBootRequested();                     // a boot update is queued
bool   otaRequestBootUpdate(const char* tag);  // queue it (false if storage write failed)
void   otaBootUpdate(const Settings& s);       // consume the request; reboots on success
String otaTakeBootResult();                    // last boot attempt's error, "" if none
