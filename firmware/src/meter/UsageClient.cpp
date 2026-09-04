#include "UsageClient.h"
#include "Platform.h"
#include <ArduinoJson.h>
#include <math.h>

// Defined in main.cpp — the first payload of a unit's life retires the
// commissioning screen. Only ever sets a RAM flag; loop() does the flash write.
extern void appMarkCommissioned();

static UsageData g_usage;
static uint32_t  g_nextPollMs = 0;
static bool      g_inited = false;

// ---------------------------------------------------------------------------
void usageInit(const Settings& s) {
  (void)s;
  // A board restored from flash outlives this. usageInit runs from two places
  // that are not "start clean" — the pull path's lazy init on first service, and
  // invalidate() after a settings POST — and clearing in either dropped the file
  // BoardStore had just read. The one caller that genuinely wants an empty
  // snapshot is a mode's begin(), which no longer calls this at all.
  if (!g_usage.restored) g_usage.clear();
  g_nextPollMs = millis();
  g_inited = true;
}

void usageForceRefresh() { g_nextPollMs = millis(); }

const UsageData& usageGet() { return g_usage; }

bool usageFresh(uint32_t withinMs) {
  // `restored` is the load-bearing half. A board read back from flash is valid
  // and lastOkMs is the millis() of the restore, which at boot is a handful of
  // seconds — so without this test the device would call a board from last night
  // fresh for the whole of its first stale window and draw it as live.
  return g_usage.valid && !g_usage.restored && (millis() - g_usage.lastOkMs) <= withinMs;
}

uint32_t usageStaleMs(const Settings& s) {
  if (s.usage.usageUrl.length() >= 8)
    return (uint32_t)s.usage.pollSec * 1000UL * 2UL + USAGE_STALE_GRACE_MS;
  if (g_usage.heartbeatSec) {
    uint32_t w = (uint32_t)g_usage.heartbeatSec * 1000UL * PUSH_STALE_BEATS
               + USAGE_STALE_GRACE_MS;
    return constrain(w, (uint32_t)PUSH_STALE_MIN_MS, (uint32_t)PUSH_STALE_MS);
  }
  return PUSH_STALE_MS;
}

// ---- parse: usage contract -------------------------------------------------
// { "s":29, "sr":142, "w":4, "wr":9876, "st":"allowed", "ok":true,
//   "sess":[{"n":"stoplicht-72","s":"w","t":14}], "ns":4,
//   "ts":1788538615, "tzo":120, "hb":15, "p":8788 }
//   s  = 5h utilization %        sr = minutes until 5h reset
//   w  = 7d utilization %        wr = minutes until 7d reset
//   st = rate-limit status       ok = false => explicit "no data"
//   sess = session board rows, already sorted and clipped by the sender:
//          n = name, s = "w"orking / "b"locked / "a"waiting, t = minutes
//   ns = live sessions on the host (>= sess length when the board overflows)
//   ts = sender's UTC epoch seconds   tzo = its local offset, minutes
//   hb = sender's heartbeat, seconds  p   = sender's listener port
//
// EVERY key is optional except one of {s, sess}, and that is load-bearing rather
// than lax. Three generations of sender have to work against one firmware: the
// upstream Python daemon (s/sr/w/wr only), the hook reporter (sess/ns only), and
// the agent (all of it). A payload missing a key parses as "not declared" and the
// device falls back to a conservative default, so an older sender is never worse
// off than it was — see usageStaleMs for the one that matters.
static void usageFilter(JsonDocument& f) {
  f["s"] = true; f["sr"] = true; f["w"] = true;
  f["wr"] = true; f["st"] = true; f["ok"] = true;
  f["ns"] = true;
  // Sender metadata. A filter drops every key it does not name, so a key added to
  // the contract and not added here parses as absent — silently, and only on the
  // device. ts/tzo = the sender's clock, hb = its heartbeat, p = its listener.
  f["ts"] = true; f["tzo"] = true; f["hb"] = true; f["p"] = true;
  JsonObject row = f["sess"].add<JsonObject>();
  row["n"] = true; row["s"] = true; row["t"] = true;
}

static uint8_t sessionState(const char* code) {
  switch (code ? code[0] : 'a') {
    case 'w': return SESS_WORKING;
    case 'b': return SESS_BLOCKED;
    default:  return SESS_AWAITING;
  }
}

static void applyBoard(UsageData& d, JsonDocument& doc) {
  d.sessionRows = 0;
  d.sessionLive = 0;
  d.boardValid  = false;
  if (!doc["sess"].is<JsonArrayConst>()) return;   // pre-board daemon

  d.boardValid = true;
  for (JsonObjectConst row : doc["sess"].as<JsonArrayConst>()) {
    if (d.sessionRows >= MAX_SESSION_ROWS) break;
    SessionInfo& si = d.sessions[d.sessionRows];
    strlcpy(si.name, row["n"] | "?", sizeof(si.name));
    si.state = sessionState(row["s"] | "a");
    si.mins  = (uint16_t)constrain((long)(row["t"] | 0L), 0L, 65535L);
    d.sessionRows++;
  }
  // A board that overflowed still reports the true count; fall back to the rows
  // we got so the header can never read fewer than what is drawn under it.
  d.sessionLive = (uint8_t)constrain((long)(doc["ns"] | (long)d.sessionRows),
                                     (long)d.sessionRows, 255L);
}

