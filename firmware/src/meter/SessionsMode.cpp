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

// The lamp colour, and whether the lamp is still speaking for the present.
//
// A board that is no longer live keeps its shape and loses its urgency: same
// rows, same order, every dot grey. This is the one thing the old "waiting..."
// screen got right by accident and at too high a price — it refused to leave a
// red dot on the glass for a permission prompt that was answered an hour ago. The
// rows are still worth seeing; the traffic light is not.
static uint16_t stateColor(uint8_t state, bool live) {
  if (!live) return C_DGRAY;
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

// Where a board that is not live came from, in one line of at most 16 characters.
//
// The time is the SENDER's wall clock, carried in the payload (UsageData::ts) and
// not the device's — see UsageData.h for why that is the right way round. Without
// a stamp the line degrades to the two words that are still true.
//
//   "LAST SEEN 09:12"   aged out during this boot: we had contact and lost it
//   "LAST KNOWN 22:41"  read back from flash: this is what the device knew before
//                       it was unplugged
//   "NO CONTACT"        a board with no stamp, from a sender too old to send one
static void fmtProvenance(const UsageData& u, char* out, size_t n) {
  const int32_t day = u.stampLocalDaySec();
  if (day < 0) { strlcpy(out, u.restored ? "LAST KNOWN" : "NO CONTACT", n); return; }
  snprintf(out, n, "%s %02d:%02d", u.restored ? "LAST KNOWN" : "LAST SEEN",
           (int)(day / 3600), (int)((day % 3600) / 60));
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
static void drawFooter(Arduino_GFX* gfx, const UsageData& u, bool live) {
  gfx->fillRect(8, FOOTER_TOP, 224, 2, C_BARBG);

  if (!live) {
    // The tally and both windows describe a moment that has passed, so neither is
    // drawn: a "1 WORKING" under grey dots would be the same lie the old screen
    // was built to avoid, and a percentage bar cannot be labelled stale. The
    // footer says when instead, and that nobody needs to do anything.
    char prov[24];
    fmtProvenance(u, prov, sizeof(prov));
    drawLeft(gfx, 8, FOOTER_TOP + 8, prov, 1, C_DIM);
    drawLeft(gfx, 8, FOOTER_TOP + 26, "Resumes by itself", 1, C_DGRAY);
    return;
  }

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

static uint32_t boardFingerprint(const UsageData& u, bool live, bool haveBoard) {
  uint32_t f = 2166136261u;

  // Liveness leads, because it now picks colours, the header's right-hand side
  // and the whole footer — and because the transition to not-live is the one
  // repaint that used to need its own bookkeeping (showedStale_) to happen at
  // all. It is a fingerprint input like any other now.
  const uint8_t sel[3] = { (uint8_t)live, (uint8_t)haveBoard, (uint8_t)u.error };
  f = fnvBytes(f, sel, sizeof(sel));

  // No board: the idle screen draws none of what follows, and mixing it in would
  // repaint a full screen — a visible black flash — every time a row aged behind
  // a screen that does not show rows. Its only moving part is the address, which
  // changes when the unit's lease does, and that is worth catching.
  if (!haveBoard) {
    String ip = netIP();
    return fnvBytes(f, ip.c_str(), ip.length());
  }

  // Picks the screen (board / "no session data" / "nothing running"), the header
  // count and its colour, and every count in the footer summary.
  const uint8_t hdr[4] = { (uint8_t)u.boardValid, (uint8_t)u.usageValid,
                           u.sessionRows, u.sessionLive };
  f = fnvBytes(f, hdr, sizeof(hdr));

  // The provenance line, to the minute it prints. Only drawn when not live, and
  // then it is the only thing on the screen that moves.
  const int32_t day = u.stampLocalDaySec();
  const uint8_t prov[3] = { (uint8_t)u.restored,
                            (uint8_t)(day < 0 ? 0xFF : (day / 60) & 0xFF),
                            (uint8_t)(day < 0 ? 0xFF : (day / 60) >> 8) };
  f = fnvBytes(f, prov, sizeof(prov));

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

static void drawBoard(const UsageData& u, bool live) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);

  drawLeft(gfx, 8, 10, "SESSIONS", 2, live ? C_WHITE : C_DIM);

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

  // "3 LIVE" is a claim about right now, so it goes away when right now is no
  // longer what is on the glass. The count is still in the rows for anyone who
  // wants it; what the header would add is the word that makes it a lie.
  if (live) {
    char head[12];
    snprintf(head, sizeof(head), "%u LIVE", (unsigned)u.sessionLive);
    drawRight(gfx, 232, 10, head, 2, u.sessionLive ? C_ACCENT : C_DIM);
  }
  gfx->fillRect(8, 34, 224, 2, C_BARBG);

  if (u.sessionRows == 0) {
    // Live and empty is "nothing running", which is a real, restful answer.
    // Not live and empty is the overnight case: the last thing the device knew
    // was that nothing was running, and the footer dates it.
    gfxDrawCentered(live ? "nothing running" : "nothing was running",
                    96, live ? 2 : 1, C_DIM);
    drawFooter(gfx, u, live);
    return;
  }

  const int rh = rowHeight(u.sessionRows);
  for (uint8_t i = 0; i < u.sessionRows; i++) {
    const SessionInfo& si = u.sessions[i];
    const int top = ROWS_TOP + i * rh;

    gfx->fillCircle(20, top + 11, 6, stateColor(si.state, live));
    drawLeft(gfx, 36, top + 3, si.name, 2, live ? C_WHITE : C_DIM);

    char age[10];
    fmtDuration(si.mins, age, sizeof(age), "<1m");
    drawRight(gfx, 232, top + 7, age, 1, live ? C_DIM : C_DGRAY);
  }

  drawFooter(gfx, u, live);
}

// The screen for a device with NO board to show at all: never fed on this boot
// and nothing on flash either. It is the old stale screen, minus the job it
// should never have had.
//
// What changed underneath it: a board now survives a reboot (meter/BoardStore)
// and the device asks its sender for a fresh one instead of waiting to be found.
// So the ordinary quiet cases — a closed laptop, a power cycle, an overnight gap —
// all draw the last board dimmed and dated, and this screen is left with only the
// cases where something is genuinely wrong or genuinely new: a unit whose sender
// has never reached it, or one upgraded into this firmware before it had a file
// to restore. That is why the command is back on top: down here, it IS the advice.
//
// "waiting..." is gone as a word. It described the device's own state machine
// rather than anything the reader could act on, and it was the first thing
// recipients saw on every power cycle.
//
// Every row and its width against the 232 px content budget:
//     10  SESSIONS                   2  (left)   8 x 6 x 2 = 96
//     64  no contact                 3           10 x 6 x 3 = 180
//    110  Nothing has reached this   1           24 x 6 x 1 = 144
//    124  display yet. To pair it:   1           24 x 6 x 1 = 144
//    160  /clawd:setup a1b2          2           17 x 6 x 2 = 204
//    196  Already paired? This is    1           23 x 6 x 1 = 138
//    210  normal while Claude Code   1           24 x 6 x 1 = 144
//    224  is closed.  <ip>           1
static void drawIdle(const Settings& s, bool error) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  drawLeft(gfx, 8, 10, "SESSIONS", 2, C_WHITE);
  gfx->fillRect(8, 34, 224, 2, C_BARBG);

  if (error) {
    // Only a PULL-mode unit can reach this: usageService sets the flag on a failed
    // fetch, and a pushed unit never fetches. "sender error", not "daemon error" —
    // what people install is the clawd plugin. 12 x 6 x 3 = 216 px.
    gfxDrawCentered("sender error", 64, 3, C_DIM);
    gfxDrawCentered("This device could not reach", 110, 1, C_DIM);   // 27 x 6 = 162
    gfxDrawCentered("the pull URL it is set to", 124, 1, C_DIM);     // 25 x 6 = 150
    String eip = netIP();
    if (eip.length() && eip != "0.0.0.0") gfxDrawCentered(eip.c_str(), 216, 1, C_DIM);
    return;
  }

  gfxDrawCentered("no contact", 64, 3, C_DIM);
  gfxDrawCentered("Nothing has reached this", 110, 1, C_DIM);
  gfxDrawCentered("display yet. To pair it:", 124, 1, C_DIM);

  char code[8];
  fmtDeviceCode(s.hostname.c_str(), code, sizeof(code));
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "/clawd:setup %s", code);
  // gfxFitSize, not gfxFitSizeMin: 6x8 beats dropping the line. A default name
  // lands at size 2.
  gfxDrawCentered(cmd, 160, gfxFitSize(cmd, 232, 2), C_WHITE);

  gfx->fillRect(8, 186, 224, 2, C_BARBG);

  // Kept, in a quieter register: a recipient who reaches this screen having
  // already paired should not be told to start again. It is rarer than it was —
  // this screen no longer stands in for an overnight gap — but a unit whose
  // /board.json has not been written yet can still land here for one evening.
  gfxDrawCentered("Already paired? Normal", 200, 1, C_DGRAY);
  gfxDrawCentered("while Claude Code is closed", 214, 1, C_DGRAY);

  // The web UI, for anyone who would rather look than type — and the IP rather
  // than <host>.local, unlike the commissioning screen. Two reasons: whoever is
  // reading this far is troubleshooting, and mDNS is exactly the thing that is
  // blocked on the networks where troubleshooting happens; and netIP() is read
  // live, so this line is correct even when the sender's cached address is the
  // thing that went wrong.
  String ip = netIP();
  if (ip.length() && ip != "0.0.0.0")
    gfxDrawCentered(ip.c_str(), 228, 1, C_DIM);
}

