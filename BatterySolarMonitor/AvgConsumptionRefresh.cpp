// AvgConsumptionRefresh.cpp  file  =================
#include "AvgConsumptionRefresh.h"
#include "AvgConsumptionTask.h"
#include "AvgConsumptionInstances.h"
#include "EnlightenHistoryFetch.h" // resolveEnlightenHosts()
#include "PlotDataInstances.h"
#include "LocalTimeMinutes.h"
#include "ntpSupport.h"
#include <Arduino.h>
#include <pfodDebugPtr.h>
#include <pfodDelay.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// Give EnlightenHistoryRefresh.cpp's own first "today" fetch a head start
// (its MIN_UPTIME_BEFORE_HISTORY_MS floor is 20000) -- both ultimately
// share the same resolveEnlightenHosts() candidate-IP state, so there's
// no strict ordering requirement, just no reason to race it either.
static const unsigned long MIN_UPTIME_BEFORE_AVG_MS = 25000;

// Matches Dwg_ConsumptionPlot1App.cpp's own dummy-fill convention.
static const uint8_t PREDICTED_COLOR = 8; // GREY

static bool ntpAndUptimeReady = false;
static bool requested = false;

// Retry backoff for a TOTAL batch failure (0 of 5 days succeeded) -- see
// this file's header comment and the "TOTAL-FAILURE RETRY" block below in
// checkAvgConsumptionRefresh(). Same retry-until-success interval as
// EnlightenLoginFetch.h's own RETRY_INTERVAL_MS.
static const unsigned long RETRY_INTERVAL_MS = 5UL * 60UL * 1000UL; // 5 minutes
static pfodDelay retryTimer;

// How many days' worth were reflected in the last paint -- 0 means never
// painted. Compared against avgConsumptionProfile.successCount every
// call so a repaint happens as soon as (and every time) that count
// advances, not just once AvgConsumptionTask.cpp's whole batch finishes.
// Written only under lockTomorrowForecastData() (PlotDataInstances.h),
// together with tomorrowAvgConsumptionData itself -- see that lock's own
// header comment for why.
static int lastPaintedSuccessCount = 0;

bool avgConsumptionForecastReady() {
  lockTomorrowForecastData();
  bool result = lastPaintedSuccessCount > 0;
  unlockTomorrowForecastData();
  return result;
}

// Paints 'snapshot's current average over every slot strictly after
// "now" -- matches BatteryProjection.cpp's own firstProjectedMinute
// reasoning exactly (PlotData::nearestSlotMinutes() is the slot
// GatewayRefresh.cpp's live point/EnlightenHistoryRefresh.cpp's merge may
// have JUST written "now"'s own actual reading into; painting only
// starts one step after it so this can never overwrite that).
//
// Also repaints tomorrowAvgConsumptionData across its WHOLE day -- unlike
// today's consumptionData, tomorrow hasn't happened yet at all, so there's
// no "now" boundary/actual data to avoid overwriting (same reasoning as
// SolarForecastRefresh.cpp's unconditional full-day repaint of
// tomorrowSolarData). Previously this file only ever touched
// consumptionData, so tomorrowAvgConsumptionData sat forever on
// Dwg_TomorrowConsumptionPlot1App.cpp's boot-time placeholder, never
// updated with a real average.
static void paintForecast(const AvgConsumptionProfile &snapshot) {
  int nowMinutes = localTimeMinutesSinceMidnight();
  int firstFutureMinute = PlotData::nearestSlotMinutes(nowMinutes) + PlotData::STEP_MINUTES;
  for (int t = firstFutureMinute; t <= PlotData::END_MINUTES; t += PlotData::STEP_MINUTES) {
    int index = (t - PlotData::START_MINUTES) / PlotData::STEP_MINUTES;
    consumptionData.setPointAtTime(t, snapshot.avgKw[index], PREDICTED_COLOR);
  }
  // tomorrowAvgConsumptionData + lastPaintedSuccessCount together, under
  // lockTomorrowForecastData() (PlotDataInstances.h) -- same reasoning as
  // SolarForecastRefresh.cpp's identical bracketing of tomorrowSolarData/
  // everPainted: DischargeToGridTask.cpp's own background-task read of
  // both must never see the count already advanced while this paint is
  // still in progress. consumptionData (today) is main-thread-only, left
  // outside the lock.
  lockTomorrowForecastData();
  for (int t = PlotData::START_MINUTES; t <= PlotData::END_MINUTES; t += PlotData::STEP_MINUTES) {
    int index = (t - PlotData::START_MINUTES) / PlotData::STEP_MINUTES;
    tomorrowAvgConsumptionData.setPointAtTime(t, snapshot.avgKw[index], PREDICTED_COLOR);
  }
  lastPaintedSuccessCount = snapshot.successCount;
  unlockTomorrowForecastData();

  if (debugPtr) {
    debugPtr->print(F("AvgConsumption: painted forecast onto consumptionData from "));
    debugPtr->print(firstFutureMinute / 60);
    debugPtr->print(':');
    if ((firstFutureMinute % 60) < 10) {
      debugPtr->print('0');
    }
    debugPtr->print(firstFutureMinute % 60);
    debugPtr->print(F(" onward and tomorrowAvgConsumptionData in full, averaged over "));
    debugPtr->print(snapshot.successCount);
    debugPtr->println(F(" day(s)"));
  }
}

