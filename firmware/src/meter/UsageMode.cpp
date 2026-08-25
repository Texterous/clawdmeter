#include "UsageMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Fmt.h"
#include "Palette.h"
#include "UsageClient.h"
#include "Mascot.h"
#include "../Net.h"

UsageMode g_usageMode;

// Card geometry. BAR_W is shared with usageFrame() below because the fill width
// in PIXELS is what the change diff compares: a reading that moves 0.3% shifts no
// bar and prints the same integer, and must not repaint anything.
static const int CARD_X = 8, CARD_W = 224, CARD_H = 82;
static const int CARD_TOP[2] = { 50, 138 };
static const int BAR_W = CARD_W - 28, BAR_H = 12;

// The two header cells that carry data, cleared individually on a partial repaint
// so the mascot logo beside them survives. The title is 6 chars at size 3 =
// 6*6*3 = 108 px from x=56 (STALE is 5 chars = 90 px and sits inside that); the
// flag is a r=5 dot centred on (228,18), so 12x12 from (222,12) covers it.
static const int TITLE_X = 56, TITLE_Y = 12, TITLE_W = 108, TITLE_H = 24;
static const int FLAG_X = 222, FLAG_Y = 12, FLAG_WH = 12;

#define HDR_FLAG   0x1u   // rate-limit status is something other than "allowed"
#define HDR_STALE  0x2u   // these are the last numbers received, not live ones

// Mascot diff state for the flicker-free full-screen idle animation.
static bool     s_mascotPrimed = false;
static uint16_t s_mascotPal[MASCOT_PALETTE_SIZE];   // the palette actually painted
static uint8_t  s_prevCells[MASCOT_GRID * MASCOT_GRID];

// Copy a mascot palette into a local RAM array using *byte* reads. pgm_read_byte
// is safe from both RAM and flash; a 16-bit load straight from flash (irom) faults
// on the ESP8266, so this never depends on where the palette actually lives.
// Tinted on the way out so the mascot's own palette follows the Display tab's
// colour correction like everything else.
static void loadPalette(const uint16_t* palette, uint16_t* out) {
  const uint8_t* p = (const uint8_t*)palette;
  for (int k = 0; k < MASCOT_PALETTE_SIZE; k++)
    out[k] = gfxTint((uint16_t)(pgm_read_byte(p + 2 * k) | (pgm_read_byte(p + 2 * k + 1) << 8)));
}

// Draw a 20x20 mascot frame at (x0,y0), cellPx per cell. Reads PROGMEM frame data.
static void blitMascot(Arduino_GFX* gfx, const uint8_t* cells, const uint16_t* palette,
                       int x0, int y0, int cellPx) {
  uint16_t pal[MASCOT_PALETTE_SIZE];
  loadPalette(palette, pal);
  for (int i = 0; i < MASCOT_GRID * MASCOT_GRID; i++) {
    uint8_t code = pgm_read_byte(&cells[i]);
    uint16_t color = (code < MASCOT_PALETTE_SIZE) ? pal[code] : 0;
    int gx = i % MASCOT_GRID, gy = i / MASCOT_GRID;
    gfx->fillRect(x0 + gx * cellPx, y0 + gy * cellPx, cellPx, cellPx, color);
  }
}

static uint16_t barColor(float pct) {
  if (pct >= 90) return C_RED;
  if (pct >= 75) return C_ACCENT;
  return C_UGREEN;
}

