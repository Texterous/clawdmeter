#include "Net.h"
#include "Platform.h"
#include <DNSServer.h>

static NetMode     g_mode = NET_AP;
static DNSServer   g_dns;
static String      g_hostname;
static String      g_apSsid;
static uint32_t    g_lastReconnect = 0;
static const Settings* g_cfg = nullptr;  // for runtime failover between saved networks
static int8_t      g_curNet = -1;        // settings index of the joined network
static uint32_t    g_downSince = 0;      // 0 = connected; else millis() the drop began
static uint32_t    g_lastApRetry = 0;    // millis() of the last station retry from AP mode

// mDNS, plus the service record the daemon discovers us by. Called on every
// promotion to station mode, not only the boot one: a device that reaches the
// network without announcing itself is online but invisible to discovery, which
// is the failure that costs an afternoon rather than a minute.
//
// The service registration used to sit behind `#if WITH_USAGE`, a macro this
// project never defines — so the preprocessor evaluated it as 0, silently, and
// no image ever built here advertised `_clawdmeter._tcp` at all. There is no
// usage-less build of this firmware (config.h: one screen, one job), so the
// guard is gone rather than fixed.
static void startMdns() {
  if (!MDNS.begin(g_hostname.c_str())) return;
  MDNS.addService("http", "tcp", 80);
  // Discoverable usage-push service so the clawdmeter daemon can find and push
  // to every SmallTV on the LAN (no hardcoded host). TXT carries the device id,
  // firmware version, and the push path.
  MDNS.addService("clawdmeter", "tcp", 80);
  MDNS.addServiceTxt("clawdmeter", "tcp", "id",   g_hostname.c_str());
  MDNS.addServiceTxt("clawdmeter", "tcp", "ver",  FW_VERSION);
  MDNS.addServiceTxt("clawdmeter", "tcp", "path", "/api/usage");
}

static void startAP(const Settings& s) {
  g_mode = NET_AP;
  WiFi.mode(WIFI_AP);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  if (s.apPass.length() >= 8) {
    WiFi.softAP(s.apSsid.c_str(), s.apPass.c_str());
  } else {
    WiFi.softAP(s.apSsid.c_str());           // open AP (WPA2 needs >=8 chars)
  }
  g_apSsid = s.apSsid;
  // Captive portal: answer every DNS query with our own IP.
  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(53, "*", apIP);
  g_lastApRetry = millis();   // the retry clock starts when the AP comes up
}

// Tear the AP down before a station attempt. g_dns.start() rebinds its socket
// every call, so the server has to be stopped rather than left running.
static void stopAP() {
  g_dns.stop();
  WiFi.softAPdisconnect(true);
}

// Try the saved networks once, ordered by what a scan can actually see: visible
// networks first, strongest first, then the rest in config order. On success
// sets g_curNet/g_mode and returns true; the caller starts mDNS.
//
// `probeUnseen` decides what happens to the networks the scan did not find. At
// boot we still try them, because a hidden SSID never appears in a scan. On an
// AP-mode retry we skip them: the captive portal is down for the duration, and
// an 8 s probe per out-of-range network is not worth that outage.
static bool staConnect(const Settings& s, void (*onProgress)(const char*),
                       bool probeUnseen) {
  WiFi.mode(WIFI_STA);

  uint8_t order[MAX_WIFI_NETS];
  bool    seen[MAX_WIFI_NETS];
  if (s.wifiCount == 1) {
    order[0] = 0;
    seen[0] = true;
  } else {
    int32_t rssi[MAX_WIFI_NETS];
    for (uint8_t i = 0; i < s.wifiCount; i++) { rssi[i] = -32768; seen[i] = false; }
    if (onProgress) onProgress("Scanning...");
    int found = WiFi.scanNetworks();
    for (int a = 0; a < found; a++)
      for (uint8_t i = 0; i < s.wifiCount; i++)
        if (WiFi.SSID(a) == s.wifi[i].ssid && WiFi.RSSI(a) > rssi[i]) {
          rssi[i] = WiFi.RSSI(a);
          seen[i] = true;
        }
    WiFi.scanDelete();

    // A scan that comes back empty (0, or WIFI_SCAN_FAILED, which a scan issued
    // right after a mode change readily returns) is absence of information, not
    // evidence of absence. Left alone it marks every network unseen, which
    // halves each connect budget below and skips them entirely on a retry —
    // least patience exactly when we know least. Assume all visible instead.
    if (found <= 0) for (uint8_t i = 0; i < s.wifiCount; i++) seen[i] = true;

    bool used[MAX_WIFI_NETS] = {false};
    for (uint8_t k = 0; k < s.wifiCount; k++) {
      int best = -1;
      for (uint8_t i = 0; i < s.wifiCount; i++) {
        if (used[i]) continue;
        if (best < 0 ||
            (seen[i] && !seen[best]) ||
            (seen[i] == seen[best] && rssi[i] > rssi[best])) best = i;
      }
      used[best] = true;
      order[k] = (uint8_t)best;
    }
  }

  for (uint8_t k = 0; k < s.wifiCount; k++) {
    if (!seen[order[k]] && !probeUnseen) continue;
    const WifiCred& n = s.wifi[order[k]];
    if (onProgress) {
      char msg[48];
      snprintf(msg, sizeof(msg), "WiFi: %s", n.ssid.c_str());
      onProgress(msg);
    }
    WiFi.begin(n.ssid.c_str(), n.pass.c_str());

    uint32_t budget = seen[order[k]] ? WIFI_JOIN_SEEN_MS : WIFI_JOIN_UNSEEN_MS;
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < budget) {
      delay(200);
      yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
      g_curNet = (int8_t)order[k];
      g_mode = NET_STA;
      g_downSince = 0;
      g_lastReconnect = millis();
      return true;
    }
    WiFi.disconnect();
    delay(100);
  }
  return false;
}

