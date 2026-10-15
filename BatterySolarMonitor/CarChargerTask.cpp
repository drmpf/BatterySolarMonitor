// CarChargerTask.cpp  file  =================
#include "CarChargerTask.h"
#include "CarChargerFetch.h"
#include "CarChargerInstances.h"
#include "CarChargeHistory.h" // loadOrResumeCarChargeToday()/saveCarChargeToday()/rolloverCarChargeDay()
#include "LocalTimeMinutes.h"
#include "ntpSupport.h" // missedSNTPupdate()
#include "LoadControlTask.h" // requestLoadControlPollNow()
#include <Arduino.h>
#include <pfodDelay.h>

// Stage 1 test cadence -- matches the eventual real control cadence
// (dailyPrediction/plot_today_solar.py's control_loads() polls every
// 5 min) so this doesn't need re-tuning once the actual start/stop logic
// lands in a later stage.
static const unsigned long POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;

// How soon requestCarChargerRecheck() re-arms the poll for, after a
// toggle -- long enough for the relay to settle (up to ~8s), short enough
// that the Status screen's optimistic post-toggle guess doesn't stand
// uncorrected for long.
static const unsigned long RECHECK_DELAY_MS = 20UL * 1000UL;

// Turning V2H ON takes roughly 2x the charger relay's own ~8s settling
// time plus a few more seconds before AC actually starts (~16-20s+) --
// longer than RECHECK_DELAY_MS above is tuned for, so
// requestCarChargerV2hRecheck() uses this longer delay instead.
static const unsigned long V2H_RECHECK_DELAY_MS = 30UL * 1000UL;

// Same fixed-load assumption as dailyPrediction/plot_today_solar.py's own
// CAR_CHARGE_W -- the charger's /api/state has no real energy/power
// reading at all, so both sides estimate kWh the same way: this many
// watts, for as long as the charger was seen actively drawing power, one
// poll interval at a time.
static const float CAR_CHARGE_W = 3700.0;

// pfodDelay (pfodParser/src/pfodDelay.h), not a plain millis()/nextDueMs
// comparison -- see GatewayPollTask.cpp's identical comment on its own
// pollTimer for why, and for the isRunning()/justFinished() usage idiom
// (https://www.forward.com.au/pfod/ArduinoProgramming/TimingDelaysInArduino.html)
// followed here.
static pfodDelay pollTimer;

// pollTimer is one shared timer serving 4 different reasons to wake up
// early, each representing a MINIMUM settle time before a fetch would see
// anything meaningful -- naively overwriting it on every request (plain
// "last caller wins") let a later, SHORTER request pre-empt an earlier,
// LONGER one still in flight (e.g. a V2H_SCHEDULE_FOLLOWUP_MS (60s) wait,
// armed to give the charger's own schedule flip time to land, cut short to
// RECHECK_DELAY_MS (20s) by an unrelated plain charger toggle happening in
// the meantime) -- confirmed as a real, if rare, possibility per explicit
// report. Fix: rank the reasons by how long they actually need, and only
// let a request take effect if its priority is >= whatever is currently
// armed. An equal-priority re-request (e.g. a second toggle of the same
// kind) still restarts the wait fresh from now -- that's a legitimate
// debounce, not a pre-emption, since the second toggle genuinely needs its
// own full settle time measured from when IT happened.
enum RecheckPriority {
  RECHECK_NONE = 0,               // POLL_INTERVAL_MS -- nothing special pending, always overridable
  RECHECK_CHARGER = 1,            // RECHECK_DELAY_MS (20s) -- charger relay settling
  RECHECK_V2H = 2,                // V2H_RECHECK_DELAY_MS (30s) -- V2H settling, slower than the relay
  RECHECK_SCHEDULE_FOLLOWUP = 3,  // V2H_SCHEDULE_FOLLOWUP_MS (60s) -- margin past the schedule's own boundary
};
static RecheckPriority currentRecheckPriority = RECHECK_NONE;