// Reduce a reading to what the panel will show. Everything that rounds, clamps or
// picks a colour happens here and nowhere else, which is what keeps the diff in
// service() honest: it compares the drawn numbers, not the payload.
//
// Diffing this instead of lastOkMs is the fix for the black flash. The daemon
// posts every ~20 s whether or not anything moved, UsageClient stamps lastOkMs on
// every successful parse, and drawUsage used to open with a full-screen clear — so
// every post was a visible flash of an unchanged screen. Nothing here is finer
// than a minute, so a live screen still repaints about once a minute, which is
// exactly as often as the countdown has something new to say.
static UsageFrame usageFrame(const UsageData& u, bool fresh) {
  UsageFrame f = {};
  const float pct[2] = { u.sessionPct, u.weeklyPct };
  const int   rst[2] = { u.sessionResetMin, u.weeklyResetMin };
  for (int i = 0; i < 2; i++) {
    float p = constrain(pct[i], 0.0f, 100.0f);
    f.card[i].pct       = (int32_t)lroundf(p);
    f.card[i].fill      = (int32_t)(BAR_W * p / 100.0f);
    f.card[i].resetMins = rst[i];
    f.card[i].color     = barColor(p);
  }
  // A non-"allowed" status (warning / rejected) gets a small accent flag.
  // strcmp, not strncmp(...,7): the daemon's documented values are `allowed`,
  // `allowed_warning` and `rejected`, and a 7-char compare reads the warning as
  // plain "allowed" — so the one status the flag exists to surface was the one
  // status that never raised it.
  if (u.status[0] && strcmp(u.status, "allowed") != 0) f.header |= HDR_FLAG;
  if (!fresh) f.header |= HDR_STALE;
  return f;
}

// One usage card: big %, a 5h/7d label, a fill bar coloured by load, and the reset
// countdown. `top` is the card's top y. The opening fillRoundRect covers the whole
// 224x82 card, which is what lets a repaint of one card skip the rest of the panel.
static void drawMeter(Arduino_GFX* gfx, int top, const char* label, const UsageCard& c) {
  gfx->fillRoundRect(CARD_X, top, CARD_W, CARD_H, 8, C_PANEL);

  char pc[8];
  snprintf(pc, sizeof(pc), "%d%%", (int)c.pct);
  uint8_t sz = gfxFitSize(pc, 150, 5);
  gfx->setTextSize(sz);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(CARD_X + 14, top + 10);
  gfx->print(pc);

  int lw = gfxTextW(label, 2);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(CARD_X + CARD_W - lw - 14, top + 12);
  gfx->print(label);

  int bx = CARD_X + 14, by = top + 52;
  gfx->fillRoundRect(bx, by, BAR_W, BAR_H, BAR_H / 2, C_BARBG);
  if (c.fill >= BAR_H)    gfx->fillRoundRect(bx, by, c.fill, BAR_H, BAR_H / 2, (uint16_t)c.color);
  else if (c.fill > 0)    gfx->fillRect(bx, by, c.fill, BAR_H, (uint16_t)c.color);

  char rs[16], line[28];
  fmtDuration((int)c.resetMins, rs, sizeof(rs));
  snprintf(line, sizeof(line), "Resets in %s", rs);
  gfx->setTextSize(2);
  gfx->setTextColor(C_DIM);
  gfx->setCursor(CARD_X + 14, top + 64);
  gfx->print(line);
}

// Stats screen. `prev` is what is already on the panel, or nullptr for a full
// repaint. A partial pass touches only the cards whose numbers moved plus, if the
// header changed, its two small cells — the mascot logo and the black around it
// are left alone. Same reasoning as gfxSplashCaption (Gfx.cpp): a fillScreen on a
// path that runs every twenty seconds IS the flicker.
static void drawUsage(const UsageFrame& f, const UsageFrame* prev) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  s_mascotPrimed = false;   // the idle animation must start from a clean grid

  if (!prev) {
    gfx->fillScreen(C_BLACK);
    blitMascot(gfx, mascotIdleCells(), mascotIdlePalette(), 6, 4, 2);
  }

  if (!prev || prev->header != f.header) {
    if (prev) {   // no fillScreen ran, so clear just the cells being rewritten
      gfx->fillRect(TITLE_X, TITLE_Y, TITLE_W, TITLE_H, C_BLACK);
      gfx->fillRect(FLAG_X, FLAG_Y, FLAG_WH, FLAG_WH, C_BLACK);
    }
    // A screen still headed CLAUDE over numbers from an hour ago reads as live.
    // Naming it is the whole of what "showMascot off" can honestly promise.
    bool stale = (f.header & HDR_STALE) != 0;
    gfx->setTextSize(3);
    gfx->setTextColor(stale ? C_ACCENT : C_WHITE);
    gfx->setCursor(TITLE_X, TITLE_Y);
    gfx->print(stale ? "STALE" : "CLAUDE");
    if (f.header & HDR_FLAG) gfx->fillCircle(228, 18, 5, C_ACCENT);
  }

  for (int i = 0; i < 2; i++)
    if (!prev || memcmp(&prev->card[i], &f.card[i], sizeof(f.card[i])) != 0)
      drawMeter(gfx, CARD_TOP[i], i ? "7d" : "5h", f.card[i]);
}

