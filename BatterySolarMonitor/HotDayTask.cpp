// HotDayTask.cpp  file  =================
#include "HotDayTask.h"
#include "ChargeFromGridFetch.h"
#include "ChargeFromGridState.h"
#include "HotDayState.h"
#include "DischargeToGridState.h" // autoDischargeEnabled() -- the shared master switch
#include "LocalTimeMinutes.h" // localTimeMinutesSinceMidnight()
#include "ntpSupport.h"       // missedSNTPupdate()
#include <Arduino.h>
#include <pfodDelay.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// Same 5min cadence as DischargeToGridTask.cpp's own
// STATUS_POLL_INTERVAL_MS/STATUS_RETRY_MS, same reasoning.
static const unsigned long STATUS_POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;
static const unsigned long STATUS_RETRY_MS = 30UL * 1000UL;

// Only ever touched from this file's own background-task tick -- no lock
// needed, same reasoning as DischargeToGridTask.cpp's own statusPollTimer.
static pfodDelay statusPollTimer;

// One-shot "force Enphase's chargeFromGrid off now" request -- same shape
// as DischargeToGridTask.cpp's own disablePending: NOT retried on its own,
// fired exactly once, optimistically assumed to have worked, and left for
// the NEXT status poll (runHotDayStatusPoll() below) to actually verify/
// resend if it didn't land. Set by requestChargeFromGridDisableNow()
// (pfodMainMenuApp.cpp's onAutoBatteryChargeDisPressed(), a genuinely
// different task from this one's own tick) -- every access goes through
// mutex(), same two-caller situation as disablePending's own comment
// describes.
static bool disablePending = false;

static SemaphoreHandle_t taskMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (taskMutex == NULL) {
    taskMutex = xSemaphoreCreateMutex();
  }
  return taskMutex;
}

static Print *dbg() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  return debugPtr;
}

// Returns true on success (and publishes the result via
// setCfgPolledStatus()) so checkHotDayPoll() below can pick
// STATUS_RETRY_MS instead of the full STATUS_POLL_INTERVAL_MS on failure.
// Resends whenever Enphase's own chargeFromGrid flag doesn't match what
// hotDayActive() says it should be -- covers BOTH directions (the one-shot
// disablePending request above, and HotDayState.h's own hotDayActive()
// being the ordinary ongoing target the rest of the time), same single
// "wantedOn" comparison either way, since disablePending only ever means
// "force off", which is also what hotDayActive()==false already wants.
static bool runHotDayStatusPoll() {
  bool enabled;
  int startMinutes, endMinutes;
  float targetSoc;
  if (!fetchChargeFromGridStatus(enabled, startMinutes, endMinutes, targetSoc)) {
    if (debugPtr) {
      debugPtr->println(F("HotDayTask: cfg status poll failed -- leaving last-known display state"));
    }
    return false;
  }
  setCfgPolledStatus(enabled, startMinutes, endMinutes, targetSoc);

  bool wantedOn = hotDayActive();
  if (enabled != wantedOn) {
    if (debugPtr) {
      debugPtr->print(F("HotDayTask: Enphase chargeFromGrid reports "));
      debugPtr->print(enabled ? F("on") : F("off"));
      debugPtr->print(F(" but hotDayActive wants it "));
      debugPtr->print(wantedOn ? F("on") : F("off"));
      debugPtr->println(F(" -- resending"));
    }
    // Optimistic -- set BEFORE the network call below, same reasoning as
    // DischargeToGridTask.cpp's own setDtgPolledEnabledAssumed() call
    // sites (that call's own GET/POST/PUT chain can take several seconds;
    // no reason to leave the display on the stale reading for that whole
    // round trip when the outcome is being assumed anyway).
    setCfgPolledEnabledAssumed(wantedOn);
    setChargeFromGridEnabled(wantedOn);
  }
  return true;
}

void checkHotDayPoll() {
  dbg();

  bool runDisableNow = false;
  xSemaphoreTake(mutex(), portMAX_DELAY);
  if (disablePending) {
    disablePending = false; // one-shot
    runDisableNow = true;
  }
  xSemaphoreGive(mutex());
  if (runDisableNow) {
    setCfgPolledEnabledAssumed(false);
    bool ok = setChargeFromGridEnabled(false);
    if (debugPtr) {
      debugPtr->println(ok ? F("HotDayTask: chargeFromGrid disable request sent and confirmed by the PUT response")
                           : F("HotDayTask: chargeFromGrid disable request sent but not confirmed -- assuming it worked anyway, next status poll will check"));
    }
  }

  if (!statusPollTimer.isRunning()) {
    statusPollTimer.start(0); // due immediately the first time this is ever ticked
  }
  if (statusPollTimer.justFinished()) {
    bool ok = runHotDayStatusPoll();
    statusPollTimer.start(ok ? STATUS_POLL_INTERVAL_MS : STATUS_RETRY_MS);
  }
}

void requestChargeFromGridDisableNow() {
  xSemaphoreTake(mutex(), portMAX_DELAY);
  disablePending = true;
  xSemaphoreGive(mutex());
}

// Re-check cadence for the forecast test itself (not the once-a-day
// decision gate below) -- same 5min cadence DischargeToGridTask.cpp's own
// LOGIC_INTERVAL_MS uses, same "early-exit retries at 0" shape for the
// exact same reason (a blocked attempt during startup must not consume
// the throttle window and delay the first real check by up to 5 more
// minutes once the data actually becomes ready).
static const unsigned long LOGIC_INTERVAL_MS = 5UL * 60UL * 1000UL;

