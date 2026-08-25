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
static uint32_t    g_lastApClientMs = 0; // millis() a station was last seen on the AP

// Both written from the mDNS receive path, read from loop(). Single-byte stores
// on this chip, so no tearing — volatile only stops the compiler caching a read
// across the MDNS.update() that can set them.
static volatile bool g_mdnsTaken    = false;  // the probe came back: not our name
static volatile bool g_mdnsWithdraw = false;  // ...and netLoop still has to tear us down

// Deterministic 1/6/11 spread. Thirty units out of one box would otherwise all
// beacon on the core's default channel — ESP8266WiFiAP.h declares
// `softAP(const char* ssid, const char* psk = NULL, int channel = 1, ...)`, so
// leaving the channel out puts every one of them on 1, in one room, at once. That
// is an association problem long before it is a throughput one. Keying the choice
// off the chip id keeps a unit's channel stable across reboots, so a phone's
// saved-network entry keeps working, while spreading a batch of consecutive ids
// three ways for free. 1/6/11 are the non-overlapping trio and are legal in every
// regulatory domain, which 12/13 are not.
static const uint8_t kApChannels[3] = { 1, 6, 11 };

// The mDNS probe finished. On failure another responder on this link already
// answers to our hostname, and the library does NOT rename: _cancelProbingForHost
// stops probing for good and MDNSResponder::indexDomain has no callers, so the
// name is simply never ours. Two things follow.
//
// We report rather than rename, because a rename would make the address printed
// on the commissioning screen a lie — and that screen already carries the IP,
// which a collision cannot touch. netMdnsTaken() lets the panel drop the .local
// line and the portal print its daemon command with the IP instead.
//
// We also have to withdraw, and that part is not optional: installing this
// callback at all changes library behaviour, because _cancelProbingForHost only
// walks the services to cancel them when NO host callback is set. Left alone we
// would keep announcing _clawdmeter._tcp with an SRV target pointing at a name
// somebody else owns — which is precisely how a stranger's daemon ends up pushing
// to the wrong unit. It cannot happen inline: this runs from the UDP receive
// path, inside the parser still walking the structures MDNS.close() frees.
//
// Set-only, never cleared here: startMdns() clears both flags before MDNS.begin(),
// which is the only moment a fresh verdict is possible. `g_mdnsTaken = !ok` read
// better and was wrong. close() frees the hostname but leaves the lwIP
// status-change callback that begin() installed (m_bLwipCb is never reset), so the
// next netif event — a DHCP renew, a reconnect nudge, the AP rebooting — schedules
// _restart(), which sets ProbingStatus_ReadyToStart again. _sendHostProbe then
// fails on the NULL hostname but still counts, and after MDNS_PROBE_COUNT tries
// _updateProbeStatus takes its "Probing finished" branch and calls us with
// ok == true. The collision would un-report itself about a second after any WiFi
// flap, silently, and the portal would go back to printing a name we no longer
// answer to.
static void onMdnsProbe(const char* name, bool ok) {
  (void)name;
  if (!ok) {
    g_mdnsTaken    = true;
    g_mdnsWithdraw = true;
  }
}

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
  g_mdnsTaken = false;
  g_mdnsWithdraw = false;
  if (!MDNS.begin(g_hostname.c_str())) return;
  // After begin and before addService. Probing only advances inside MDNS.update()
  // (called from netLoop), so this is in time to see the very first result, and
  // the library never drops it again — stcProbeInformation::clear defaults to
  // leaving the user data alone, so a restart keeps the callback.
  MDNS.setHostProbeResultCallback(onMdnsProbe);
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
  // The channel is the THIRD argument, so the open path has to name a passphrase
  // to reach it. nullptr is the core's own open form (psk_len 0 => AUTH_OPEN); ""
  // reaches the same branch but is one typo away from a hotspot that silently
  // demands a password nobody was handed.
  uint8_t ch = kApChannels[platformChipId() % 3];
  bool up;
  if (s.apPass.length() >= 8) {
    up = WiFi.softAP(s.apSsid.c_str(), s.apPass.c_str(), ch);
  } else {
    up = WiFi.softAP(s.apSsid.c_str(), nullptr, ch);   // open AP (WPA2 needs >=8 chars)
  }
  g_apSsid = s.apSsid;

  // softAP refuses a name that is empty or over 32 characters — and a passphrase
  // over 64 — by returning false and bringing up NOTHING. Reaching startAP means
  // the station attempt already failed, so that leaves a unit with no radio anyone
  // can reach, and this board has no download mode: the recovery is opening the
  // case and soldering. That is the same hazard the /update guards in Web.cpp
  // exist for, and it is just as easy to trigger, because nothing clamps apSsid on
  // the way in — the portal's Name field has no maxlength and settingsApplyJson
  // takes the string as given, so clearing that one field is enough. Come back up
  // on the factory name, which is per-unit and so still unique in a room of these,
  // and keep the passphrase only if the core would have taken it.
  if (!up) {
    char unitId[5];
    snprintf(unitId, sizeof(unitId), "%04x", (unsigned)(platformChipId() & 0xFFFF));
    g_apSsid = String(DEFAULT_AP_SSID) + "-" + unitId;
    bool keepPass = s.apPass.length() >= 8 && s.apPass.length() <= 64;
    WiFi.softAP(g_apSsid.c_str(), keepPass ? s.apPass.c_str() : nullptr, ch);
  }

  // Captive portal: answer every DNS query with our own IP.
  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(53, "*", apIP);
  g_lastApRetry = millis();        // the retry clock starts when the AP comes up
  g_lastApClientMs = g_lastApRetry;  // ...and so does the quiet period below it
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
    // Stop the moment the SDK says the attempt is dead rather than merely slow.
    // wifi_station_get_connect_status tells those apart, and WL_CONNECTED means
    // GOT_IP, so the budget has to cover association + handshake + DHCP. Shortening
    // the budget instead (this used to clamp AP-mode retries to 6 s) punished the
    // honest slow join: a unit with CORRECT credentials whose router needs longer
    // than the clamp never joins, retry after retry, forever, and the only thing
    // its owner can see is a setup screen that says nothing is wrong. Reading the
    // status gets the case that clamp was written for — a mistyped password — out
    // in about a second (see the grace period below), still far better than the
    // clamp managed, and leaves a slow network its full window on a retry as well
    // as at boot.
    //
    // The grace period is not padding. WiFi.begin() ends in wifi_station_connect()
    // and immediately returns status(), and the SDK does not promise that status
    // has been reset by then — so the failure the PREVIOUS candidate left behind
    // can still be readable here. Acting on it would skip this candidate without
    // ever attempting it, which is worse than the slow join we are fixing: the
    // second saved network would never be tried after the first one had a bad
    // password. A real auth rejection needs the handshake and cannot arrive inside
    // a second anyway, so the wait costs nothing.
    uint32_t start = millis();
    for (;;) {
      wl_status_t st = WiFi.status();
      if (st == WL_CONNECTED) break;
      uint32_t waited = millis() - start;
      if (waited >= budget) break;
      if (waited > 1000 &&
          (st == WL_WRONG_PASSWORD || st == WL_CONNECT_FAILED ||
           st == WL_NO_SSID_AVAIL)) break;   // definitive: the budget buys nothing
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
    uint8_t clients = WiFi.softAPgetStationNum();
    if (clients) g_lastApClientMs = millis();
    // Two guards, both load-bearing.
    //
    // wifiCount: a factory-fresh giveaway unit has no saved network, so it never
    // retries and its hotspot is permanent. That is the whole normal path for a
    // batch handed to strangers — do not "simplify" the check away.
    //
    // The quiet period, not just "no station attached right now": a phone drops
    // off an internet-less hotspot the moment its screen locks, which the
    // instantaneous count reads as nobody being here. Yanking the portal out from
    // under someone halfway through retyping a WiFi password is the one failure
    // this device cannot talk its way out of, because there is no printed card to
    // fall back on.
    if (g_cfg && g_cfg->wifiCount && !clients &&
        millis() - g_lastApRetry > AP_RETRY_MS &&
        millis() - g_lastApClientMs > AP_QUIET_MS) {
      apRetrySta();
    }
    return;
  }
  // STA: keep mDNS alive, nudge reconnect if we dropped. After a long outage
  // rotate through the other saved networks. Never scan here — it would block
  // the display loop and the web server; WiFi.begin is fire-and-forget and its
  // status is picked up on later passes.
  if (g_mdnsWithdraw) {
    g_mdnsWithdraw = false;
    MDNS.close();   // goodbye + drop the services; see onMdnsProbe for why here
  }
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
bool    netMdnsTaken() { return g_mdnsTaken; }

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
