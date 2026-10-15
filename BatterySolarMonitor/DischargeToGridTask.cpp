// DischargeToGridTask.cpp  file  =================
#include "DischargeToGridTask.h"
#include "DischargeToGridFetch.h"
#include "DischargeToGridState.h"
#include "LocalTimeMinutes.h" // localTimeMinutesSinceMidnight()
#include "ntpSupport.h"       // missedSNTPupdate()
#include "PlotDataInstances.h" // tomorrowSolarData, tomorrowAvgConsumptionData, lockTomorrowForecastData()
#include "AvgConsumptionRefresh.h" // avgConsumptionForecastReady()
#include "SolarForecastRefresh.h"  // solarForecastReady()
#include "BatteryProjection.h" // requestBatteryProjectionRecompute()
#include "HotDayState.h" // readHotDayForecast() -- see checkDischargeToGridLogic()'s own hot-day gate
#include "WeatherForecastInstances.h" // tomorrowWeatherForecast -- see checkDischargeToGridLogic()'s own cold-day gate
#include "SolarForecastTask.h" // lockSolarForecast()/unlockSolarForecast() -- also guards tomorrowWeatherForecast
#include "CarChargerTask.h" // lockCarChargerState()/unlockCarChargerState()
#include "CarChargerInstances.h" // carChargingKwhToday
#include <Arduino.h>
#include <pfodDelay.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// Recurring status-poll cadence -- same 5min cadence this project's other
// POLL_INTERVAL_MS constants use (CarChargerTask.cpp/HotWaterTask.cpp/
// EnlightenLoginFetch.cpp's RETRY_INTERVAL_MS).
static const unsigned long STATUS_POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;

// Same role as GatewayPollTask.cpp's own FETCH_RETRY_DELAY_MS: a FAILED
// status poll retries much sooner than STATUS_POLL_INTERVAL_MS, instead of
// leaving the button's displayed schedule/will-vs-can text at its stale
// (or, right after boot, still-default placeholder) value for up to a
// full 5 minutes. Most likely to matter right after boot, before
// EnlightenLoginFetch.h's own login/validate cycle (which only starts at
// its own 15s uptime floor) has had a chance to run at all.
static const unsigned long STATUS_RETRY_MS = 30UL * 1000UL;

// Cold Day gate threshold (deg C) -- added 2026-10-14, per explicit
// instruction: tomorrow's forecast max temp at or below this is treated
// as a cold day for discharge-to-grid purposes (no excess solar expected
// worth exporting), same "keep discharge off" shape as the Hot Day gate
// just below it in checkDischargeToGridLogic(). A fixed constant, unlike
// BatteryChargeSettings.h's own adjustable tomorrowsMaxTempTriggerC()
// (Hot Day's analogous threshold) -- per explicit instruction, just
// "max temp <=18C", not a new settings-menu control.
static const float COLD_DAY_MAX_TEMP_C = 18.0;

// Minimum tomorrow's forecast solar excess (tomorrowSolarKwh minus
// avgConsumptionKwh) worth discharging the battery to the grid for --
// below this, skip discharge even though solar still exceeds consumption.
// Per explicit instruction ("tomorrows excess is <5kWh"), replacing the
// previous plain ">0" threshold. Fixed constant, same reasoning as
// COLD_DAY_MAX_TEMP_C above.
static const float MIN_TOMORROW_EXCESS_KWH = 5.0;

// Minimum car-charging energy already delivered TODAY
// (CarChargerInstances.h's carChargingKwhToday) for discharge-to-grid to
// be considered tonight. Per explicit instruction ("car charge is
// <10kWhr today"). Fixed constant, same reasoning as the two thresholds
// above.
static const float MIN_CAR_CHARGE_TODAY_KWH = 10.0;

// statusPollTimer is only ever touched from this file's own background-
// task tick, so it needs no lock -- isRunning() checked and acted on
// BEFORE justFinished() (the safe lazy-arm order; see EnlightenLoginFetch.cpp's
// own top-of-file comment on the hazard of doing it the other way around).
// Matches HotWaterTask.cpp's pollTimer idiom exactly.
static pfodDelay statusPollTimer;