// Gated re-arm for the two public request*() functions below -- rejects a
// request whose priority is lower than whatever's already counting down.
// NOT used for the authoritative end-of-poll decision in
// checkCarChargerPoll() itself -- that one sets pollTimer/
// currentRecheckPriority directly and unconditionally, since by that point
// the poll that any pending reason armed has actually just run, so
// whatever priority was in force before is now resolved and a fresh
// decision (needFollowup or not) is what should apply next, gate or no
// gate.
static void armPollTimerIfHigherPriority(unsigned long delayMs, RecheckPriority priority) {
  if (priority < currentRecheckPriority) {
    return;
  }
  pollTimer.start(delayMs);
  currentRecheckPriority = priority;
}

// Created on first use, not at global-construction time -- see
// SolarForecastTask.cpp's identical comment for why.
static SemaphoreHandle_t carChargerMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (carChargerMutex == NULL) {
    carChargerMutex = xSemaphoreCreateMutex();
  }
  return carChargerMutex;
}

void lockCarChargerState() {
  xSemaphoreTake(mutex(), portMAX_DELAY);
}

void unlockCarChargerState() {
  xSemaphoreGive(mutex());
}

// Empty until the first poll WITH A SYNCED CLOCK establishes it (see
// checkCarChargerPoll()'s own !missedSNTPupdate() guard around all of
// this, added 2026-09-28 alongside CarChargeHistory.h) -- a full
// "YYYY-MM-DD" string (localTodayDateString()), not just day-of-year, so
// it can't be confused across a year boundary either. Before NTP has ever
// synced, this and CarChargeHistory.h's own persisted-file check are
// skipped entirely now -- unlike before 2026-09-28, a spurious "day
// changed" read against the boot-default epoch date is no longer
// harmless once there's a real LittleFS file it could wrongly bank as a
// finished day, so it must never run against an unsynced clock at all.
static String lastCarChargeDate;

// The ACTUAL time between successive accumulation updates -- NOT assumed
// to be POLL_INTERVAL_MS, because requestCarChargerRecheck() can make this
// function run as little as RECHECK_DELAY_MS (20s) after the previous
// call. Crediting a full POLL_INTERVAL_MS-worth of energy for a 20s
// interval would over-estimate by ~15x whenever a recheck happens to
// catch the charger mid-charge. false until the first poll has actually
// run, so that one poll (no real "since last poll" duration to measure)
// falls back to the nominal POLL_INTERVAL_MS rather than a meaningless 0
// or a huge millis()-since-boot value.
static bool everPolled = false;
static unsigned long lastPollMs = 0;

// How soon after the CHARGER's OWN v2h_auto on/off boundary
// (state.v2hOnTimeMinutes/v2hOffTimeMinutes -- switched entirely by that
// other ESP32's own EV_Charger_V2H_switch/switchSettings.cpp handleAuto(),
// independent of anything this sketch does) to force a follow-up fetch --
// added per explicit report: a poll landing seconds before the boundary
// (e.g. 14:01:03 for an on=14:01 schedule) still reads the pre-flip state,
// and the NEXT poll was otherwise a full POLL_INTERVAL_MS (5min) away,
// leaving the status log/display showing stale v2h=off for up to 5min
// after it actually switched on.
static const unsigned long V2H_SCHEDULE_FOLLOWUP_MS = 60UL * 1000UL;

// Local minutes-since-midnight as of the last poll that had a valid
// synced clock -- -1 until that first happens. Used only to detect "an
// on/off boundary was crossed SINCE the last poll" (see crossedBoundary()
// below); deliberately NOT updated on a poll with no synced clock or a
// failed fetch, so the next good poll's gap still correctly spans
// whatever boundary may have been crossed during that gap.
static int lastV2hScheduleCheckMin = -1;

