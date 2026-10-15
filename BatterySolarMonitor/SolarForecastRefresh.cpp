// SolarForecastRefresh.cpp  file  =================
#include "SolarForecastRefresh.h"
#include "SolarForecastTask.h"
#include "SolarForecastInstances.h"
#include "SolarForecastMapping.h"
#include "PlotDataInstances.h"
#include "LocalTimeMinutes.h"
#include "ntpSupport.h" // missedSNTPupdate()
#include <Arduino.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// BLACK -- this is Open-Meteo forecast data, not actual/measured
// production. Matches the predicted-colour convention used everywhere
// else in this project (e.g. all of tomorrowSolarData's dummy fill was
// black; tomorrow is always entirely a forecast, there being no "actual"
// for a day that hasn't happened).
static const uint8_t PREDICTED_COLOR = 0; // BLACK

// Sticky -- see solarForecastReady()'s own comment (SolarForecastRefresh.h)
// for why this can't just be solarForecastUpdated() itself. Written only
// under lockTomorrowForecastData() (PlotDataInstances.h), together with
// tomorrowSolarData itself -- see that lock's own header comment for why.
static bool everPainted = false;

bool solarForecastReady() {
  lockTomorrowForecastData();
  bool result = everPainted;
  unlockTomorrowForecastData();
  return result;
}

void checkSolarForecastRefresh() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  if (!solarForecastUpdated()) {
    return;
  }
  if (missedSNTPupdate()) {
    return; // no real "now" yet -- can't trust skipBeforeMinutes below
  }

  float todayNoonKw, tomorrowNoonKw, todayNoonCloudPct, tomorrowNoonCloudPct;
  lockSolarForecast();
  // solarData: skip already-elapsed points -- GatewayRefresh.cpp may have
  // written real/measured (GREEN) values there, which this forecast
  // refresh must not stomp back to a black prediction. tomorrowSolarData
  // has no "actual" yet (tomorrow hasn't happened), so it's always
  // refreshed in full.
  //
  // The skip boundary must be "now"'s own rounded slot (via
  // PlotData::nearestSlotMinutes()) plus one step, not the raw
  // now-minutes value: mapSolarForecastToPlotData()'s loop only ever
  // visits exact-on-grid times, so comparing them against an un-rounded
  // "now" can (and, before this fix, did -- see that method's own
  // comment) let this overwrite the very slot GatewayRefresh.cpp just
  // wrote a live GREEN reading into, roughly half the time.
  int skipBeforeMinutes = PlotData::nearestSlotMinutes(localTimeMinutesSinceMidnight()) + PlotData::STEP_MINUTES;
  mapSolarForecastToPlotData(todaySolarForecast, solarData, PREDICTED_COLOR, skipBeforeMinutes);
  // tomorrowSolarData + everPainted together, under lockTomorrowForecastData()
  // (PlotDataInstances.h) -- DischargeToGridTask.cpp's own background-task
  // read of both must never see everPainted already true while this paint
  // is still in progress. Nested inside lockSolarForecast() above -- a
  // different mutex, and nobody ever takes them in the reverse order, so
  // no deadlock risk.
  lockTomorrowForecastData();
  mapSolarForecastToPlotData(tomorrowSolarForecast, tomorrowSolarData, PREDICTED_COLOR);
  everPainted = true;
  unlockTomorrowForecastData();
  todayNoonKw = todaySolarForecast.estimatedKw(12);
  tomorrowNoonKw = tomorrowSolarForecast.estimatedKw(12);
  // Debug-only, alongside the estimates below -- lets a boot log confirm
  // whether the morning-floor correction's cloud_cover reading is
  // plausible (e.g. high cloud legitimately keeping the floor below the
  // shade-corrected estimate) rather than having to guess, per
  // MorningFloorTable.h's own cloud-scaling comment.
  todayNoonCloudPct = todaySolarForecast.hours[12].cloudCoverPct;
  tomorrowNoonCloudPct = tomorrowSolarForecast.hours[12].cloudCoverPct;
  unlockSolarForecast();

  if (debugPtr) {
    debugPtr->print(F("SolarForecast: solarData/tomorrowSolarData updated from forecast, noon estimates today="));
    debugPtr->print(todayNoonKw, 2);
    debugPtr->print(F(" kW ("));
    debugPtr->print(todayNoonCloudPct, 0);
    debugPtr->print(F("% cloud) tomorrow="));
    debugPtr->print(tomorrowNoonKw, 2);
    debugPtr->print(F(" kW ("));
    debugPtr->print(tomorrowNoonCloudPct, 0);
    debugPtr->println(F("% cloud)"));
  }
}
// ================= end of SolarForecastRefresh.cpp  file