// One-shot "send Enphase's own setting off" request -- NOT retried on its
// own. Fired exactly once, optimistically assumed to have worked (per
// explicit instruction), and left for the NEXT ~5min status poll
// (runStatusPoll() below) to actually verify: if that poll finds Enphase
// still reporting dtgControl.enabled true while the button wanted it off,
// it resends the same disable command -- again just assumed to have
// worked, so the whole thing self-corrects every ~5min for as long as the
// mismatch persists, rather than a dedicated fast retry loop.
//
// Set true at startup (the mandatory boot action: "on restart always set
// enphase to disable") and again by requestDischargeToGridDisableNow()
// (the button handler, when the user switches to disabled) -- a
// genuinely different task from this one's own background-task tick, same
// two-caller situation LoadControlState.cpp's own carV2hRecoveryPending
// comment describes for the "Clear Recovery Flag" button -- so every
// access goes through mutex().
static bool disablePending = true;

// True from boot until a status poll actually CONFIRMS Enphase reports
// discharge off -- i.e. the separate, longer-lived half of the mandatory
// boot safety check's own retry coverage. Deliberately NOT the same
// signal as runStatusPoll()'s own wantedOff (autoDischargeEnabled(), now
// also extended to cover the auto-logic's own pending/negative decision --
// see that function's own comment): that latch (DischargeToGridState.h)
// is persisted and, as of 2026-10-13, no longer force-reset at boot -- if
// it happens to be in its ordinary "enabled" state (the common case)
// going into a reboot, checking it (or even the extended wantedOff, which
// is also true by default at boot until the auto-logic's first real
// decision lands) instead of this flag would mean the mandatory boot
// disable got no resend coverage at all if its one fire-and-forget
// attempt failed to land -- exactly the gap this flag closes, as
// independent, redundant coverage of the SAME early window. Also read
// (not written) by checkDischargeToGridLogic() below,
// so its own decision can never race the boot safety check -- still no
// lock needed, since both are this same file's own background-task tick,
// never concurrent with each other. Cleared the first time it's
// confirmed and never set again after that -- a one-time boot safety
// check, not an ongoing obligation the way wantedOff is.
static bool bootDisableUnconfirmed = true;

