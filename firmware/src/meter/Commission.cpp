#include "Commission.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Net.h"
#include "Palette.h"
#include "Fmt.h"

// One tick a second: the dots advance and the IP is re-read. Everything else in
// commissionService is a millis() compare, and that matters — loop() calls in
// here ~200 times a second while a unit is uncommissioned.
#define COMMISSION_TICK_MS 1000UL

// The animated line's band, cleared and redrawn on its own. This is the
// gfxSplashCaption pattern (Gfx.cpp): a fillScreen once a second here would be
// exactly the flicker the rest of this release exists to remove.
#define WAIT_BAND_Y   206
#define WAIT_BAND_H    20
#define WAIT_TEXT_Y   208
// x of the WIDEST state ("Waiting for data..." = 19 x 6 x 2 = 228 px), held fixed
// rather than recentred per frame so the text does not shuffle sideways as the
// dots come and go.
#define WAIT_TEXT_X     6

static bool     s_primed = false;    // the screen is ours and up to date
static uint8_t  s_dots   = 0;        // 0..3
static uint32_t s_tickAt = 0;
static char     s_ip[16] = "";       // address currently drawn; empty = none
// Part of the drawn state, not a live read: drawFull is the only place that
// consults netMdnsTaken(), and it cannot see the answer on the first paint. The
// probe is only scheduled by MDNS.begin() and advances inside MDNS.update(), so
// the conflict lands hundreds of milliseconds after this screen is already up —
// and with the address unchanged there was nothing left to trigger a repaint.
// The name would have stayed on the glass for good, pointing at the stranger's
// unit that won the race, which is the one failure this guard exists to prevent.
static bool     s_mdnsTaken = false; // netMdnsTaken() as of the last full paint

void commissionInvalidate() { s_primed = false; }

static void drawWaiting() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  // The dots are trimmed off the end of the full string rather than appended, so
  // there is only ever one literal and one length to keep straight.
  static const char kWait[] = "Waiting for data...";
  char line[sizeof(kWait)];
  strlcpy(line, kWait, sizeof(line));
  line[sizeof(kWait) - 4 + s_dots] = '\0';

  gfx->fillRect(0, WAIT_BAND_Y, TFT_WIDTH, WAIT_BAND_H, C_BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(WAIT_TEXT_X, WAIT_TEXT_Y);
  gfx->print(line);
}

// Full paint. Every row and its width against the 232 px content budget:
//    10  ALMOST DONE               3   11 x 6 x 3 = 198
//    58  In Claude Code, run       1   19 x 6 x 1 = 114
//    74  /clawd:setup a1b2         2   17 x 6 x 2 = 204
//   106  or set it up at           1   15 x 6 x 1 =  90
//   120  <host>.local              2   16 x 6 x 2 = 192
//   142  <ip>                      2   15 x 6 x 2 = 180  (longest possible IPv4)
//   208  Waiting for data...       2   19 x 6 x 2 = 228
// `addr` is null when the station is not associated.
static void drawFull(const Settings& s, const char* addr) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);

  gfxDrawCentered("ALMOST DONE", 10, 3, C_ACCENT);
  gfx->fillRect(8, 44, 224, 2, C_BARBG);

  // The whole remaining step, as the literal thing to type. This used to be three
  // rows of prose about "a program on your computer" that sent the reader to the
  // portal to find out what that program was — true when the sender was a Python
  // daemon nobody could name, and now just a detour: the sender is the clawd
  // plugin and its setup command fits on one row at size 2, code included.
  //
  // The code, not the hostname, because that is the argument /clawd:setup asks
  // for; the address below is for anyone who would rather look than type.
  // No mascot: its meaning is "your laptop is asleep", which is the opposite of
  // what this screen says, and it would cost the rows below it.
  char code[8];
  fmtDeviceCode(s.hostname.c_str(), code, sizeof(code));
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "/clawd:setup %s", code);
  gfxDrawCentered("In Claude Code, run", 58, 1, C_DIM);
  gfxDrawCentered(cmd, 74, gfxFitSize(cmd, 232, 2), C_WHITE);

  if (!addr) {
    // Not associated, so there is no address worth typing. Printing 0.0.0.0 as
    // the thing to open is worse than saying nothing — it reads as an instruction.
    gfxDrawCentered("Rejoining WiFi", 120, 2, C_ACCENT);      // 14 x 6 x 2 = 168
  } else {
    // Both addresses, because their failure modes are opposite: a stale IP after
    // a DHCP renew fails SILENTLY (the sender posts into the void), while an
    // unresolvable .local fails immediately and loudly. Showing both means one of
    // the two is always diagnosable. The name goes first because it survives a
    // lease change; it is also the line that gets dropped when a hand-set
    // hostname is too long for even 6x8, since the IP always fits.
    // netMdnsTaken(): another responder on this link already owns the name, and
    // ours has withdrawn its service record rather than renamed itself — so
    // <host>.local now resolves ONLY to the stranger's unit. Printing it would
    // aim the recipient's sender at somebody else's screen, which is worse than
    // printing no name at all. The IP-only layout below already handles it.
    gfxDrawCentered("or set it up at", 106, 1, C_DIM);
    char url[64];
    snprintf(url, sizeof(url), "%s.local", s.hostname.c_str());
    uint8_t sz = (s.hostname.length() && !netMdnsTaken())
                   ? gfxFitSizeMin(url, 232, 2, 1) : 0;
    if (sz) {
      gfxDrawCentered(url, 120, sz, C_UGREEN);
      // Same size as the name above it, deliberately. The .local lookup is the
      // one that fails on a network with mDNS blocked — measured on the author's
      // own AP — so demoting the address that always works would be backwards.
      gfxDrawCentered(addr, 142, gfxFitSize(addr, 232, 2), C_UGREEN);
    } else {
      gfxDrawCentered(addr, 120, gfxFitSize(addr, 232, 2), C_UGREEN);
    }
  }

  gfx->fillRect(8, 176, 224, 2, C_BARBG);
  drawWaiting();
}

void commissionService(const Settings& s) {
  if (!gfxDev()) return;

  // The early return is the point: netIP() returns a String, and building and
  // freeing one of those on every pass of loop() is exactly the churn this chip
  // cannot afford. On the other ~199 passes a second this is one comparison.
  uint32_t now = millis();
  if (s_primed && (now - s_tickAt) < COMMISSION_TICK_MS) return;
  s_tickAt = now;

  String ipStr = netIP();
  const char* ip = ipStr.c_str();
  const bool up = netConnected() && strcmp(ip, "0.0.0.0") != 0;
  const char* addr = up ? ip : "";

  // Full repaint on first entry, whenever the drawn address changes, and whenever
  // the mDNS verdict changes. Not tidiness: after a DHCP renew this screen is the
  // only place the new address exists, and nobody is watching it to notice.
  // Tracking the drawn string rather than the raw IP makes the associated/not
  // transition fall out of the same comparison. The mDNS term is the one that
  // arrives LATE — see s_mdnsTaken above.
  const bool taken = netMdnsTaken();
  if (!s_primed || strcmp(addr, s_ip) != 0 || taken != s_mdnsTaken) {
    strlcpy(s_ip, addr, sizeof(s_ip));
    s_mdnsTaken = taken;
    s_dots = 0;
    drawFull(s, s_ip[0] ? s_ip : nullptr);
    s_primed = true;
    return;
  }

  s_dots = (uint8_t)((s_dots + 1) & 3);
  drawWaiting();
}
