// UsageMode.h — Claude usage meter feature.
//
// Shows 5h/7d usage bars + a small mascot when data is flowing, and an animated
// pixel-art mascot when the daemon goes quiet. Owns its fetch (UsageClient), its
// mascot animation (Mascot) and its render/dirty state.
#pragma once
#include "Mode.h"
#include "config.h"

// One usage card in the exact form it reaches the panel: the printed integer, the
// bar's width in pixels, its colour, and the minute value the countdown string is
// built from. The renderer takes this instead of the raw UsageData, so the change
// diff and the draw cannot disagree about what "changed" means — a value that is
// drawn is a member, and a member is diffed. See usageFrame() in UsageMode.cpp.
struct UsageCard {
  int32_t  pct;         // printed percentage, already rounded and clamped
  int32_t  fill;        // bar fill width in px
  int32_t  resetMins;   // fmtDuration input; minute-granular, so it ticks 1/min
  uint32_t color;       // bar colour, already tinted
};

// The whole stats screen in drawn form — the mode's record of what is on the
// glass. Compared with memcmp: every member is a 4-byte integer, so the struct
// has no padding to leave indeterminate and no float equality to reason about.
struct UsageFrame {
  UsageCard card[2];    // 0 = the 5h window, 1 = the 7d window
  uint32_t  header;     // bit 0: the status flag dot   bit 1: the STALE title
};

class UsageMode : public DisplayMode {
 public:
  const char* id() const override { return "usage"; }
  uint8_t     modeConst() const override { return MODE_USAGE; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  // Another mode owned the panel, so nothing on it is ours. Clearing primed_ and
  // showingMascot_ as well as needRender_ is what forces a full repaint: the old
  // version set needRender_ alone, which the mascot branch never reads — a wake
  // while the animation was up left it patching cells into somebody else's screen.
  void wake(const Settings& s) override {
    needRender_ = true; primed_ = false; showingMascot_ = false;
  }

 private:
  uint32_t   usageSampled_ = 0;      // lastOkMs already fed to the mascot tracker
  uint32_t   footerMs_ = 0;          // last idle-footer poll (netIP() allocates)
  UsageFrame shown_ = {};            // what the panel is showing; written only after a draw
  bool       primed_ = false;        // shown_ describes real pixels, so a partial repaint is safe
  bool       showingMascot_ = false;
  bool       needRender_ = true;
};

extern UsageMode g_usageMode;
