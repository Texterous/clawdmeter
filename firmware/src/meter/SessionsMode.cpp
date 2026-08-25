#include "SessionsMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Fmt.h"
#include "Palette.h"
#include "UsageClient.h"
#include "Net.h"

SessionsMode g_sessionsMode;

// ---- layout ---------------------------------------------------------------
// The panel is 240x240 and the font is a 6x8 bitmap at integer scales, so every
// number here is a real pixel count rather than a proportion.
//
//   0..34    header: "SESSIONS" + the live count
//   44..176  rows
//   176..224 footer: the counts line and the 5h window bar
//
// Rows shrink from 30 px to 22 px past four sessions, which is what keeps the
// footer anchored at 176 for every board size: 4*30 = 120 and 6*22 = 132 both
// land at or under it. Anchoring the footer instead of stacking it after the
// rows is what stops a full board from drawing over its own summary.
static const int ROWS_TOP   = 44;
static const int FOOTER_TOP = 176;

static int rowHeight(uint8_t rows) { return rows <= 4 ? 30 : 22; }

static uint16_t stateColor(uint8_t state) {
  switch (state) {
    case SESS_BLOCKED: return C_RED;
    case SESS_WORKING: return C_UGREEN;
    default:           return C_AMBER;
  }
}

// Right-align on the 6x8 grid: gfxTextW is exact, so this lands on a pixel.
static void drawRight(Arduino_GFX* gfx, int right, int y, const char* s,
                      uint8_t size, uint16_t color) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(right - gfxTextW(s, size), y);
  gfx->print(s);
}

static void drawLeft(Arduino_GFX* gfx, int x, int y, const char* s,
                     uint8_t size, uint16_t color) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

// "1 BLOCKED  2 WAITING  1 WORKING", minus whatever is zero — each count in its
// own lamp colour rather than the whole line in grey. The row dots are the traffic
// light and this is its tally, so a red "1 BLOCKED" that reads from across the
// desk is the point of having a tally at all.
//
// Drawn as up to three segments advancing left to right. Widest case is three
// two-digit counts: 3 x (10 x 6) + 2 x 12 gaps = 204 px inside the 224 px content
// width, and a segment that would overrun is dropped whole rather than clipped.
static void drawSummary(Arduino_GFX* gfx, const UsageData& u, int y) {
  if (u.sessionLive > u.sessionRows) {   // board overflowed: say so rather than
    char line[24];                       // describing only the rows drawn
    snprintf(line, sizeof(line), "SHOWING %u OF %u",
             (unsigned)u.sessionRows, (unsigned)u.sessionLive);
    drawLeft(gfx, 8, y, line, 1, C_DIM);
    return;
  }

  // Ordered blocked, waiting, working — the same urgency order the rows are
  // sorted in, so the tally reads as a summary of the list above it.
  uint8_t count[3] = { 0, 0, 0 };
  for (uint8_t i = 0; i < u.sessionRows; i++) {
    switch (u.sessions[i].state) {
      case SESS_BLOCKED: count[0]++; break;
      case SESS_WORKING: count[2]++; break;
      default:           count[1]++; break;
    }
  }

  static const char* const kLabel[3] = { "BLOCKED", "WAITING", "WORKING" };
  const uint16_t color[3] = { C_RED, C_AMBER, C_UGREEN };
  int x = 8;
  for (uint8_t i = 0; i < 3; i++) {
    if (!count[i]) continue;
    char seg[16];
    snprintf(seg, sizeof(seg), "%u %s", (unsigned)count[i], kLabel[i]);
    int w = gfxTextW(seg, 1);
    if (x + w > 232) return;
    drawLeft(gfx, x, y, seg, 1, color[i]);
    x += w + 12;                          // two spaces of gap at size 1
  }
}

// One usage window: a label, its percentage right-aligned to the window's own
// right edge, and a bar under both. Two of these sit side by side in the footer,
// which is how both windows fit in the vertical space the single 5h bar used to
// have all to itself.
static const int WIN_W  = 104;   // 8..112 and 128..232, a 16 px gutter between
static const int WIN_2X = 128;
static void drawWindow(Arduino_GFX* gfx, int x, const char* label, float pctRaw) {
  const float pct = constrain(pctRaw, 0.0f, 100.0f);
  drawLeft(gfx, x, 202, label, 1, C_DIM);
  char pctStr[8];
  snprintf(pctStr, sizeof(pctStr), "%d%%", (int)lroundf(pct));
  drawRight(gfx, x + WIN_W, 202, pctStr, 1, C_DIM);

  const int by = 214, bh = 10;
  gfx->fillRoundRect(x, by, WIN_W, bh, bh / 2, C_BARBG);
  int fw = (int)(WIN_W * pct / 100.0f);
  uint16_t fill = pct >= 90 ? C_RED : pct >= 75 ? C_ACCENT : C_UGREEN;
  if (fw >= bh)    gfx->fillRoundRect(x, by, fw, bh, bh / 2, fill);
  else if (fw > 0) gfx->fillRect(x, by, fw, bh, fill);
}