// The most recently APPLIED auto-discharge decision, and whether there's
// been one yet this "episode" (since boot, or since the button was last
// re-enabled after being disabled -- see checkDischargeToGridLogic()'s
// own disabled branch, further down). Declared up here, ahead of where
// they're mainly used, specifically so runStatusPoll() below can also
// read them -- see its own wantedOff comment for why. Only ever touched
// from this file's own background-task tick -- no lock needed, same
// reasoning as statusPollTimer/bootDisableUnconfirmed.
static bool haveDtgDecision = false;
static bool lastDtgDecision = false;

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
// setDtgPolledStatus()) so checkDischargeToGridPoll() below can pick
// STATUS_RETRY_MS instead of the full STATUS_POLL_INTERVAL_MS on failure.
//
// Also the self-correcting half of the one-shot disable action (see
// disablePending's own comment above): reads the latch BEFORE
// setDtgPolledStatus() below can force it, so a mismatch is judged against
// what the button wanted going into this poll, not what it gets forced to
// as a side effect of this same result.
//
// wantedOff also covers the auto-discharge DECISION's own current intent,
// not just the button latch -- confirmed as a real gap in practice: once
// bootDisableUnconfirmed clears (boot disable confirmed off for good) and
// the latch allows discharge, NOTHING previously kept re-asserting "off"
// if checkDischargeToGridLogic() hadn't positively decided to turn it ON
// (either because it hasn't decided yet, or because it decided OFF) --
// Enphase's own cloud-side schedule re-engaging on its own (independent
// of our last request) would then go completely uncorrected for as long
// as that stayed true, with no "enabling discharge" decision ever logged
// to explain it. Skipped once the schedule window has actually started
// today, matching checkDischargeToGridLogic()'s own "ignore changes
// during and after the discharge schedule starts" rule -- this poll
// shouldn't keep fighting Enphase's own setting once that function has
// stopped caring either.
static bool runStatusPoll() {
  bool enabled;
  int startMinutes, endMinutes;
  float endSoc;
  if (!fetchDischargeToGridStatus(enabled, startMinutes, endMinutes, endSoc)) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridTask: status poll failed -- leaving last-known display state"));
    }
    return false;
  }

  bool wantedOff = !autoDischargeEnabled();
  bool autoLogicWantsOff = false;
  if (!wantedOff) {
    bool windowStarted = !missedSNTPupdate() && dtgScheduleStartMinutes() >= 0
                          && localTimeMinutesSinceMidnight() >= dtgScheduleStartMinutes();
    if (!windowStarted && !(haveDtgDecision && lastDtgDecision)) {
      autoLogicWantsOff = true;
      wantedOff = true;
    }
  }
  setDtgPolledStatus(enabled, startMinutes, endMinutes, endSoc); // may force the latch to enabled -- see that function's own header comment

  if (!enabled) {
    bootDisableUnconfirmed = false; // confirmed off -- boot safety check satisfied, for good (see its own comment)
  } else if (wantedOff || bootDisableUnconfirmed) {
    // Enphase still reports discharge on, and either the button/auto-logic
    // wants it off or the mandatory boot disable is still unconfirmed --
    // either way, the last disable attempt didn't actually take yet (most
    // likely just Enphase's own asynchronous apply still catching up --
    // see DischargeToGridFetch.h -- not necessarily a real failure), or it
    // was switched back on some other way (e.g. Enphase's own schedule
    // re-engaging on its own). Resend once, optimistically assumed to
    // work -- if it still hasn't taken by the NEXT poll, this same check
    // fires again then.
    //
    // Note setDtgPolledStatus() just above never touches the latch itself
    // (removed 2026-10-13, per explicit instruction -- see
    // DischargeToGridState.h's own comment); this resend is purely about
    // getting Enphase's OWN setting to actually match off, regardless of
    // whatever the latch/auto-logic currently want.
    if (debugPtr) {
      debugPtr->print(F("DischargeToGridTask: Enphase still reports discharge on ("));
      if (autoLogicWantsOff) {
        debugPtr->print(F("auto-logic hasn't decided on"));
      } else if (wantedOff) {
        debugPtr->print(F("button wants it off"));
      } else {
        debugPtr->print(F("boot safety check unconfirmed"));
      }
      debugPtr->println(F(") -- resending disable"));
    }
    // Optimistic -- see setDtgPolledEnabledAssumed()'s own comment
    // (DischargeToGridState.h). Overrides the enabled=true this same call
    // just published via setDtgPolledStatus() above, so the button/
    // BatteryProjection reflect THIS resend's assumed outcome, not the
    // stale "still on" reading that triggered it -- confirmed as a real
    // display-lag bug otherwise (streaming_2026-10-08T03-36-12-135Z.txt:
    // button/plot kept showing "will"/the DTG override well after a
    // disable had been sent and resent). Set BEFORE the network call
    // below, not after -- setDischargeToGridEnabled()'s own GET/POST/PUT
    // chain can take several seconds (confirmed in the same streaming
    // log), and there's no reason to leave the display on the stale
    // "still on" reading for that whole round trip when the outcome is
    // being assumed anyway.
    setDtgPolledEnabledAssumed(false);
    requestBatteryProjectionRecompute(); // SOC plot's own DTG override must reflect this immediately too -- see that function's own comment (BatteryProjection.h)
    setDischargeToGridEnabled(false);
  }
  return true;
}

void checkDischargeToGridPoll() {
  dbg();

  bool runDisableNow = false;
  xSemaphoreTake(mutex(), portMAX_DELAY);
  if (disablePending) {
    disablePending = false; // one-shot -- see its own comment
    runDisableNow = true;
  }
  xSemaphoreGive(mutex());
  if (runDisableNow) {
    // Optimistic, set BEFORE the network call below -- see
    // setDtgPolledEnabledAssumed()'s own comment (DischargeToGridState.h):
    // the button/BatteryProjection should reflect this disable
    // immediately rather than sitting on whatever was last actually
    // polled (possibly still "on", e.g. right at boot before any poll
    // has run at all) for the several seconds setDischargeToGridEnabled()'s
    // own GET/POST/PUT chain can take, let alone until the next ~5min
    // status poll happens to run.
    setDtgPolledEnabledAssumed(false);
    requestBatteryProjectionRecompute(); // SOC plot's own DTG override must reflect this immediately too -- see that function's own comment (BatteryProjection.h)
    // Fire and forget -- see disablePending's own comment. ok is only
    // logged, never acted on: runStatusPoll()'s own bootDisableUnconfirmed/
    // wantedOff check (not this return value) is what decides whether a
    // resend is needed later.
    bool ok = setDischargeToGridEnabled(false);
    if (debugPtr) {
      debugPtr->println(ok ? F("DischargeToGridTask: disable request sent and confirmed by the PUT response")
                           : F("DischargeToGridTask: disable request sent but not confirmed -- assuming it worked anyway, next status poll will check"));
    }
  }

  // statusPollTimer is never touched from the button-press task -- no lock needed here.
  if (!statusPollTimer.isRunning()) {
    statusPollTimer.start(0); // due immediately the first time this is ever ticked
  }
  if (statusPollTimer.justFinished()) {
    bool ok = runStatusPoll();
    statusPollTimer.start(ok ? STATUS_POLL_INTERVAL_MS : STATUS_RETRY_MS);
  }
}