void checkAvgConsumptionRefresh() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  if (!requested) {
    if (!ntpAndUptimeReady) {
      // Need real calendar dates for the daysAgo= lookups below.
      if (missedSNTPupdate()) {
        return;
      }
      if (millis() < MIN_UPTIME_BEFORE_AVG_MS) {
        return;
      }
      ntpAndUptimeReady = true;
    }
    // resolveEnlightenHosts() does DNS -- main thread only, see
    // EnlightenHistoryFetch.h. Safe to call again even if
    // EnlightenHistoryRefresh.cpp already has -- idempotent, just
    // refreshes the same shared candidate-IP list.
    resolveEnlightenHosts();
    requestAvgConsumptionFetch();
    requested = true;
    retryTimer.start(RETRY_INTERVAL_MS); // armed in case this whole batch fails -- see below
    return;
  }

  // From here on (whether AvgConsumptionTask.cpp's batch is still running
  // or has already finished), react to ANY progress: repaint whenever
  // successCount has advanced since the last paint. Once the batch
  // finishes, successCount stops changing and this naturally becomes a
  // cheap no-op forever after -- no separate "done" bookkeeping needed.
  lockAvgConsumptionProfile();
  int successCount = avgConsumptionProfile.successCount;
  AvgConsumptionProfile snapshot = avgConsumptionProfile; // small (69 floats + 69 ints) -- cheap to copy under the lock
  unlockAvgConsumptionProfile();

  // ntpAndUptimeReady only latches that NTP synced ONCE, at boot -- it
  // never un-latches, but missedSNTPupdate() itself can go true again
  // later (ntpSupport.cpp: no update for 70 min re-arms it), e.g. a long
  // WiFi outage mid-uptime. Re-check here too, right before
  // paintForecast() reads localTimeMinutesSinceMidnight(), rather than
  // trusting the one-time latch for the rest of this device's uptime.
  // Simply skipping (not painting) is enough -- successCount stays ahead
  // of lastPaintedSuccessCount, so this retries automatically on every
  // later call until the clock is trustworthy again.
  if (successCount > lastPaintedSuccessCount && !missedSNTPupdate()) {
    paintForecast(snapshot);
  }

  // TOTAL-FAILURE RETRY (added 2026-09-11): AvgConsumptionTask.cpp's whole
  // 5-day batch runs exactly once per requestAvgConsumptionFetch() call.
  // Confirmed in deviceLog.txt: on a device booting without an existing
  // /auth_token.csv, this file's own readiness gate above (NTP synced +
  // MIN_UPTIME_BEFORE_AVG_MS uptime) can fire and request the batch BEFORE
  // EnlightenLoginFetch.h's boot-time self-refresh has finished building a
  // session cookie -- every one of the 5 date fetches then hits an empty
  // enlightenSession(), which the Enlighten API answers with a 302
  // redirect, not a 401, so EnlightenHistoryFetch.cpp's own
  // notifyAuthTokenRejected() feedback (which only fires on a confirmed
  // 401) never triggers either. successCount stays 0, and without this
  // retry, avgConsumptionForecastReady() (lastPaintedSuccessCount>0) would
  // never become true for the rest of the boot -- permanently blocking
  // BatteryProjection.cpp's SOC forecast line, the Status screen's Usage
  // line, and the solar-vs-consumption comparison, not just for a few
  // minutes but until the next daily reboot.
  //
  // A PARTIAL success (successCount >= 1) is deliberately left alone and
  // never retried -- that's already a usable average (see
  // AvgConsumptionTask.cpp's own "3-of-5 still usable" design), so
  // retrying would only throw away already-good data for no benefit.
  //
  // retryTimer.justFinished() is checked unconditionally here, on its own
  // -- never folded into a compound && condition (pfodParserAI-guide.md
  // sec 4, "the one rule that matters"). isRunning() dropped entirely:
  // retryTimer is always running by the time this line can be reached
  // (armed unconditionally right after the very first requestAvgConsumptionFetch()
  // above, before 'requested' ever latches true), so it was redundant --
  // and combining it with justFinished() in one condition is itself the
  // wrong shape regardless.
  bool retryDue = retryTimer.justFinished();
  if (successCount == 0 && retryDue) {
    if (!avgConsumptionAttemptRunning()) {
      if (debugPtr) {
        debugPtr->println(F("AvgConsumption: still 0 days after retry interval -- requesting batch again"));
      }
      resolveEnlightenHosts();
      requestAvgConsumptionFetch();
    }
    retryTimer.start(RETRY_INTERVAL_MS); // keep retrying every 5 min until >=1 day succeeds
  }
}
// ================= end of AvgConsumptionRefresh.cpp  file
