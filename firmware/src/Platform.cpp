// Platform.cpp — the few things that need a real definition (not header-inline)
// but are still core-specific. Keeps the SNTP callback wiring out of the feature
// code, same as the inline shims in Platform.h.
#include "Platform.h"
#include <coredecls.h>

static void (*s_syncCb)() = nullptr;

// settimeofday_cb fires whenever the clock is set; from_sntp distinguishes an
// SNTP update (what we care about) from a manual settimeofday (which we never do).
static void sntpSyncNotify(bool from_sntp) { if (from_sntp && s_syncCb) s_syncCb(); }

void platformOnTimeSync(void (*cb)()) {
  s_syncCb = cb;
  settimeofday_cb(sntpSyncNotify);
}
