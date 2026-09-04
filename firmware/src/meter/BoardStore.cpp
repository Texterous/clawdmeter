#include "BoardStore.h"
#include "UsageClient.h"
#include "Net.h"
#include <LittleFS.h>
#include <ESP8266WiFi.h>

// Two lines, not JSON: "<ip>:<port>" then the payload verbatim. The body is
// already JSON and escaping it into a second JSON document to store it would cost
// a parse, an escape pass and the heap for both, on a chip where the alternative
// is readStringUntil('\n'). The payload contract is the file format.
static const char* BOARD_PATH = "/board.json";

static String   g_body;          // last payload accepted, as delivered
static String   g_senderIp;      // learned from the POST's source address
static uint16_t g_senderPort = 0;
static bool     g_dirty      = false;
static bool     g_wrote      = false;   // this boot has flushed at least once
static uint32_t g_lastWrite  = 0;
static uint32_t g_lastPoke   = 0;
static bool     g_pokedBoot  = false;

// ---------------------------------------------------------------------------
bool boardStoreBegin() {
  File f = LittleFS.open(BOARD_PATH, "r");
  if (!f) return false;

  String head = f.readStringUntil('\n');
  String body = f.readString();
  f.close();
  head.trim();

  int c = head.lastIndexOf(':');
  if (c > 0) {
    g_senderIp   = head.substring(0, c);
    g_senderPort = (uint16_t)head.substring(c + 1).toInt();
  }
  if (!g_senderPort) { g_senderIp = ""; g_senderPort = 0; }

  body.trim();
  if (body.length() < 2) return false;
  g_body = body;
  // Restored, not received: usageApplyRestored is what keeps this out of
  // usageFresh(), so the screen shows the board and says it is history.
  return usageApplyRestored(body);
}

void boardStoreNote(const String& body, const String& senderIp, uint16_t senderPort) {
  if (senderIp.length() && senderPort) {
    if (senderIp != g_senderIp || senderPort != g_senderPort) {
      g_senderIp   = senderIp;
      g_senderPort = senderPort;
      g_dirty      = true;      // the address is half of what the file is for
    }
  }
  if (body == g_body) return;
  // A body longer than the panel can ever show is a sender bug or a probe, not a
  // board. Refusing to persist it protects the one copy that boots.
  if (body.length() > BOARD_STORE_MAX) return;
  g_body = body;
  g_dirty = true;
}

static void flush() {
  File f = LittleFS.open(BOARD_PATH, "w");
  if (!f) return;
  if (g_senderIp.length() && g_senderPort) {
    f.print(g_senderIp); f.print(':'); f.println(g_senderPort);
  } else {
    f.println();
  }
  f.print(g_body);
  f.close();
  g_dirty = false;
  g_wrote = true;
  g_lastWrite = millis();
}

// ---------------------------------------------------------------------------
void boardStorePoke() {
  if (!g_senderPort || !g_senderIp.length()) return;
  if (!netConnected()) return;

  IPAddress ip;
  if (!ip.fromString(g_senderIp)) return;

  // Raw client, no HTTPClient: this is one line out and nothing back. The reply
  // is a push to /api/usage arriving on its own, so reading the response would
  // only pay for a body we throw away. setTimeout bounds the connect, which
  // otherwise blocks for five seconds against an address that has gone away.
  WiFiClient c;
  c.setTimeout(BOARD_POKE_TIMEOUT_MS);
  if (!c.connect(ip, g_senderPort)) { c.stop(); return; }
  c.print(F("GET /refresh HTTP/1.1\r\nHost: "));
  c.print(g_senderIp);
  c.print(F("\r\nUser-Agent: clawdmeter\r\nConnection: close\r\n\r\n"));
  c.stop();
}

void boardStoreService(const Settings& s) {
  // Persist: first change of a boot goes straight down so a unit paired a minute
  // ago survives being unplugged; after that one write per BOARD_PERSIST_MIN_MS.
  // The file is only ever read at boot, so a board a few minutes behind is still
  // the right thing to have on flash — and flash is the one part of this device
  // that wears out.
  if (g_dirty && (!g_wrote || millis() - g_lastWrite >= BOARD_PERSIST_MIN_MS)) flush();

  if (usageFresh(usageStaleMs(s))) return;   // data is arriving; nothing to ask for

  // Nothing recent. Ask once as soon as there is a network — that is the boot
  // case, and it is the difference between a second of history on screen and a
  // whole heartbeat of it — then keep asking slowly, which is what picks a
  // sleeping laptop back up without waiting for it to notice us.
  if (!g_pokedBoot) {
    if (!netConnected()) return;
    g_pokedBoot = true;
    g_lastPoke  = millis();
    boardStorePoke();
    return;
  }
  if (millis() - g_lastPoke < BOARD_POKE_IDLE_MS) return;
  g_lastPoke = millis();
  boardStorePoke();
}

bool   boardStoreHasSender() { return g_senderPort && g_senderIp.length(); }

String boardStoreSender() {
  if (!boardStoreHasSender()) return String();
  return g_senderIp + ":" + String(g_senderPort);
}

void boardStoreForget() {
  LittleFS.remove(BOARD_PATH);
  g_body = ""; g_senderIp = ""; g_senderPort = 0; g_dirty = false;
}
