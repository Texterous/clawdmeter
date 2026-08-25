// Fmt.h — duration formatting shared by the meter screens.
#pragma once
#include <Arduino.h>

// Minutes -> a label that never exceeds 6 characters, which is what the
// right-hand column of both the usage cards and the session rows can hold:
//   "45m"  "3h 07m"  "2d 4h"
// `zero` is what sub-minute means in context — a reset countdown reads "now",
// an elapsed timer reads "<1m".
inline void fmtDuration(int mins, char* out, size_t n, const char* zero = "now") {
  if (mins <= 0) { strlcpy(out, zero, n); return; }
  int d = mins / 1440, h = (mins % 1440) / 60, m = mins % 60;
  if (d > 0)      snprintf(out, n, "%dd %dh", d, h);
  else if (h > 0) snprintf(out, n, "%dh %02dm", h, m);
  else            snprintf(out, n, "%dm", m);
}

// The characters /clawd:setup asks for: the tail of the hostname. Every default
// name ends in the 4-hex chip suffix (Settings::setDefaults), which is what makes
// one unit tellable from thirty on the same table, and it is what the plugin's
// finder matches on — so the screen prints exactly that string rather than asking
// anyone to count backwards through "clawd-a1b2.local". A hand-set name shorter
// than four characters yields the whole of it, which is still the right answer.
inline void fmtDeviceCode(const char* host, char* out, size_t n) {
  size_t len = host ? strlen(host) : 0;
  strlcpy(out, len > 4 ? host + len - 4 : (host ? host : ""), n);
}