// True if boundaryMin lies in (fromMinExclusive, toMinInclusive], wrapping
// at midnight -- i.e. "was boundaryMin crossed going from fromMin to
// toMin". False when fromMin==toMin (no time has actually passed since the
// last check, e.g. back-to-back rechecks in the same clock-minute).
static bool crossedBoundary(int fromMinExclusive, int toMinInclusive, int boundaryMin) {
  if (fromMinExclusive == toMinInclusive) {
    return false;
  }
  int gap = (toMinInclusive - fromMinExclusive + 1440) % 1440;
  int boundaryGap = (boundaryMin - fromMinExclusive + 1440) % 1440;
  return boundaryGap > 0 && boundaryGap <= gap;
}

void checkCarChargerPoll() {
  if (!pollTimer.isRunning()) {
    pollTimer.start(0); // due immediately the first time InternetPollTask.cpp ticks this
  }
  if (!pollTimer.justFinished()) {
    return;
  }
  unsigned long nowMs = millis();
  unsigned long elapsedMs = everPolled ? (nowMs - lastPollMs) : POLL_INTERVAL_MS;
  lastPollMs = nowMs;
  everPolled = true;

  CarChargerState state;
  bool ok = fetchCarChargerState(state); // logs its own result either way -- see CarChargerFetch.cpp

  lockCarChargerState();
  // Guarded on !missedSNTPupdate() (added 2026-09-28, see
  // lastCarChargeDate's own comment) -- localTodayDateString() before the
  // first real NTP sync reads the boot-default epoch date, which must
  // never be persisted or compared against CarChargeHistory.h's files.
  if (!missedSNTPupdate()) {
    char todayBuf[11];
    localTodayDateString(todayBuf, sizeof(todayBuf));
    String todayDateStr(todayBuf);
    if (lastCarChargeDate.length() == 0) {
      // First poll with a synced clock this boot -- resolves whether an
      // unplanned reboot happened earlier today (resume) or crossed a
      // midnight while the device was off (bank the stale day, start
      // fresh) -- see loadOrResumeCarChargeToday()'s own doc comment.
      // KNOWN, ACCEPTED GAP: loadOrResumeCarChargeToday() unconditionally
      // OVERWRITES carChargingKwhToday, so the rare case of the charger
      // already reporting charging=true on one of the few polls before
      // the clock's very first sync (accumulating a few seconds/minutes
      // in RAM with no file to check against yet) is discarded here, not
      // merged in. Not worth the extra complexity to fix: NTP normally
      // syncs within seconds of WiFi connecting, well inside one 5-min
      // poll interval, so the window this could ever matter in is tiny.
      loadOrResumeCarChargeToday(todayDateStr, &carChargingKwhToday);
      lastCarChargeDate = todayDateStr;
    } else if (todayDateStr != lastCarChargeDate) {
      // Genuine local-midnight rollover while running.
      rolloverCarChargeDay(todayDateStr, lastCarChargeDate, carChargingKwhToday);
      carChargingKwhToday = 0.0;
      lastCarChargeDate = todayDateStr;
    }
  }
  if (ok) {
    latestCarChargerState = state;
    carChargerReachable = true;
    // "Actually drawing power" -- charger reports on, 240VAC confirms a
    // car is really plugged in and drawing (not just left switched on
    // with nothing connected), and V2H/V2L isn't blocking it. Same
    // definition dailyPrediction/plot_today_solar.py's own
    // _append_charger_history() uses for its "charging" column, which
    // car_charging_kwh_today() then counts. Charged for the ACTUAL time
    // since the last poll (elapsedMs, not a fixed assumption -- see
    // everPolled/lastPollMs above) at the assumed CAR_CHARGE_W rate -- an
    // estimate, not a measurement, exactly like the Python original.
    bool charging = state.chargerOn && state.ac240vActive && !state.v2hActive;
    if (charging) {
      carChargingKwhToday += CAR_CHARGE_W * (elapsedMs / 3600000.0) / 1000.0;
      // Persist every time it actually changes (added 2026-09-28) -- at
      // most once per ~5min poll, so an unplanned reboot loses at most a
      // few minutes' worth instead of the whole day. Only meaningful once
      // lastCarChargeDate is established (the block above); skipped on
      // the vanishingly rare poll where the clock is still unsynced AND
      // the charger happens to already be reporting charging=true.
      if (lastCarChargeDate.length() > 0) {
        saveCarChargeToday(lastCarChargeDate, carChargingKwhToday);
      }
    }
  } else {
    carChargerReachable = false; // latestCarChargerState left at its last-good value -- see CarChargerInstances.h
  }
  unlockCarChargerState();

  // Sync point (added 2026-09-19, per explicit report/instruction):
  // CarChargerTask.cpp's own poll cadence was never phase-aligned with
  // LoadControlTask.cpp's independent ~5min pollTimer -- confirmed via
  // device log (streaming_2026-09-19T04-05-33-646Z.txt) to let a car-start
  // decision run against up to a full poll-interval-stale car.v2hActive
  // reading, right around the charger's own V2H schedule boundary. Rather
  // than special-casing just the V2H-recovery-recheck and schedule-
  // boundary-follow-up polls (the previous, narrower fix), EVERY
  // successful fetch now unconditionally triggers LoadControlTask.cpp's
  // full decision cycle against this fresh data -- see
  // requestLoadControlPollNow()'s own comment for why re-running that
  // logic more often than its own nominal ~5min cadence is safe (every
  // trigger here corresponds to genuinely fresh, distinct data, and the
  // decision functions themselves are idempotent against repeated fresh
  // evaluation). A failed fetch triggers nothing -- LoadControlTask.cpp's
  // own V2H_RECOVERY_RECHECK_MS/EARLY_RECHECK_MS reschedules remain as the
  // fallback for a persistently-unreachable charger.
  if (ok) {
    requestLoadControlPollNow();
  }

  // Detect "a v2h_auto on/off boundary was crossed since the last poll"
  // from THIS poll's freshly-fetched schedule -- see
  // V2H_SCHEDULE_FOLLOWUP_MS's own comment above for why. Needs a synced
  // clock (missedSNTPupdate()) to trust nowMin at all; only updates
  // lastV2hScheduleCheckMin on a good clock + successful fetch, so a
  // failed poll or an unsynced clock just leaves the next good poll to
  // span the full gap correctly rather than resetting it to "now".
  bool needFollowup = false;
  if (ok && state.v2hAutoActive && !missedSNTPupdate()) {
    int nowMin = localTimeMinutesSinceMidnight();
    if (lastV2hScheduleCheckMin >= 0) {
      needFollowup = crossedBoundary(lastV2hScheduleCheckMin, nowMin, state.v2hOnTimeMinutes)
          || crossedBoundary(lastV2hScheduleCheckMin, nowMin, state.v2hOffTimeMinutes);
    }
    lastV2hScheduleCheckMin = nowMin;
  }

  // Authoritative end-of-poll decision -- unconditional, NOT through
  // armPollTimerIfHigherPriority() (see that function's own comment): this
  // poll just ran, so whatever priority reason brought it here has been
  // serviced, and what runs next is decided fresh from THIS poll's own
  // findings.
  if (needFollowup) {
    pollTimer.start(V2H_SCHEDULE_FOLLOWUP_MS);
    currentRecheckPriority = RECHECK_SCHEDULE_FOLLOWUP;
  } else {
    pollTimer.start(POLL_INTERVAL_MS);
    currentRecheckPriority = RECHECK_NONE;
  }
}

void requestCarChargerRecheck() {
  armPollTimerIfHigherPriority(RECHECK_DELAY_MS, RECHECK_CHARGER);
}

void requestCarChargerV2hRecheck() {
  armPollTimerIfHigherPriority(V2H_RECHECK_DELAY_MS, RECHECK_V2H);
}
// ================= end of CarChargerTask.cpp  file
