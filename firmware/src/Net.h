// Net.h — WiFi station / fallback AP / captive portal / mDNS
#pragma once
#include <Arduino.h>
#include "Settings.h"

enum NetMode { NET_STA, NET_AP };

// Connects to the configured station; falls back to AP if that fails or no
// credentials are stored. `onProgress` (optional) is called with short status
// strings so the display can show what's happening during the boot connect.
void netBegin(const Settings& s, void (*onProgress)(const char*) = nullptr);
void netLoop();           // pump DNS (AP) / mDNS (STA) / reconnect

NetMode  netMode();
bool     netConnected();  // STA associated with an IP
String   netIP();         // current IP (STA or AP)
String   netSSID();       // joined SSID (STA) or AP SSID
int      netRSSI();       // STA signal, 0 in AP mode

// True once the mDNS probe came back claimed: another responder on this link
// already answers to <hostname>.local, so that name resolves to somebody else's
// device and only our IP is certainly ours. Callers show the IP instead of the
// name. Always false in AP mode, where no responder runs at all. See onMdnsProbe
// in Net.cpp for why the answer is "report it" rather than "rename".
bool     netMdnsTaken();