static bool applyUsageDoc(UsageData& d, JsonDocument& doc) {
  if (doc["ok"].is<bool>() && doc["ok"].as<bool>() == false) return false;

  // Either half is enough. A sender with rate limits but no board is the older
  // contract; a sender with a board but no rate limits is the plugin's hook,
  // which runs in every Claude Code entrypoint but is never given them. Requiring
  // "s" rejected the second with a 400 and left the panel on "waiting...".
  const bool hasUsage = doc["s"].is<float>() || doc["s"].is<int>();
  const bool hasBoard = doc["sess"].is<JsonArrayConst>();
  if (!hasUsage && !hasBoard) return false;

  if (hasUsage) {
    d.sessionPct      = constrain(doc["s"].as<float>(), 0.0f, 100.0f);
    d.weeklyPct       = constrain(doc["w"] | 0.0f, 0.0f, 100.0f);
    d.sessionResetMin = doc["sr"] | 0;
    d.weeklyResetMin  = doc["wr"] | 0;
    strlcpy(d.status, doc["st"] | "", sizeof(d.status));
    d.usageValid = true;
  }
  applyBoard(d, doc);

  // All four optional, and the last payload wins. A hook-only reporter carries
  // none of them, which drops the stale window back to the conservative
  // PUSH_STALE_MS — the forgiving direction, and the right one for a sender that
  // only speaks when something happens.
  d.stampEpoch    = doc["ts"] | 0UL;
  d.stampTzOffMin = (int16_t)constrain((long)(doc["tzo"] | 0L), -1440L, 1440L);
  d.heartbeatSec  = (uint16_t)constrain((long)(doc["hb"] | 0L), 0L, 3600L);
  d.senderPort    = (uint16_t)constrain((long)(doc["p"] | 0L), 0L, 65535L);

  d.valid = true;
  d.error = false;
  d.restored = false;      // whatever this is, it arrived
  d.lastOkMs = millis();
  // The one point both paths pass through — pushed via usageApply (Web.cpp) and
  // pulled via parseUsage — so hooking it here is what makes a pull-mode unit
  // leave the commissioning screen too.
  appMarkCommissioned();
  return true;
}

static bool parseUsage(UsageData& d, Stream& stream) {
  JsonDocument filter; usageFilter(filter);
  JsonDocument doc;
  if (deserializeJson(doc, stream, DeserializationOption::Filter(filter))) return false;
  return applyUsageDoc(d, doc);
}

// Pushed payload (POST /api/usage): same contract, parsed from a String body.
bool usageApply(const String& body) {
  JsonDocument filter; usageFilter(filter);
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return false;
  return applyUsageDoc(g_usage, doc);
}

// The same parse for the copy that survived the reboot. applyUsageDoc clears
// `restored` because everything it normally parses did just arrive, so the flag
// goes on afterwards — and it is what stops usageFresh() from calling last
// night's board live.
bool usageApplyRestored(const String& body) {
  if (!usageApply(body)) return false;
  g_usage.restored = true;
  return true;
}

// ---- one HTTP GET + parse (only used when a pull URL is configured) --------
static bool fetchUsage(const Settings& s) {
  const String& url = s.usage.usageUrl;
  if (url.length() < 8) return false;
  bool https = url.startsWith("https://");

  std::unique_ptr<NetClient> client;
  if (https) {
#if WITH_TLS
    if (ESP.getFreeHeap() < 20000) return false;   // too little heap for TLS (incl. the 9 KB thunk); skip, don't crash
    client.reset(platformMakeSecureClient(2048));   // LAN / self-hosted endpoint
#else
    // Slim build: no TLS stack. Refusing here is what keeps BearSSL out of the
    // link — the agent pushes over plain HTTP on the LAN anyway, so this only
    // ever trips on a hand-entered https:// pull URL.
    return false;
#endif
  } else {
    client.reset(new WiFiClient());
  }

  HTTPClient http;
  http.setTimeout(s.httpTimeout);
  http.setReuse(false);
  if (!http.begin(*client, url)) return false;
  http.addHeader("Accept", "application/json");

  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  bool ok = parseUsage(g_usage, http.getStream());
  http.end();
  return ok;
}

// ---------------------------------------------------------------------------
void usageService(const Settings& s) {
  if (!g_inited) usageInit(s);
  if ((int32_t)(millis() - g_nextPollMs) < 0) return;

  if (!fetchUsage(s)) g_usage.error = true;   // keep stale data, flag the error

  g_nextPollMs = millis() + (uint32_t)s.usage.pollSec * 1000UL;
}