// ---- DisplayMode ----------------------------------------------------------
void SessionsMode::begin(const Settings& s) {
  // No usageInit() here any more, and that is the whole of what makes a restored
  // board visible: begin() runs after boardStoreBegin() has put last night's
  // rows into the snapshot, and usageInit() clears it. main.cpp calls the two in
  // that order; clearing here would have thrown away the file it just read.
  needRender_ = true;
}

void SessionsMode::invalidate(const Settings& s) {
  needRender_ = true;
  usageInit(s);
  usageForceRefresh();
}

void SessionsMode::service(const Settings& s) {
  // Pull when a Usage URL is configured, otherwise sit still and let the sender
  // POST to /api/usage. The same test picks the stale window — see usageStaleMs.
  if (s.usage.usageUrl.length() >= 8) usageService(s);

  const UsageData& u = usageGet();
  const bool live = usageFresh(usageStaleMs(s));

  // Rows to draw, whatever their age. A restored board and a board that aged out
  // ten minutes ago are the same thing to this screen: real sessions, dimmed and
  // dated. Only a unit with nothing at all falls through to drawIdle, and a
  // pull-mode error goes there too — it is a message about the sender, not a
  // board, so leaving old rows up under it would bury it.
  const bool haveBoard = u.valid && u.boardValid && !u.error;

  const uint32_t fp = boardFingerprint(u, live, haveBoard);
  if (!needRender_ && fp == renderedFp_) return;

  // One render path, gated on one digest. The stale screen used to keep its own
  // showedStale_ / staleError_ flags to avoid repainting itself every 5 ms; with
  // liveness and the error flag mixed into the fingerprint there is nothing left
  // for them to remember, and the transition into and out of not-live repaints
  // for the same reason every other change does.
  if (haveBoard) drawBoard(u, live);
  else           drawIdle(s, u.error);
  renderedFp_ = fp;
  needRender_ = false;
}