// Footer: the state tally, plus both usage windows when the sender has them — so
// the board is still worth looking at when nothing needs attention, and the
// numbers that used to need a screen of their own live here instead.
static void drawFooter(Arduino_GFX* gfx, const UsageData& u) {
  gfx->fillRect(8, FOOTER_TOP, 224, 2, C_BARBG);

  drawSummary(gfx, u, FOOTER_TOP + 8);

  // Nothing here when the sender has no rate limits to give. The plugin's hook
  // runs in every entrypoint but is never handed them, so drawing 0% would be a
  // confident lie on the most-read part of the screen. This is the whole reason
  // the windows are a footer and not a mode: absent data costs two rows of
  // silence instead of a screen that cannot be filled.
  if (!u.usageValid) return;

  drawWindow(gfx, 8,       "5H", u.sessionPct);
  drawWindow(gfx, WIN_2X,  "7D", u.weeklyPct);
}

// FNV-1a over the values the board actually draws, in the form it draws them.
// A digest rather than a kept copy because the board is six variable-length rows:
// four bytes of state against a hundred, and the names go in as the strings that
// get printed rather than as their buffers, whose tail past the NUL is left over
// from a longer name.
//
// Diffing this instead of lastOkMs is the fix for the black flash. The daemon
// posts every ~20 s whether or not anything moved, UsageClient stamps lastOkMs on
// every successful parse, and drawBoard opens with a full-screen clear — so every
// post was a visible flash of an unchanged board. Nothing here is finer than a
// minute, so a live board still repaints about once a minute, which is when the
// row ages and the countdown actually move.
//
// The invariant is that every value reaching the panel is mixed in below. Missing
// one is bounded rather than silent — the row ages tick every minute and drag the
// whole digest with them — but it is still a bug, so mix it in.
static uint32_t fnvBytes(uint32_t f, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  while (n--) f = (f ^ *b++) * 16777619u;
  return f;
}

static uint32_t boardFingerprint(const UsageData& u) {
  uint32_t f = 2166136261u;
  // Picks the screen (board / "no session data" / "nothing running"), the header
  // count and its colour, and every count in the footer summary.
  const uint8_t hdr[4] = { (uint8_t)u.boardValid, (uint8_t)u.usageValid,
                           u.sessionRows, u.sessionLive };
  f = fnvBytes(f, hdr, sizeof(hdr));

  for (uint8_t i = 0; i < u.sessionRows; i++) {
    const SessionInfo& si = u.sessions[i];
    f = fnvBytes(f, si.name, strlen(si.name));
    const uint8_t row[3] = { si.state, (uint8_t)si.mins, (uint8_t)(si.mins >> 8) };
    f = fnvBytes(f, row, sizeof(row));
  }

  // Footer usage windows: for each, the printed percentage, the bar's fill width
  // in pixels (finer than the number above it), and the fill colour's band. Both
  // windows, not just the 5h one — a digest that omits the 7d bar is a bar that
  // never repaints when it moves.
  uint8_t foot[6];
  const float pcts[2] = { u.sessionPct, u.weeklyPct };
  for (uint8_t i = 0; i < 2; i++) {
    float p = constrain(pcts[i], 0.0f, 100.0f);
    foot[i * 3 + 0] = (uint8_t)lroundf(p);
    foot[i * 3 + 1] = (uint8_t)(int)(WIN_W * p / 100.0f);
    foot[i * 3 + 2] = (uint8_t)(p >= 90 ? 2 : p >= 75 ? 1 : 0);
  }
  return fnvBytes(f, foot, sizeof(foot));
}

static void drawBoard(const UsageData& u) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);

  drawLeft(gfx, 8, 10, "SESSIONS", 2, C_WHITE);

  if (!u.boardValid) {
    // The payload parsed but carried no board. Say which end needs attention
    // rather than showing an empty board, which would read as "idle".
    //
    // Not "UPDATE THE DAEMON" any more: the sender people install is the clawd
    // plugin, and this screen must not name a component the reader does not
    // have. What reaches here is a sender that predates the board — an old
    // plugin, or the upstream Python daemon — so "your sender" is the phrase
    // that is true of every one of them.
    gfxDrawCentered("no session data", 108, 2, C_DIM);
    gfxDrawCentered("UPDATE YOUR SENDER", 132, 1, C_ACCENT);
    return;
  }

  char head[12];
  snprintf(head, sizeof(head), "%u LIVE", (unsigned)u.sessionLive);
  drawRight(gfx, 232, 10, head, 2, u.sessionLive ? C_ACCENT : C_DIM);
  gfx->fillRect(8, 34, 224, 2, C_BARBG);

  if (u.sessionRows == 0) {
    gfxDrawCentered("nothing running", 96, 2, C_DIM);
    drawFooter(gfx, u);
    return;
  }

  const int rh = rowHeight(u.sessionRows);
  for (uint8_t i = 0; i < u.sessionRows; i++) {
    const SessionInfo& si = u.sessions[i];
    const int top = ROWS_TOP + i * rh;

    gfx->fillCircle(20, top + 11, 6, stateColor(si.state));
    drawLeft(gfx, 36, top + 3, si.name, 2, C_WHITE);

    char age[10];
    fmtDuration(si.mins, age, sizeof(age), "<1m");
    drawRight(gfx, 232, top + 7, age, 1, C_DIM);
  }

  drawFooter(gfx, u);
}