void requestDischargeToGridDisableNow() {
  xSemaphoreTake(mutex(), portMAX_DELAY);
  disablePending = true;
  xSemaphoreGive(mutex());
}

// Re-check cadence, per explicit instruction ("re-check each 5mins").
// Only ever reached once every gate below has actually passed at least
// once this attempt -- see the early-exit comment further down for why a
// blocked attempt re-arms at 0 instead, matching BatteryProjection.cpp's
// own identical fix for the same "first real check needlessly delayed by
// up to 5 more minutes" hazard.
static const unsigned long LOGIC_INTERVAL_MS = 5UL * 60UL * 1000UL;

// logicTimer is only ever touched from this file's own background-task
// tick -- no lock needed, same reasoning as statusPollTimer above.
// haveDtgDecision/lastDtgDecision (same no-lock reasoning) are declared
// further up, alongside bootDisableUnconfirmed -- see that declaration's
// own comment for why. Compared against each newly computed decision so
// setDischargeToGridEnabled() is only ever called on an actual CHANGE,
// per explicit instruction -- never just because 5 minutes passed and
// the same answer came out again.
static pfodDelay logicTimer;

void checkDischargeToGridLogic() {
  dbg();

  if (!autoDischargeEnabled()) {
    // Per explicit instruction: button disabled -- don't use this logic
    // at all. Reset so a later re-enable starts completely fresh -- the
    // next decision, even if it happens to match whatever was last
    // applied before being disabled, is treated as new and applied, per
    // "if the button is later changed to enable discharge then start
    // using logic to set enphase setting again".
    haveDtgDecision = false;
    logicTimer.stop(); // isRunning()-gated re-arm below fires fresh once re-enabled
    return;
  }

  // isRunning() checked and acted on BEFORE justFinished() -- the safe
  // lazy-arm order; see EnlightenLoginFetch.cpp's own top-of-file comment
  // on the hazard of doing it the other way around.
  if (!logicTimer.isRunning()) {
    logicTimer.start(0); // due immediately the first time this is ticked, or just after being re-enabled
  }
  if (!logicTimer.justFinished()) {
    return;
  }

  // From here on, every early exit below re-arms at 0 (retry next tick),
  // NOT LOGIC_INTERVAL_MS -- committing to the full 5min wait only
  // happens once a real decision (or the "window already started" no-op)
  // has actually been reached. Same fix, same reasoning, as
  // BatteryProjection.cpp's own "Only now, having actually committed..."
  // comment: otherwise a blocked attempt during startup (clock not synced
  // yet, schedule not polled yet, forecasts still warming up) would
  // consume the throttle window and delay the FIRST real check by up to
  // 5 more minutes once the data actually became ready.
  if (missedSNTPupdate()) {
    logicTimer.start(0); // need a real "now" -- not a real attempt yet
    return;
  }
  int nowMinutes = localTimeMinutesSinceMidnight();

  int scheduleStartMinutes = dtgScheduleStartMinutes();
  if (scheduleStartMinutes < 0) {
    logicTimer.start(0); // DTG schedule not polled yet -- nothing to compare "now" against
    return;
  }
  if (nowMinutes >= scheduleStartMinutes) {
    // The window has started (or already passed) for today -- ignore
    // changes, per explicit instruction. Nothing left to do until
    // tomorrow, so the real cadence is fine here, not a busy retry.
    logicTimer.start(LOGIC_INTERVAL_MS);
    return;
  }

  if (bootDisableUnconfirmed) {
    logicTimer.start(0); // don't race the mandatory boot-off check -- see its own comment
    return;
  }

  // Hot Day gate -- added 2026-10-13, per explicit instruction: "if
  // forecast max temp tomorrow is < setting then skip this branch and
  // fall back to testing for discharge to grid" / "if ... >= setting
  // then disable discharge to grid (if it is enabled)". Tested fresh on
  // EVERY tick (not gated to HotDayTask.cpp's own once-a-day 2:02pm
  // decision -- that function owns WHEN to flip hotDayActive()/switch
  // HW-Car%/charge-from-grid, this just has to stay out of its way and
  // keep discharge off for as long as a hot day is forecast). A "not
  // ready yet" read (forecast hasn't arrived) retries at 0 rather than
  // falling through, same as every other not-ready gate above -- never
  // guesses hot/not-hot from unready data.
  bool isHotForecast;
  if (!readHotDayForecast(isHotForecast)) {
    logicTimer.start(0);
    return;
  }
  if (isHotForecast) {
    // Same "reset so a later resumption starts completely fresh" shape as
    // the disabled-latch branch above -- runStatusPoll()'s own wantedOff
    // check (haveDtgDecision cleared, so `haveDtgDecision && lastDtgDecision`
    // is false) already keeps discharge forced off for as long as this
    // stays true, with no separate one-shot "disable now" call needed
    // here. Re-armed at the real cadence, not 0 -- this IS a real,
    // stable decision (to stay off), just re-tested regularly like
    // every other real decision in this function.
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridTask: hot day forecast for tomorrow -- skipping the normal discharge decision, keeping discharge off"));
    }
    haveDtgDecision = false;
    logicTimer.start(LOGIC_INTERVAL_MS);
    return;
  }

  // Cold Day gate -- added 2026-10-14, per explicit instruction: prevent
  // discharge to grid if tomorrow is forecast to be a cold day too (max
  // temp <= COLD_DAY_MAX_TEMP_C), same "stay out of the way, keep
  // discharge off" shape as the Hot Day gate just above -- tested fresh
  // on every tick, never guesses cold/not-cold from a forecast that
  // hasn't arrived yet. Reads tomorrowWeatherForecast directly (same
  // fields/lock HotDayState.cpp's own readHotDayForecast() already reads
  // for its own hot-day comparison above) rather than extending that
  // function, since this is a separate, DischargeToGridTask-only
  // threshold, not part of the Hot Day feature itself.
  lockSolarForecast();
  bool tomorrowForecastReady = tomorrowWeatherForecast.received;
  float tomorrowMaxTempC = tomorrowWeatherForecast.tempMaxC;
  unlockSolarForecast();
  if (!tomorrowForecastReady) {
    logicTimer.start(0); // forecast not ready yet -- never guess
    return;
  }
  if (tomorrowMaxTempC <= COLD_DAY_MAX_TEMP_C) {
    // Same "reset so a later resumption starts completely fresh" shape as
    // the Hot Day gate above -- runStatusPoll()'s own wantedOff check
    // (haveDtgDecision cleared, so `haveDtgDecision && lastDtgDecision` is
    // false) already keeps discharge forced off for as long as this stays
    // true.
    if (debugPtr) {
      debugPtr->print(F("DischargeToGridTask: cold day forecast for tomorrow ("));
      debugPtr->print(tomorrowMaxTempC, 1);
      debugPtr->println(F("C) -- skipping the normal discharge decision, keeping discharge off"));
    }
    haveDtgDecision = false;
    logicTimer.start(LOGIC_INTERVAL_MS);
    return;
  }

  // Read directly -- same two totals (tomorrowSolarData/
  // tomorrowAvgConsumptionData, PlotDataInstances.h) Dwg_StatusApp.cpp's
  // own status line and BatteryProjection.cpp already read straight off.
  // Unlike those two (both main-thread-only callers), this read is
  // genuinely cross-thread, so it holds lockTomorrowForecastData()
  // (PlotDataInstances.h) across BOTH readiness checks AND both
  // totalKwh() calls as one atomic snapshot -- see that lock's own header
  // comment for why it must cover all four together, not just the data.
  lockTomorrowForecastData();
  bool ready = avgConsumptionForecastReady() && solarForecastReady();
  float tomorrowSolarKwh = ready ? tomorrowSolarData.totalKwh() : 0.0;
  float avgConsumptionKwh = ready ? tomorrowAvgConsumptionData.totalKwh() : 0.0;
  unlockTomorrowForecastData();
  if (!ready) {
    logicTimer.start(0); // can't decide yet -- still dummy-fill/placeholder data
    return;
  }

  // Today's car-charging energy so far (CarChargerInstances.h), under
  // CarChargerTask.cpp's own lock -- see MIN_CAR_CHARGE_TODAY_KWH's own
  // comment, further up.
  lockCarChargerState();
  float carChargeTodayKwh = carChargingKwhToday;
  unlockCarChargerState();

  logicTimer.start(LOGIC_INTERVAL_MS); // a real decision follows -- real cadence from here

  float tomorrowExcessKwh = tomorrowSolarKwh - avgConsumptionKwh;

  // Per explicit instruction (2026-10-14): discharge to grid now also
  // requires tomorrow's excess to be at least MIN_TOMORROW_EXCESS_KWH
  // (replacing the previous plain ">0" threshold) AND today's car
  // charging to be at least MIN_CAR_CHARGE_TODAY_KWH -- both on top of
  // the Hot Day/Cold Day gates above, which already keep this false
  // whenever either of those apply.
  bool wantDischarge = (tomorrowExcessKwh >= MIN_TOMORROW_EXCESS_KWH)
                        && (carChargeTodayKwh >= MIN_CAR_CHARGE_TODAY_KWH);

  if (haveDtgDecision && wantDischarge == lastDtgDecision) {
    return; // no change -- per explicit instruction, only update the Enphase setting when it does
  }

  if (debugPtr) {
    debugPtr->print(F("DischargeToGridTask: auto-discharge logic tomorrow solar="));
    debugPtr->print(tomorrowSolarKwh, 2);
    debugPtr->print(F("kWh avg consumption="));
    debugPtr->print(avgConsumptionKwh, 2);
    debugPtr->print(F("kWh excess="));
    debugPtr->print(tomorrowExcessKwh, 2);
    debugPtr->print(F("kWh car charge today="));
    debugPtr->print(carChargeTodayKwh, 2);
    debugPtr->print(F("kWh -> "));
    debugPtr->println(wantDischarge ? F("enabling discharge") : F("disabling discharge"));
  }

  // Assumed to have worked -- updates the button text immediately,
  // BEFORE the network call below, instead of leaving it stale for
  // however long setDischargeToGridEnabled()'s own GET/POST/PUT chain
  // takes (confirmed several seconds in practice), let alone until the
  // next ~5min status poll happens to run (confirmed as a real,
  // several-minute delay otherwise --
  // streaming_2026-10-08T03-13-54-188Z.txt/streaming_2026-10-08T03-17-52-546Z.txt:
  // decided+PUT succeeded at 14:09:44-50, display didn't update until the
  // poll at 14:14:08). If this guess is wrong, that next real poll's own
  // setDtgPolledStatus() call overwrites it with the truth regardless --
  // see setDtgPolledEnabledAssumed()'s own comment (DischargeToGridState.h).
  setDtgPolledEnabledAssumed(wantDischarge);
  // The SOC plot's own DTG override (BatteryProjection.cpp) reads the
  // same dtgPolledEnabled() just set above, but only every ~1min
  // (PROJECTION_INTERVAL_MS) on its own throttle -- without this, the
  // plot specifically would still lag behind the button text by up to
  // that long. See requestBatteryProjectionRecompute()'s own comment
  // (BatteryProjection.h).
  requestBatteryProjectionRecompute();
  lastDtgDecision = wantDischarge;
  haveDtgDecision = true;

  // Fire and forget, same as the boot/button disable action above -- not
  // retried if it fails; the next 5min re-check only tries again if the
  // computed decision actually changes again, per explicit instruction
  // ("only if it changes"), not on a timer.
  setDischargeToGridEnabled(wantDischarge);
}

// See DischargeToGridTask.h's own comment on these three -- plain
// read-only access to this file's own fixed thresholds, added 2026-10-14
// for pfodMainMenuApp.cpp's status label.
float coldDayMaxTempC() {
  return COLD_DAY_MAX_TEMP_C;
}

float minTomorrowExcessKwh() {
  return MIN_TOMORROW_EXCESS_KWH;
}

float minCarChargeTodayKwh() {
  return MIN_CAR_CHARGE_TODAY_KWH;
}
// ================= end of DischargeToGridTask.cpp  file
