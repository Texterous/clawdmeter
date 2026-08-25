// SessionsMode.h — the session board: what every live Claude session is doing.
//
// One row per session: a state dot, the session name, and how long it has been
// in that state. The daemon does the finding, classifying, sorting and clipping
// (see UsageData.h); this screen only draws what arrived.
//
// Shares UsageClient with UsageMode — both read the same pushed payload, so
// switching modes costs no extra traffic and no second fetch schedule.
#pragma once
#include "Mode.h"
#include "config.h"

class SessionsMode : public DisplayMode {
 public:
  const char* id() const override { return "sessions"; }
  uint8_t     modeConst() const override { return MODE_SESSIONS; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  // Another mode owned the panel. Clearing showedStale_ as well as needRender_ is
  // what gets the stale screen back up: its own guard would otherwise decide it
  // was already there and leave the other mode's pixels alone.
  void wake(const Settings& s) override { needRender_ = true; showedStale_ = false; }

 private:
  // A digest of what the board DRAWS, not of when a payload arrived — see
  // boardFingerprint(). Written only after a real draw, so it always describes the
  // pixels. It needs no "nothing yet" value: needRender_ starts true and forces
  // the first paint before the digest is ever trusted.
  uint32_t renderedFp_ = 0;
  bool     showedStale_ = false;
  bool     staleError_ = false;   // which of the two stale reasons is on screen
  bool     needRender_ = true;
};

extern SessionsMode g_sessionsMode;