// Nothing has arrived recently: say so rather than leaving a stale board up. A
// board that keeps showing "working" for a laptop that went to sleep is worse
// than one that admits it lost contact.
//
// But "waiting..." alone was a dead end, and it is the screen people actually
// hit — the one moment a recipient needs to know what to do and there is no card
// in the box to tell them. The device knows all three of the things that would
// help, so it prints them: the command, the code that picks this unit out of
// thirty, and the address of its own web UI.
//
// Only a commissioned unit reaches here — main.cpp holds one that has never been
// fed on the commissioning screen instead — so this never has to double as a
// first-run screen, and re-running setup is the honest advice: it is idempotent,
// and re-finding a unit whose IP moved is the usual reason a paired device goes
// quiet and stays quiet.
//
// Every row and its width against the 232 px content budget:
//     10  SESSIONS               2  (left)   8 x 6 x 2 = 96
//     56  waiting...             3           10 x 6 x 3 = 180
//    104  In Claude Code, run    1           19 x 6 x 1 = 114
//    120  /clawd:setup a1b2      2           17 x 6 x 2 = 204
//    176  or open                1            7 x 6 x 1 = 42
//    192  <ip>                   2           15 x 6 x 2 = 180
static void drawStale(const Settings& s, bool error) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  drawLeft(gfx, 8, 10, "SESSIONS", 2, C_WHITE);
  gfx->fillRect(8, 34, 224, 2, C_BARBG);

  // "sender error", not "daemon error": what people install is the clawd plugin.
  gfxDrawCentered(error ? "sender error" : "waiting...", 56, 3, C_DIM);

  char code[8];
  fmtDeviceCode(s.hostname.c_str(), code, sizeof(code));
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "/clawd:setup %s", code);
  gfxDrawCentered("In Claude Code, run", 104, 1, C_DIM);
  // gfxFitSize, not gfxFitSizeMin: this is the line the screen exists for now, so
  // 6x8 beats dropping it. A default name lands at size 2.
  gfxDrawCentered(cmd, 120, gfxFitSize(cmd, 232, 2), C_WHITE);

  gfx->fillRect(8, 156, 224, 2, C_BARBG);

  // The web UI, for anyone who would rather look than type — and the IP rather
  // than <host>.local, unlike the commissioning screen. Two reasons: whoever is
  // reading a stale screen is troubleshooting, and mDNS is exactly the thing that
  // is blocked on the networks where troubleshooting happens; and netIP() is read
  // live, so this line is correct even when the sender's cached address is the
  // thing that went wrong.
  String ip = netIP();
  if (ip.length() && ip != "0.0.0.0") {
    gfxDrawCentered("or open", 176, 1, C_DIM);
    gfxDrawCentered(ip.c_str(), 192, gfxFitSize(ip.c_str(), 232, 2), C_UGREEN);
  }
}

// ---- DisplayMode ----------------------------------------------------------
void SessionsMode::begin(const Settings& s) {
  usageInit(s);
  showedStale_ = false;
  needRender_ = true;
}

void SessionsMode::invalidate(const Settings& s) {
  needRender_ = true;
  showedStale_ = false;
  usageInit(s);
  usageForceRefresh();
}

void SessionsMode::service(const Settings& s) {
  // Pull when a Usage URL is configured, otherwise sit still and let the sender
  // POST to /api/usage. The same test picks the stale window — see usageStaleMs.
  if (s.usage.usageUrl.length() >= 8) usageService(s);

  const UsageData& u = usageGet();

  if (usageFresh(usageStaleMs(s))) {
    if (showedStale_) { showedStale_ = false; needRender_ = true; }
    uint32_t fp = boardFingerprint(u);
    if (fp != renderedFp_) needRender_ = true;
    if (needRender_) { drawBoard(u); renderedFp_ = fp; needRender_ = false; }
  } else if (!showedStale_ || u.error != staleError_) {
    // Also repainted when the REASON changes: a pull-mode unit goes quiet first
    // ("waiting...") and only learns "sender error" on the first failed poll after
    // that, which is the half of the message worth reading.
    showedStale_ = true;
    staleError_  = u.error;
    drawStale(s, u.error);
  }
}
