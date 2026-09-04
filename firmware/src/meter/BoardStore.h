// BoardStore.h — the board survives a reboot, and the device can ask for a new one.
//
// Two jobs, both aimed at one screen nobody should ever see: the empty one.
//
//   1. PERSIST. The live snapshot is RAM (UsageData), so every power cycle used to
//      start from nothing and the panel sat on "waiting..." until the next hook
//      happened to fire — which, with Claude Code closed, is never. The last
//      accepted payload is kept in /board.json and restored at boot, so a
//      rebooted unit comes up showing what it knew, marked as history.
//
//   2. POKE. Every push tells the device the sender's address for free (the source
//      IP of the POST), and the payload carries the port its listener is on. So a
//      unit that wants fresh data does not have to wait to be found: it asks. One
//      GET at boot turns a reboot into about a second of stale board instead of
//      up to a heartbeat, and the same call every minute while nothing is
//      arriving is what recovers a laptop that woke up on a new address.
//
// Both are best-effort by design. No sender, unparseable file, poke refused —
// each leaves the device exactly where it was, which is showing the last board it
// had rather than an error about the one it does not.
#pragma once
#include <Arduino.h>
#include "Settings.h"

// Restore the last board into the live snapshot. Call once at boot, before the
// mode's begin(). True when something was restored.
bool   boardStoreBegin();

// A payload was accepted. Records body and sender in RAM and marks it dirty; the
// flash write is deferred to boardStoreService, out of the HTTP response.
void   boardStoreNote(const String& body, const String& senderIp, uint16_t senderPort);

// Flush a changed board, and poke the sender when nothing is arriving. Loop().
void   boardStoreService(const Settings& s);

// Ask the sender to push now. No-op without a remembered address.
void   boardStorePoke();

bool   boardStoreHasSender();
String boardStoreSender();     // "10.0.0.5:8788", or "" when none is known
void   boardStoreForget();     // factory reset drops the board with the config
