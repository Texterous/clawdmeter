#include "Gfx.h"
#include "splash/logo.h"
#include "Platform.h"
#include <Arduino_GFX_Library.h>
#include <SPI.h>

// The SmallTV's ST7789 has its CS line tied to GND and only latches SPI in
// **mode 3**. Arduino_GFX's stock Arduino_ST7789 forces SPI_MODE2 on the ESP8266
// (wrong clock edge for this panel), so the controller never initializes and the
// screen stays black even with the backlight on. Subclass begin() to force mode 3
// — matching the known-good GeekMagic community firmwares. (On ESP32 the base
// class already selects mode 3, so the override is harmless there.)

// Runtime panel colour order, read by the setRotation override below. The board
// header's TFT_BGR is the factory default for that variant; the Display tab can
// override it, because units of the same model do turn up with red and blue
// swapped in the controller.
static bool g_bgr = (TFT_BGR != 0);

class Arduino_ST7789_SmallTV : public Arduino_ST7789 {
 public:
  using Arduino_ST7789::Arduino_ST7789;   // inherit constructors
  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    _override_datamode = SPI_MODE3;
    return Arduino_TFT::begin(speed);
  }

  // Arduino_ST7789 hardcodes the MADCTL RGB order, so re-issue MADCTL with the
  // BGR bit (0x08) tracking g_bgr on every rotation change. The MX/MY/MV values
  // below are the base class's own mapping (ST7789_MADCTL_RGB is 0x00), so with
  // g_bgr false this writes exactly what the library would have written. Only
  // rotations 0-3 are used by the SmallTV (setRotation(r & 3)).
  void setRotation(uint8_t r) override {
    Arduino_TFT::setRotation(r);           // updates _rotation + width/height
    uint8_t madctl;
    switch (_rotation) {
      case 1:  madctl = ST7789_MADCTL_MX | ST7789_MADCTL_MV; break;
      case 2:  madctl = ST7789_MADCTL_MX | ST7789_MADCTL_MY; break;
      case 3:  madctl = ST7789_MADCTL_MY | ST7789_MADCTL_MV; break;
      default: madctl = 0; break;          // case 0
    }
    if (g_bgr) madctl |= 0x08;              // BGR
    _bus->beginWrite();
    _bus->writeC8D8(ST7789_MADCTL, madctl);
    _bus->endWrite();
  }
};

// ---- colour correction ----------------------------------------------------
// Per-channel gain in percent, 100 = untouched. Kept as plain bytes so the
// all-default case is a single comparison on the hot drawing path.
static uint8_t g_rGain = 100, g_gGain = 100, g_bGain = 100;

