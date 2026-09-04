// UsageClient.h — pulls Claude usage from the local daemon's HTTP endpoint.
//
// The companion daemon (see daemon/) polls the Claude API rate-limit headers and
// serves the latest snapshot as a tiny JSON object. The device GETs that URL on
// its poll schedule — exactly like the stock webhook, but a different contract.
#pragma once
#include "Settings.h"
#include "UsageData.h"

void usageInit(const Settings& s);
void usageService(const Settings& s);     // call each loop; fetches on the poll schedule
void usageForceRefresh();                 // poll again on the next service() call
const UsageData& usageGet();
bool usageFresh(uint32_t withinMs);       // true if the last good update is recent enough

// How long a reading stays good. Three regimes, and the same
// `usageUrl.length() >= 8` test that decides whether to fetch at all picks the
// first of them:
//   pull — the device fetches every pollSec, so twice that plus a grace period
//          means "we missed two in a row".
//   push, sender declared a heartbeat — trust that many missed beats. This is the
//          agent, and it is why a closed lid now reads as quiet within a minute.
//   push, no heartbeat declared — the hook-only reporter, which speaks on events
//          and is legitimately silent while somebody reads. See PUSH_STALE_MS.
// Not inline any more: the middle case reads the last payload.
uint32_t usageStaleMs(const Settings& s);

// Apply a usage payload PUSHED to the device (POST /api/usage) — used when the
// device can't reach the daemon (Wi-Fi client isolation) so the daemon pushes.
bool usageApply(const String& body);      // parse {s,sr,w,wr,st,ok}; true on success

// Same parse, for the copy BoardStore kept across the reboot. Identical contract,
// one difference that is the whole point: the result is marked `restored`, so it
// draws but never counts as fresh.
bool usageApplyRestored(const String& body);
