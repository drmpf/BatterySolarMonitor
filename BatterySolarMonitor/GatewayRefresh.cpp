// GatewayRefresh.cpp  file  =================
#include "GatewayRefresh.h"
#include "GatewayPollTask.h"
#include "GatewayInstances.h"
#include "PlotDataInstances.h"
#include "LocalTimeMinutes.h"
#include "ntpSupport.h" // missedSNTPupdate()
#include <Arduino.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// Matches the ACTUAL_COLOR already established on each chart for
// measured data: Dwg_SolarPlot1App.cpp's GREEN, Dwg_ConsumptionPlot1App.cpp's
// BLUE, Dwg_BatteryPlot1App.cpp's 208.
static const uint8_t SOLAR_ACTUAL_COLOR = 2;        // GREEN
static const uint8_t CONSUMPTION_ACTUAL_COLOR = 12; // BLUE
static const uint8_t BATTERY_ACTUAL_COLOR = 208;    // matches the original decorative curve's colour

void checkGatewayRefresh() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  if (!gatewayReadingUpdated()) {
    return;
  }
  if (missedSNTPupdate()) {
    return; // no real clock yet -- can't trust "now" enough to place this actual reading on the chart
  }

  float solarKw, consumeKw, batterySocPercent;
  lockGatewayReading();
  solarKw = latestSolarKw;
  consumeKw = latestConsumeKw;
  batterySocPercent = latestBatterySocPercent;
  unlockGatewayReading();

  int nowMinutes = localTimeMinutesSinceMidnight();
  solarData.setPointAtTime(nowMinutes, solarKw, SOLAR_ACTUAL_COLOR);
  consumptionData.setPointAtTime(nowMinutes, consumeKw, CONSUMPTION_ACTUAL_COLOR);
  if (batterySocPercent >= 0.0) { // -1 sentinel -- that poll's battery-SOC fetch failed, see GatewayFetch.h
    batteryChargeData.setPointAtTime(nowMinutes, batterySocPercent, BATTERY_ACTUAL_COLOR);
  }

  if (debugPtr) {
    debugPtr->print(F("Gateway: actual point set at "));
    debugPtr->print(nowMinutes / 60);
    debugPtr->print(F(":"));
    if ((nowMinutes % 60) < 10) {
      debugPtr->print('0');
    }
    debugPtr->print(nowMinutes % 60);
    debugPtr->print(F(" solar="));
    debugPtr->print(solarKw, 2);
    debugPtr->print(F("kW consume="));
    debugPtr->print(consumeKw, 2);
    debugPtr->print(F("kW"));
    if (batterySocPercent >= 0.0) {
      debugPtr->print(F(" battery="));
      debugPtr->print(batterySocPercent, 1);
      debugPtr->print('%');
    }
    debugPtr->println();
  }
}
// ================= end of GatewayRefresh.cpp  file