// Idle animation: full-screen mascot, diffed cell-by-cell for a flicker-free draw.
// Returns true if it repainted every cell. The idle footer sits in the bottom
// grid rows, so a full pass overpaints it and the caller has to put it back; a
// partial pass cannot, because those cells are background in every idle frame and
// therefore never differ from s_prevCells.
static bool drawMascot(const uint8_t* cells, const uint16_t* palette, bool restart) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx || !cells || !palette) return false;
  uint16_t pal[MASCOT_PALETTE_SIZE];
  loadPalette(palette, pal);
  const int CP = TFT_WIDTH / MASCOT_GRID;                 // 240 / 20 = 12
  const int x0 = (TFT_WIDTH  - MASCOT_GRID * CP) / 2;
  const int y0 = (TFT_HEIGHT - MASCOT_GRID * CP) / 2;

  // Compare the palette CONTENTS, not the pointer they came from. Four of the five
  // palettes in mascot_frames.h are byte-identical, so the pointer test fired a
  // full repaint on every 20 s mood rotation over pixels that had not changed —
  // and a genuine palette swap that left the cell codes alone would have been
  // missed, leaving the old colours up. pal is already tinted, so a Display-tab
  // colour change lands in the same test for free.
  bool full = restart || !s_mascotPrimed || memcmp(pal, s_mascotPal, sizeof(pal)) != 0;

  // No fillScreen here: the grid is 20 cells of 12 px at x0=y0=0, so a full pass
  // paints all 240x240 and the clear was overpainted pixel-for-pixel. It bought
  // nothing and it was the flash.
  for (int i = 0; i < MASCOT_GRID * MASCOT_GRID; i++) {
    uint8_t code = pgm_read_byte(&cells[i]);
    if (!full && code == s_prevCells[i]) continue;
    s_prevCells[i] = code;
    uint16_t color = (code < MASCOT_PALETTE_SIZE) ? pal[code] : 0;
    int gx = i % MASCOT_GRID, gy = i / MASCOT_GRID;
    gfx->fillRect(x0 + gx * CP, y0 + gy * CP, CP, CP, color);
  }
  s_mascotPrimed = true;
  memcpy(s_mascotPal, pal, sizeof(pal));
  return full;
}

// The idle screen's address footer.
//
// Why this exists: the mascot means "nothing is feeding me", and once a unit is
// commissioned that is the screen it comes back to on every cold boot with the
// laptop shut. It carried no text at all, so a recipient whose .local does not
// resolve — which is the common case on the networks this has been tested on —
// had no way left to reach the device. That is the same dead end the commissioning
// screen was added to remove, just moved to the second boot onward.
//
// Geometry: the grid is 20 rows of 12 px and rateGroup() is pinned to group 0
// while no sample has ever been taken, so only idle_breathe and idle_blink can
// play here and neither touches a cell below row 16. Rows 17-19 (y >= 204) are
// therefore free. Widths at 6 px per character per size: a 32-char hostname plus
// ".local" is 38 x 6 = 228 px at size 1, and the longest IPv4 is 15 x 6 x 2 =
// 180 px at size 2 — both inside the 232 px content width.
static const int FOOT_Y = 208, FOOT_H = 32;

static char s_footer[64] = "";