void netBegin(const Settings& s, void (*onProgress)(const char*)) {
  g_cfg = &s;
  g_hostname = s.hostname.length() ? s.hostname : String(DEFAULT_HOSTNAME);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  platformSetHostname(g_hostname.c_str());

  if (s.wifiCount == 0) {
    if (onProgress) onProgress("No WiFi saved");
    startAP(s);
    return;
  }

  // Blocking is fine here: only the boot screen is up.
  if (staConnect(s, onProgress, /*probeUnseen=*/true)) {
    startMdns();
    if (onProgress) onProgress(WiFi.localIP().toString().c_str());
    return;
  }

  if (onProgress) onProgress("WiFi failed -> AP");
  startAP(s);
}

// AP mode is a waiting room, not a destination: retry the saved networks, and
// bring the portal back if they are still out of reach. Without this a single
// failed association at boot is permanent until someone power-cycles the unit —
// and for whoever is holding it, the recovery action is "unplug it", which they
// have no way of knowing.
//
// Promotion needs no further plumbing: the web server is bound to every
// interface, and main's loop() re-reads netMode() on each pass, so the display
// path takes over and repaints over the AP screen on its own.
static void apRetrySta() {
  stopAP();
  if (staConnect(*g_cfg, nullptr, /*probeUnseen=*/false)) startMdns();
  else                                                    startAP(*g_cfg);
}

void netLoop() {
  if (g_mode == NET_AP) {
    g_dns.processNextRequest();
    // Only while nobody is connected to the AP: mid-setup, the portal must not
    // be yanked out from under them. With no client attached, the AP going away
    // for a few seconds is unobservable.
    if (g_cfg && g_cfg->wifiCount &&
        millis() - g_lastApRetry > AP_RETRY_MS &&
        WiFi.softAPgetStationNum() == 0) {
      apRetrySta();
    }
    return;
  }
  // STA: keep mDNS alive, nudge reconnect if we dropped. After a long outage
  // rotate through the other saved networks. Never scan here — it would block
  // the display loop and the web server; WiFi.begin is fire-and-forget and its
  // status is picked up on later passes.
  platformMdnsUpdate();
  if (WiFi.status() == WL_CONNECTED) {
    g_downSince = 0;
    return;
  }
  if (!g_downSince) g_downSince = millis();
  // WiFi.reconnect() is a disconnect followed by a connect, so nudging faster
  // than an association can complete tears down attempts that were still in
  // progress — a loaded AP can need well over ten seconds for auth plus DHCP.
  // The SDK's own auto-reconnect is already retrying underneath; this is the
  // backstop for when that gives up, not the mechanism.
  if (millis() - g_lastReconnect > STA_NUDGE_MS) {
    g_lastReconnect = millis();
    if (g_cfg && g_cfg->wifiCount > 1 && millis() - g_downSince > STA_ROTATE_MS) {
      g_curNet = (int8_t)((g_curNet + 1) % g_cfg->wifiCount);
      WiFi.begin(g_cfg->wifi[g_curNet].ssid.c_str(), g_cfg->wifi[g_curNet].pass.c_str());
      g_downSince = millis();   // give this candidate its own window before rotating on
    } else {
      WiFi.reconnect();         // keep nudging the current network first
    }
  }
}

NetMode netMode()      { return g_mode; }
bool    netConnected() { return g_mode == NET_STA && WiFi.status() == WL_CONNECTED; }

String netIP() {
  return (g_mode == NET_AP) ? WiFi.softAPIP().toString()
                            : WiFi.localIP().toString();
}

String netSSID() {
  return (g_mode == NET_AP) ? g_apSsid : WiFi.SSID();
}

int netRSSI() {
  return (g_mode == NET_STA) ? WiFi.RSSI() : 0;
}
