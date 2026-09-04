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

  // Session board. Absent from a sender that does not build one, hence
  // boardValid: an empty board ("nothing running") and no board at all
  // ("UPDATE YOUR SENDER") are different things and the screen says so.
  SessionInfo sessions[MAX_SESSION_ROWS];
  uint8_t     sessionRows;   // rows populated in sessions[]
  uint8_t     sessionLive;   // sessions alive on the host; may exceed sessionRows
  bool        boardValid;    // the payload carried a "sess" array

  // Whether the payload carried 5h/7d numbers at all. A board-only sender —
  // the plugin's hook, which cannot see rate limits — sets boardValid without
  // this, and the sessions footer then omits the 5H WINDOW row rather than
  // drawing a confident 0%.
  bool        usageValid;

  bool     valid;            // populated at least once
  bool     error;            // most recent fetch failed
  uint32_t lastOkMs;         // millis() of last good update

  // Restored from flash at boot rather than received on this boot. The rows are
  // real and worth drawing — they are what the device knew when it lost power —
  // but they are history, so usageFresh() refuses them and the screen labels
  // them. Cleared by the first payload that actually arrives.
  bool     restored;

  // When the sender says it built this payload: UTC epoch seconds, plus the
  // sender's own offset from it in minutes. Both optional.
  //
  // This is deliberately the sender's clock and not the device's. The panel needs
  // a wall-clock time for one line of text ("LAST SEEN 09:12"), and getting one
  // out of SNTP means a working NTP path, a POSIX TZ rule the recipient never set,
  // and a DST database — on a device that is already talking to a computer which
  // knows the answer. So the answer comes with the data.
  uint32_t stampEpoch;
  int16_t  stampTzOffMin;

  // The sender's heartbeat, in seconds, and the port its listener is on. Both
  // optional: zero means "not declared" and the device falls back to the
  // conservative defaults in config.h.
  uint16_t heartbeatSec;
  uint16_t senderPort;

  void clear() {
    sessionPct = weeklyPct = 0;
    sessionResetMin = weeklyResetMin = 0;
    status[0] = 0;
    sessionRows = sessionLive = 0;
    boardValid = false;
    usageValid = false;
    valid = false;
    error = false;
    lastOkMs = 0;
    restored = false;
    stampEpoch = 0;
    stampTzOffMin = 0;
    heartbeatSec = 0;
    senderPort = 0;
  }

  // Local wall-clock seconds-since-midnight the sender stamped this payload with,
  // or -1 when it carried no stamp. Modulo arithmetic on the sender's own local
  // epoch: no time zone rule, no NTP, and correct across DST because the offset
  // came from the machine that observes it.
  int32_t stampLocalDaySec() const {
    if (!stampEpoch) return -1;
    int32_t local = (int32_t)(stampEpoch % 86400UL) + (int32_t)stampTzOffMin * 60;
    while (local < 0)      local += 86400;
    while (local >= 86400) local -= 86400;
    return local;
  }
};