uint16_t gfxTint(uint16_t c) {
  if (g_rGain == 100 && g_gGain == 100 && g_bGain == 100) return c;
  uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r = r * g_rGain / 100; if (r > 31) r = 31;
  g = g * g_gGain / 100; if (g > 63) g = 63;
  b = b * g_bGain / 100; if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static Arduino_DataBus* bus = nullptr;
static Arduino_GFX*     gfx = nullptr;

Arduino_GFX* gfxDev() { return gfx; }

// ---------------------------------------------------------------------------
void gfxBegin(const Settings& s) {
#ifdef TFT_PWR_PIN
  // Boards with a switched panel power rail (NM-TV-154): energize the display
  // before anything else or the panel never comes up.
  pinMode(TFT_PWR_PIN, OUTPUT);
  digitalWrite(TFT_PWR_PIN, TFT_PWR_ON);
#endif
  // Backlight FIRST: do it before the panel/SPI init so the screen lights up even
  // if panel init has trouble. A dark backlight then means the sketch didn't get
  // this far (early crash / bad flash) — a useful boot indicator.
  pinMode(TFT_BL, OUTPUT);
  platformAnalogWriteInit(TFT_BL);
  gfxSetBrightness(s.brightness, s.backlightInverted);

  bus = new Arduino_HWSPI(TFT_DC, TFT_CS);   // ESP8266 HW-SPI (fixed SCLK/MOSI)
  // IPS=true so the panel colors are not inverted. The ST7789(V) has 240x320
  // RAM against 240x240 glass, so the rotation 2/3 row offset (TFT_ROW_OFFSET2,
  // 80) matters: without it a 180°-rotated image slides into the dead band.
  // Use the SmallTV variant so the SPI bus comes up in mode 3 (see class above).
  gfx = new Arduino_ST7789_SmallTV(bus, TFT_RST, 0 /*rotation*/, true /*IPS*/,
                                   TFT_WIDTH, TFT_HEIGHT,
                                   TFT_COL_OFFSET1, TFT_ROW_OFFSET1,
                                   TFT_COL_OFFSET2, TFT_ROW_OFFSET2);
  gfx->begin();
  // Colour order/inversion/gain before the first pixel, so the panel comes up
  // already corrected instead of flashing an uncorrected boot screen. This also
  // writes MADCTL for the configured rotation.
  gfxApplyColors(s);
  // Nothing in this UI ever wants wrapped text: overflowing labels used to
  // wrap around to x=0 on the next line (stray characters at the left edge).
  gfx->setTextWrap(false);
  gfx->fillScreen(C_BLACK);
}

void gfxSetBrightness(uint8_t pct, bool inverted) {
  if (pct > 100) pct = 100;
  int duty = (int)pct * 255 / 100;
  if (inverted) duty = 255 - duty;
  analogWrite(TFT_BL, duty);
}

void gfxSetRotation(uint8_t r) {
  if (gfx) gfx->setRotation(r & 3);
}

void gfxApplyColors(const Settings& s) {
  g_rGain = s.display.rGain;
  g_gGain = s.display.gGain;
  g_bGain = s.display.bGain;
  g_bgr = (s.display.colorOrder == COLOR_ORDER_BGR) ? true
        : (s.display.colorOrder == COLOR_ORDER_RGB) ? false
                                                    : (TFT_BGR != 0);
  if (!gfx) return;
  gfx->setRotation(s.rotation & 3);   // re-issues MADCTL with the new order
  gfx->invertDisplay(s.display.invert);
}

// ---- text helpers (built-in 6x8 font, integer scaled) ---------------------
int gfxTextW(const char* s, uint8_t size) { return (int)strlen(s) * GFX_FONT_W * size; }

void gfxDrawCentered(const char* s, int y, uint8_t size, uint16_t color) {
  if (!gfx) return;
  int x = (TFT_WIDTH - gfxTextW(s, size)) / 2;
  if (x < 0) x = 0;
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

// Largest size in [minSize, maxSize] whose rendered width fits maxW, or 0 when
// even minSize overflows. The old single-function version fell through to 1
// without ever width-testing it, so a 21-character name rendered as 6x8 text on
// a 1.54" panel — legible only to someone who already knew what it said.
// Callers that get 0 drop or truncate the line.
uint8_t gfxFitSizeMin(const char* s, int maxW, uint8_t maxSize, uint8_t minSize) {
  if (!minSize) minSize = 1;
  for (uint8_t sz = maxSize; ; sz--) {
    if (gfxTextW(s, sz) <= maxW) return sz;
    if (sz <= minSize) break;   // uint8_t: decrementing past minSize would wrap to 255
  }
  return 0;
}

// Kept for the lines that genuinely cannot be dropped — the setup SSID, the IP,
// the boot caption. One loop, so every existing caller behaves bit-identically.
uint8_t gfxFitSize(const char* s, int maxW, uint8_t maxSize) {
  uint8_t sz = gfxFitSizeMin(s, maxW, maxSize, 1);
  return sz ? sz : 1;
}

// ---------------------------------------------------------------------------
// Draw the 1-bit mark at an integer scale, centred horizontally. A per-pixel
// loop rather than drawBitmap because we want it scaled: 909 ink pixels at boot
// is imperceptible, and it keeps the asset at one small size in flash.
static void drawMark(int top, uint8_t scale, uint16_t color) {
  const int w = LOGO_W * scale;
  const int x0 = (TFT_WIDTH - w) / 2;
  for (int y = 0; y < LOGO_H; y++) {
    for (int x = 0; x < LOGO_W; x++) {
      uint8_t byte = pgm_read_byte(&TEXTEROUS_LOGO[y * LOGO_ROW_BYTES + (x >> 3)]);
      if (!(byte & (0x80 >> (x & 7)))) continue;
      if (scale == 1) gfx->drawPixel(x0 + x, top + y, color);
      else gfx->fillRect(x0 + x * scale, top + y * scale, scale, scale, color);
    }
  }
}

// Splash geometry, kept in one place so the caption strip and the mark cannot
// drift into each other. The mark is 88x148 px at scale 2, so it ends at y=176;
// the caption strip starts well below that.
#define SPLASH_MARK_TOP   28
#define SPLASH_MARK_SCALE  2
#define SPLASH_CAP_Y     196
#define SPLASH_CAP_BAND_Y  190
#define SPLASH_CAP_BAND_H   26

void gfxSplashCaption(const char* caption) {
  if (!gfx) return;
  // Clear only the strip under the mark. A fillScreen here would wipe the mark
  // and reintroduce the flicker this exists to avoid.
  gfx->fillRect(0, SPLASH_CAP_BAND_Y, TFT_WIDTH, SPLASH_CAP_BAND_H, C_BLACK);
  if (caption && caption[0])
    gfxDrawCentered(caption, SPLASH_CAP_Y, gfxFitSize(caption, 232, 2), C_GRAY);
}

void gfxSplash(const char* caption) {
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  drawMark(SPLASH_MARK_TOP, SPLASH_MARK_SCALE, C_WHITE);
  gfxSplashCaption(caption);
}

void gfxBoot(const char* line1, const char* line2) {
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  gfxDrawCentered(line1, 95, 3, C_WHITE);
  if (line2 && line2[0]) gfxDrawCentered(line2, 130, 2, C_GRAY);
}

// Where the unit's own web UI lives once it is on a real network. Never with an
// "http://" prefix: that turns a 16-character name into 23 chars = 276 px, over
// the 232 px budget, and the line silently loses its size-2 rendering. Phones and
// desktop browsers both resolve a bare host, so the prefix bought nothing.
static void hostUrl(char* out, size_t n, const char* host) {
  snprintf(out, n, "%s.local", host);
}

// Setup-mode screen. Two layouts, because the geometry genuinely differs: the
// default AP is open (DEFAULT_AP_PASS is empty) and gets the roomier spacing,
// while a password needs two more rows paid for out of the margins.
//
// The SSID is the largest thing on the screen after the title, at text size 3,
// directly under an imperative that names it. That pairing IS the answer to a
// room of thirty units: they all broadcast names of identical shape differing
// only in four hex characters, and the only reliable way to pick yours is to read
// it off the unit in your hand. At the old size 1 the caps are 0.65 mm on 39 mm
// of glass — a 13 cm read. At size 3 they are 3.9 mm, which is arm's length.
// There is no vertical budget left for a sentence about it; the portal's first
// card carries the explicit "if this name is not on the screen in front of you,
// you have joined someone else's device".
void gfxApInfo(const char* ssid, const char* pass, const char* ip, const char* host) {
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  const bool locked = pass && pass[0];

  gfxDrawCentered("SET ME UP", locked ? 6 : 10, 3, C_YELLOW);          // 9 x 6 x 3 = 162
  gfxDrawCentered("Join this WiFi:", locked ? 38 : 46, 2, C_GRAY);     // 15 x 6 x 2 = 180
  // gfxFitSize, not gfxFitSizeMin: this line can never be dropped, it is the
  // whole point of the screen. "Clawd-a1b2" is 10 x 6 x 3 = 180 px, so a default
  // name lands at size 3. A hand-set name too long even for 6x8 clips at the
  // panel edge (setTextWrap is off in gfxBegin) — self-inflicted, and not the
  // giveaway case.
  gfxDrawCentered(ssid, locked ? 58 : 68, gfxFitSize(ssid, 232, 3), C_WHITE);

  if (locked) {
    gfxDrawCentered("Password:", 88, 1, C_GRAY);                       // 9 x 6 x 1 = 54
    gfxDrawCentered(pass, 100, gfxFitSize(pass, 232, 2), C_WHITE);     // <=19 ch at size 2
    gfx->fillRect(8, 124, 224, 2, C_DGRAY);
  } else {
    gfxDrawCentered("(no password)", 98, 2, C_GRAY);                   // 13 x 6 x 2 = 156
    gfx->fillRect(8, 122, 224, 2, C_DGRAY);
  }

  gfxDrawCentered("Then open:", 134, 2, C_GRAY);                       // 10 x 6 x 2 = 120
  // The bare address, never "http://" + ip: that is 22 chars for a worst-case
  // IPv4 = 264 px at size 2, so the single most important string on the screen
  // used to fall silently to 6x8. "192.168.4.1" is 11 x 6 x 3 = 198 px, and the
  // longest possible IPv4 still fits at size 2 (15 x 6 x 2 = 180).
  gfxDrawCentered(ip, locked ? 154 : 156, gfxFitSize(ip, 232, 3), C_GREEN);
  gfx->fillRect(8, locked ? 186 : 190, 224, 2, C_DGRAY);

  // Where the UI lives after setup. Dropped whole, label included, when even 6x8
  // will not fit — that takes a hostname over 32 characters, and anyone who set
  // one of those can find the page again without being told.
  if (host && host[0]) {
    char url[64];
    hostUrl(url, sizeof(url), host);
    uint8_t sz = gfxFitSizeMin(url, 232, 2, 1);
    if (sz) {
      gfxDrawCentered("After setup:", locked ? 196 : 200, 1, C_GRAY);  // 12 x 6 x 1 = 72
      gfxDrawCentered(url, locked ? 210 : 214, sz, C_GRAY);            // 16 x 6 x 2 = 192
    }
  }
}

void gfxStaInfo(const char* ssid, const char* ip, const char* host) {
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  gfxDrawCentered("CONNECTED", 18, 3, C_GREEN);                        // 9 x 6 x 3 = 162
  gfxDrawCentered("Network:", 62, 2, C_GRAY);                          // 8 x 6 x 2 = 96
  // Measure the string that actually gets drawn. This used to size `ssid` while
  // printing "-" for a null one: the wrong width, and a strlen on a null pointer.
  const char* nm = (ssid && ssid[0]) ? ssid : "-";
  gfxDrawCentered(nm, 84, gfxFitSize(nm, 232, 3), C_WHITE);            // 32-ch SSID -> size 1
  gfxDrawCentered("Open in browser:", 126, 2, C_GRAY);                 // 16 x 6 x 2 = 192
  // IP big (worst-case IPv4 is 15 x 6 x 2 = 180); mDNS name below as the
  // friendlier option, dropped rather than clipped when the hostname is absurd.
  const char* addr = (ip && ip[0]) ? ip : "-";
  gfxDrawCentered(addr, 150, gfxFitSize(addr, 232, 3), C_GREEN);
  if (host && host[0]) {
    char url[64];
    hostUrl(url, sizeof(url), host);
    uint8_t sz = gfxFitSizeMin(url, 232, 2, 1);
    if (sz) gfxDrawCentered(url, 188, sz, C_GRAY);                     // 16 x 6 x 2 = 192
  }
}

// Persistent crash screen shown in safe mode (after an exception reset). Holds the
// crash PC + fault address still so they can be read, and the IP for OTA recovery.
void gfxCrash(const char* epc, const char* addr, const char* ip) {
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  gfxDrawCentered("CRASH", 12, 4, C_RED);
  gfxDrawCentered("epc", 60, 2, C_GRAY);
  gfxDrawCentered(epc && epc[0] ? epc : "-", 80, 3, C_WHITE);
  gfxDrawCentered("addr", 124, 2, C_GRAY);
  gfxDrawCentered(addr && addr[0] ? addr : "-", 146, 2, C_WHITE);
  gfxDrawCentered("OTA flash to fix:", 182, 2, C_GRAY);
  gfxDrawCentered(ip && ip[0] ? ip : "-", 204, 2, C_GREEN);
}
