// Commission.h — the "on WiFi, never fed" screen.
//
// The one screen between joining a network and running the sender on a computer.
// It has no timeout on purpose: the address it carries is the only copy anyone
// gets. main.cpp's gfxStaInfo flash is 3.5 s long and lands exactly while the
// user is head-down in their phone's WiFi settings getting back onto their own
// network — which is how the address used to be lost for good.
#pragma once
#include "Settings.h"

void commissionService(const Settings& s);   // each loop tick while !commissioned
void commissionInvalidate();                 // something else drew over us