static void drawIdleFooter(const Settings& s, bool force) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  String ip = netIP();
  char want[64];
  snprintf(want, sizeof(want), "%s.local\n%s", s.hostname.c_str(), ip.c_str());
  // Repaint only when the text actually changes (a DHCP move) or the mascot just
  // overpainted us. Repainting on every animation tick would be a flashing band.
  if (!force && strcmp(want, s_footer) == 0) return;
  strlcpy(s_footer, want, sizeof(s_footer));

  gfx->fillRect(0, FOOT_Y, TFT_WIDTH, FOOT_H, C_BLACK);

  char host[48];
  snprintf(host, sizeof(host), "%s.local", s.hostname.c_str());
  // Dropped rather than shrunk below 6x8: a hostname long enough to overflow was
  // hand-set by someone who can already find the page.
  if (gfxFitSizeMin(host, 232, 1, 1)) gfxDrawCentered(host, FOOT_Y, 1, C_DIM);
  // The IP is the line that always works, so it gets the readable size.
  if (ip.length()) gfxDrawCentered(ip.c_str(), FOOT_Y + 12, gfxFitSize(ip.c_str(), 232, 2), C_UGREEN);
}

// ---- DisplayMode ----------------------------------------------------------
void UsageMode::begin(const Settings& s) {
  usageInit(s);
  mascotInit();
  usageSampled_ = 0;
  shown_ = UsageFrame{};
  primed_ = false;
  showingMascot_ = false;
  needRender_ = true;
}

void UsageMode::invalidate(const Settings& s) {
  needRender_ = true;
  primed_ = false;       // a settings change repaints the lot, tinted colours included
  showingMascot_ = false;
  usageInit(s);
  usageForceRefresh();
}

void UsageMode::service(const Settings& s) {
  // Pull mode: poll the daemon when a Usage URL is set. Push mode: leave it blank
  // and the daemon POSTs to /api/usage (for networks where the device can't reach
  // the PC). Either way usageGet() drives the render below.
  if (s.usage.usageUrl.length() >= 8) usageService(s);

  const UsageData& u = usageGet();

  // Feed the burn-rate tracker once per fresh reading (drives the mascot's mood).
  if (u.valid && u.lastOkMs != usageSampled_) {
    usageSampled_ = u.lastOkMs;
    mascotSample(u.sessionPct);
  }

  // Considered stale after ~2 missed polls (plus a grace) — then show the animation.
  uint32_t staleMs = (uint32_t)s.usage.pollSec * 1000UL * 2UL + USAGE_STALE_GRACE_MS;
  bool fresh = usageFresh(staleMs);

  // showMascot off means "numbers, not animation": keep the last reading on screen
  // with the title marking it stale, rather than hiding it behind the idle
  // creature. The setting was persisted and had a checkbox, and no renderer read
  // it — a settings page that lies is worse than one option fewer.
  if (fresh || (!s.usage.showMascot && u.valid)) {
    if (showingMascot_) { showingMascot_ = false; needRender_ = true; primed_ = false; }
    UsageFrame f = usageFrame(u, fresh);
    // needRender_ (boot, a settings change, a mode switch, the mascot coming down)
    // repaints regardless; otherwise only a change in what would be drawn does.
    if (memcmp(&f, &shown_, sizeof(f)) != 0) needRender_ = true;
    if (needRender_) {
      drawUsage(f, primed_ ? &shown_ : nullptr);
      shown_ = f;
      primed_ = true;
      needRender_ = false;
    }
  } else {
    // Only while nothing has ever arrived this session. A unit that has had a
    // reading and merely went stale has a working agent behind it, and its owner
    // does not need the address printed at them.
    const bool wantFooter = !u.valid;
    bool full;
    if (!showingMascot_) {
      showingMascot_ = true;
      primed_ = false;
      s_footer[0] = '\0';   // came from another screen: the band is not ours yet
      mascotReset();
      full = drawMascot(mascotCells(), mascotPalette(), /*restart=*/true);
    } else {
      full = mascotTick() ? drawMascot(mascotCells(), mascotPalette(), /*restart=*/false)
                          : false;
    }
    // netIP() builds a String, so poll it at 1 Hz rather than every 5 ms pass —
    // the only thing being watched for is a DHCP move. A full mascot pass has
    // just overpainted the band and skips the interval.
    if (wantFooter && (full || millis() - footerMs_ >= 1000)) {
      footerMs_ = millis();
      drawIdleFooter(s, full);
    }
  }
}
