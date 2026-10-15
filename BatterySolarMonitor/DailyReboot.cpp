// DailyReboot.cpp  file  =================
#include "DailyReboot.h"
#include "LocalTimeMinutes.h"
#include "ntpSupport.h"
#include <Arduino.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// -1 = "haven't seen a real reading yet this boot" -- the very first
// post-sync check just records lastMinutes without comparing, so a device
// that happens to boot right after midnight doesn't mistake its own first
// reading for a rollover and reboot-loop. Every check after that compares
// against the PREVIOUS call's minutes-since-midnight: a rollover is
// exactly "today's reading is smaller than last time's" (23:59 -> 00:00
// wraps 1439 down to ~0), which fires exactly once per real midnight and
// -- critically -- can never fire again right after the reboot it just
// triggered, since the fresh boot session starts this back at -1 with no
// "previous" to compare against yet.
static int lastMinutes = -1;

// A plain "did minutes decrease" check also fires on the once-a-year DST
// autumn fallback (AEDT->AEST, ~April): the clock itself jumps backwards
// by 60 minutes around 2-3am (localtime() already applies this via the
// TZ string ntpSupport.cpp sets -- see setTZfromPOSIXstr()), which reads
// as a genuine decrease in minutes-since-midnight but is NOT a midnight
// rollover. The spring-forward transition (AEST->AEDT, ~October) only
// ever jumps the clock FORWARD, so it can't cause a false trigger here.
// A real midnight wrap is always a much larger drop (up to ~1439, down to
// whatever the previous check happened to catch) than a 60-minute DST
// fallback, so requiring the drop to exceed this margin tells the two
// apart regardless of exactly when each check happens to run.
static const int MIN_ROLLBACK_MINUTES = 90;

void checkDailyReboot() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  if (missedSNTPupdate()) {
    return; // need a real clock before "minutes since midnight" means anything
  }

  int nowMinutes = localTimeMinutesSinceMidnight();
  if (lastMinutes >= 0 && (lastMinutes - nowMinutes) > MIN_ROLLBACK_MINUTES) {
    if (debugPtr) {
      debugPtr->println(F("DailyReboot: midnight rollover -- rebooting for routine cleanup"));
    }
    delay(200); // give the debug line above time to actually reach Serial before reset
    ESP.restart();
  }
  lastMinutes = nowMinutes;
}
// ================= end of DailyReboot.cpp  file
