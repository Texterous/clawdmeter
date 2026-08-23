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
  void wake(const Settings& s) override { needRender_ = true; }  // repaint only

 private:
  uint32_t renderedOk_ = 0xFFFFFFFF;   // lastOkMs already on screen
  bool     showedStale_ = false;
  bool     needRender_ = true;
};

extern SessionsMode g_sessionsMode;
