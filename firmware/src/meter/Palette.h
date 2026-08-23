// Palette.h — the Claude-usage colours, shared by every meter screen.
//
// Anthropic-inspired dark theme, RGB565 of the originals. Everything goes
// through gfxTint() like the shared palette in Gfx.h, so the Display tab's
// colour correction reaches these too — panels vary between units and a screen
// that hardcodes raw RGB565 is the one that looks wrong on half the batch.
#pragma once
#include "Gfx.h"

#define C_ACCENT  gfxTint(0xDBAA)   // terra-cotta 0xd97757
#define C_UGREEN  gfxTint(0x7C6B)   // green 0x788c5d
#define C_PANEL   gfxTint(0x18E3)   // card fill 0x1f1f1e
#define C_BARBG   gfxTint(0x2945)   // unfilled bar track 0x2a2a28
#define C_DIM     gfxTint(0xB574)   // secondary text 0xb0aea5

// Amber for the "waiting on you" lamp. C_YELLOW (0xFFE0) is too acid to read as
// a warning next to C_RED — it fights the red instead of sitting below it.
#define C_AMBER   gfxTint(0xFD20)   // 0xffa500
