// UsageData.h — runtime (volatile) Claude usage snapshot from the daemon.
#pragma once
#include <Arduino.h>
#include "config.h"

// One row of the session board. The daemon has already done the work: it found
// the live sessions, classified each, sorted them most-urgent-first and clipped
// the names to what the panel can render. The device only draws.
enum SessionState : uint8_t {
  SESS_WORKING = 0,   // the model holds the turn
  SESS_BLOCKED,       // stalled mid-tool-call — usually a permission prompt
  SESS_AWAITING,      // turn over, waiting for a human
};

struct SessionInfo {
  char     name[SESSION_NAME_LEN + 1];
  uint8_t  state;     // SessionState
  uint16_t mins;      // minutes in that state
};

struct UsageData {
  float    sessionPct;       // 5-hour window utilization (0..100)
  int      sessionResetMin;  // minutes until the 5-hour window resets
  float    weeklyPct;        // 7-day window utilization (0..100)
  int      weeklyResetMin;   // minutes until the 7-day window resets
  char     status[16];       // e.g. "allowed", "allowed_warning", "rejected"

  // Session board. Absent from older daemons, hence boardValid: an empty board
  // ("nothing running") and no board at all ("your daemon is too old") are
  // different things and the screen says so.
  SessionInfo sessions[MAX_SESSION_ROWS];
  uint8_t     sessionRows;   // rows populated in sessions[]
  uint8_t     sessionLive;   // sessions alive on the host; may exceed sessionRows
  bool        boardValid;    // the payload carried a "sess" array

  bool     valid;            // populated at least once
  bool     error;            // most recent fetch failed
  uint32_t lastOkMs;         // millis() of last good update

  void clear() {
    sessionPct = weeklyPct = 0;
    sessionResetMin = weeklyResetMin = 0;
    status[0] = 0;
    sessionRows = sessionLive = 0;
    boardValid = false;
    valid = false;
    error = false;
    lastOkMs = 0;
  }
};
