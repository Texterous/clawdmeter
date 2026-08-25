// Gfx.h — shared ST7789 device, drawing primitives, and boot/status screens.
//
// This is the core display layer. The meter renders on top of it via gfxDev()
// and the exposed text helpers. Nothing feature-specific lives here.
#pragma once
#include <Arduino.h>
#include "Settings.h"

class Arduino_GFX;   // fwd-decl: only the drawing .cpp files pull in the full lib

// ---- Shared colors (RGB565) ----------------------------------------------
// The SmallTV variants ship visibly different panels: some run warm, some cold,
// and a few have red and blue swapped in the controller. gfxTint() applies the
// per-channel gain set in the Display tab to every colour on its way to the
// panel, so a unit can be matched to the others without touching each renderer.
// Colours computed inside the meter go through it too (see SessionsMode).
uint16_t gfxTint(uint16_t rgb565);

#define C_BLACK  gfxTint(0x0000)
#define C_WHITE  gfxTint(0xFFFF)
#define C_GREEN  gfxTint(0x07E0)
#define C_RED    gfxTint(0xF800)
#define C_GRAY   gfxTint(0x8410)
#define C_DGRAY  gfxTint(0x4208)
#define C_YELLOW gfxTint(0xFFE0)
#define C_BLUE   gfxTint(0x041F)

// ---- Device lifecycle -----------------------------------------------------
void         gfxBegin(const Settings& s);
void         gfxSetBrightness(uint8_t pct, bool inverted);
void         gfxSetRotation(uint8_t r);
// Push the Display tab's colour settings to the panel: MADCTL colour order,
// the inversion bit, and the per-channel gain gfxTint() applies. Callers repaint
// afterwards — already-drawn pixels keep the previous correction.
void         gfxApplyColors(const Settings& s);
Arduino_GFX* gfxDev();                 // shared draw target for feature renderers

// ---- Text helpers (built-in 6x8 font, integer scaled) ---------------------
#define GFX_FONT_W 6
#define GFX_FONT_H 8

int     gfxTextW(const char* s, uint8_t size);
void    gfxDrawCentered(const char* s, int y, uint8_t size, uint16_t color);
// Largest size that fits, never below 1. For lines that cannot be dropped: 6x8
// beats nothing, even when 6x8 is barely readable.
uint8_t gfxFitSize(const char* s, int maxW, uint8_t maxSize);
// Same, but returns 0 when even minSize overflows maxW, so the caller can drop
// or truncate the line instead of drawing something illegible. Use this for any
// line the screen can live without.
uint8_t gfxFitSizeMin(const char* s, int maxW, uint8_t maxSize, uint8_t minSize);

// ---- Shared boot / status / diagnostic screens ----------------------------
// Brand splash: the Texterous mark (src/splash/logo.h) over a caption. This is
// the first paint at boot and it stays up for the whole of it — every boot
// progress line goes through gfxSplashCaption, which repaints ONLY the caption
// strip and leaves the mark alone. Calling gfxBoot during boot instead would
// fillScreen and wipe the mark after a few milliseconds.
void gfxSplash(const char* caption);
void gfxSplashCaption(const char* caption);
void gfxBoot(const char* line1, const char* line2);
// Setup mode. `host` is the mDNS name the unit will answer to once it is on a
// real network — the address a recipient needs AFTER this screen is gone, which
// is the one thing the old three-argument version could not tell them.
void gfxApInfo(const char* ssid, const char* pass, const char* ip, const char* host);
void gfxStaInfo(const char* ssid, const char* ip, const char* host);
void gfxCrash(const char* epc, const char* addr, const char* ip);  // safe-mode diag
