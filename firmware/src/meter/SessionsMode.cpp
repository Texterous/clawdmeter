#include "SessionsMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Fmt.h"
#include "Palette.h"
#include "UsageClient.h"

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

// "1 BLOCKED  2 WAITING  1 WORKING", minus whatever is zero. Worst case is all
// three at 31 chars = 186 px at size 1, which fits the 224 px content width.
static void summaryLine(const UsageData& u, char* out, size_t n) {
  if (u.sessionLive > u.sessionRows) {   // board overflowed: say so rather than
    snprintf(out, n, "SHOWING %u OF %u", // describing only the rows drawn
             (unsigned)u.sessionRows, (unsigned)u.sessionLive);
    return;
  }
  uint8_t blocked = 0, waiting = 0, working = 0;
  for (uint8_t i = 0; i < u.sessionRows; i++) {
    switch (u.sessions[i].state) {
      case SESS_BLOCKED: blocked++; break;
      case SESS_WORKING: working++; break;
      default:           waiting++; break;
    }
  }
  out[0] = 0;
  size_t len = 0;
  // snprintf returns what it WOULD have written, so clamp before advancing —
  // otherwise a truncating write makes (n - len) underflow on the next one.
  auto append = [&](unsigned count, const char* label) {
    if (!count || len + 1 >= n) return;
    int wrote = snprintf(out + len, n - len, "%s%u %s", len ? "  " : "", count, label);
    if (wrote > 0) len = (len + (size_t)wrote < n) ? len + (size_t)wrote : n - 1;
  };
  append(blocked, "BLOCKED");
  append(waiting, "WAITING");
  append(working, "WORKING");
}

// Footer: the summary plus the 5h window, so the board is still worth looking at
// when nothing needs attention.
static void drawFooter(Arduino_GFX* gfx, const UsageData& u) {
  gfx->fillRect(8, FOOTER_TOP, 224, 2, C_BARBG);

  char line[36];
  summaryLine(u, line, sizeof(line));
  if (line[0]) drawLeft(gfx, 8, FOOTER_TOP + 8, line, 1, C_DIM);

  drawLeft(gfx, 8, 202, "5H WINDOW", 1, C_DIM);
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", (int)lroundf(constrain(u.sessionPct, 0.0f, 100.0f)));
  drawRight(gfx, 232, 202, pct, 1, C_DIM);

  const int bx = 8, by = 214, bw = 224, bh = 10;
  gfx->fillRoundRect(bx, by, bw, bh, bh / 2, C_BARBG);
  int fw = (int)(bw * constrain(u.sessionPct, 0.0f, 100.0f) / 100.0f);
  uint16_t fill = u.sessionPct >= 90 ? C_RED : u.sessionPct >= 75 ? C_ACCENT : C_UGREEN;
  if (fw >= bh)    gfx->fillRoundRect(bx, by, fw, bh, bh / 2, fill);
  else if (fw > 0) gfx->fillRect(bx, by, fw, bh, fill);
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
  const uint8_t hdr[3] = { (uint8_t)u.boardValid, u.sessionRows, u.sessionLive };
  f = fnvBytes(f, hdr, sizeof(hdr));

  for (uint8_t i = 0; i < u.sessionRows; i++) {
    const SessionInfo& si = u.sessions[i];
    f = fnvBytes(f, si.name, strlen(si.name));
    const uint8_t row[3] = { si.state, (uint8_t)si.mins, (uint8_t)(si.mins >> 8) };
    f = fnvBytes(f, row, sizeof(row));
  }

  // Footer 5h window: the printed percentage, the bar's fill width in pixels
  // (224 px, so finer than the number above it), and the fill colour's band.
  float p = constrain(u.sessionPct, 0.0f, 100.0f);
  const uint8_t foot[3] = { (uint8_t)lroundf(p), (uint8_t)(int)(224 * p / 100.0f),
                            (uint8_t)(p >= 90 ? 2 : p >= 75 ? 1 : 0) };
  return fnvBytes(f, foot, sizeof(foot));
}

static void drawBoard(const UsageData& u) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);

  drawLeft(gfx, 8, 10, "SESSIONS", 2, C_WHITE);

  if (!u.boardValid) {
    // The payload parsed but carried no board — an older daemon. Say which end
    // needs updating instead of showing an empty board that looks like "idle".
    gfxDrawCentered("no session data", 108, 2, C_DIM);
    gfxDrawCentered("UPDATE THE DAEMON", 132, 1, C_ACCENT);
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

// Nothing has arrived recently: say so rather than leaving a stale board up.
// A board that keeps showing "working" for a laptop that went to sleep is worse
// than one that admits it lost contact.
static void drawStale(bool error) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  drawLeft(gfx, 8, 10, "SESSIONS", 2, C_WHITE);
  gfx->fillRect(8, 34, 224, 2, C_BARBG);
  gfxDrawCentered(error ? "daemon error" : "waiting...", 112, 2, C_DIM);
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
  // Same contract as UsageMode: pull when a Usage URL is configured, otherwise
  // sit still and let the daemon POST to /api/usage.
  if (s.usage.usageUrl.length() >= 8) usageService(s);

  const UsageData& u = usageGet();
  uint32_t staleMs = (uint32_t)s.usage.pollSec * 1000UL * 2UL + USAGE_STALE_GRACE_MS;

  if (usageFresh(staleMs)) {
    if (showedStale_) { showedStale_ = false; needRender_ = true; }
    uint32_t fp = boardFingerprint(u);
    if (fp != renderedFp_) needRender_ = true;
    if (needRender_) { drawBoard(u); renderedFp_ = fp; needRender_ = false; }
  } else if (!showedStale_ || u.error != staleError_) {
    // Also repainted when the REASON changes: a pull-mode unit goes quiet first
    // ("waiting...") and only learns "daemon error" on the first failed poll after
    // that, which is the half of the message worth reading.
    showedStale_ = true;
    staleError_  = u.error;
    drawStale(u.error);
  }
}