// logicTimer -- only ever touched from this file's own background-task
// tick, no lock needed, same reasoning as statusPollTimer above.
// tomorrowHotDayDecided -- true once TODAY's 2:02pm-trigger decision
// about TOMORROW's Hot Day status has been applied; stops this
// re-deciding on every tick for the rest of today once that's happened.
// Named for what it tracks, not just "once per day" -- the decision made
// at today's trigger is always about tomorrow's forecast
// (readHotDayForecast()), never today's. Cleared back to false by a plain
// time comparison (the `nowMinutes < triggerMinutes` branch below, every
// day before its own trigger time is reached) rather than relying on this
// device's own daily midnight reboot (DailyReboot.h) to reset it -- unlike
// DischargeToGridTask.cpp's own haveDtgDecision/lastDtgDecision (which
// don't need a daily reset for correctness, just a clean slate), THIS
// flag is the only thing standing between "decide once" and "decide
// forever", so relying on a reboot that might not happen on schedule
// would leave Hot Day permanently stuck after the first decision if one
// was ever missed. Also cleared by the master switch being re-enabled
// after being disabled (see the disabled branch below).
static pfodDelay logicTimer;
static bool tomorrowHotDayDecided = false;

void checkHotDayLogic() {
  dbg();

  if (!autoDischargeEnabled()) {
    // Master switch off -- per explicit instruction, stop checking
    // entirely. The immediate "disable both and revert HW/Car% to
    // default" transition itself is pfodMainMenuApp.cpp's own job
    // (onAutoBatteryChargeDisPressed(), the instant the switch is turned
    // off) -- this function just has to stay out of the way while
    // disabled, and start completely fresh the next time it's enabled
    // again, same shape as DischargeToGridTask.cpp's own disabled branch.
    tomorrowHotDayDecided = false;
    logicTimer.stop(); // isRunning()-gated re-arm below fires fresh once re-enabled
    return;
  }

  if (!logicTimer.isRunning()) {
    logicTimer.start(0); // due immediately the first time this is ticked, or just after being re-enabled
  }
  if (!logicTimer.justFinished()) {
    return;
  }

  // From here on, every early exit re-arms at 0 (retry next tick), NOT
  // LOGIC_INTERVAL_MS -- same "don't let a blocked attempt during startup
  // delay the first real check" fix DischargeToGridTask.cpp's own
  // checkDischargeToGridLogic() and BatteryProjection.cpp both already
  // apply for the identical reason.
  if (missedSNTPupdate()) {
    logicTimer.start(0); // need a real "now" -- not a real attempt yet
    return;
  }
  int nowMinutes = localTimeMinutesSinceMidnight();

  int cfgEndMinutes = cfgScheduleEndMinutes();
  if (cfgEndMinutes < 0) {
    logicTimer.start(0); // cfg schedule not polled yet -- nothing to compute today's trigger time from
    return;
  }
  int triggerMinutes = cfgEndMinutes + 2; // "2mins after the charge from grid schedule ends", per explicit instruction

  if (nowMinutes < triggerMinutes) {
    // Not yet today's trigger time -- also where tomorrowHotDayDecided
    // gets cleared for the day, rather than relying on a reboot to reset
    // it (see that flag's own comment, further up): every day starts out
    // here, before its own trigger time, so clearing it on every tick in
    // this branch is equivalent to clearing it once at the start of the
    // day, but without needing to detect "day just changed" explicitly --
    // and it's a no-op once already false, so this costs nothing extra.
    tomorrowHotDayDecided = false;
    logicTimer.start(LOGIC_INTERVAL_MS); // real cadence, keep testing until the trigger time arrives
    return;
  }

  if (tomorrowHotDayDecided) {
    logicTimer.start(LOGIC_INTERVAL_MS); // already decided tomorrow's Hot Day status at today's trigger -- nothing more until tomorrow's own trigger
    return;
  }

  bool isHot;
  if (!readHotDayForecast(isHot)) {
    logicTimer.start(0); // forecast not ready yet -- retry soon, don't miss today's trigger window
    return;
  }

  logicTimer.start(LOGIC_INTERVAL_MS); // a real decision follows -- real cadence from here
  tomorrowHotDayDecided = true;

  if (debugPtr) {
    debugPtr->print(F("HotDayTask: 2:02pm trigger reached, tomorrow forecast is "));
    debugPtr->print(isHot ? F("HOT") : F("not hot"));
    debugPtr->println(isHot ? F(" -- switching to Hot Day HW/Car% and enabling charge-from-grid")
                            : F(" -- reverting to default HW/Car% and disabling charge-from-grid"));
  }

  // Persists; no-op if already matching (e.g. a hot day followed by
  // another hot day). effectiveHwSocOnPercent()/effectiveEvSocOnPercent()
  // (HotDayState.h, read by LoadControlTask.cpp) reflect this immediately.
  setHotDayActive(isHot);

  // Optimistic -- see setCfgPolledEnabledAssumed()'s own comment
  // (ChargeFromGridState.h). The recurring status poll above
  // (runHotDayStatusPoll()) is this call's own retry mechanism if it
  // doesn't land first try -- fire and forget here, same as
  // DischargeToGridTask.cpp's own checkDischargeToGridLogic() tail.
  setCfgPolledEnabledAssumed(isHot);
  setChargeFromGridEnabled(isHot);
}
// ================= end of HotDayTask.cpp  file
