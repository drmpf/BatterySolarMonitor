// AvgConsumptionTask.cpp  file  =================
#include "AvgConsumptionTask.h"
#include "EnlightenHistoryFetch.h"
#include "AvgConsumptionInstances.h"
#include "LocalTimeMinutes.h"
#include <Arduino.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// "Last 5 days" -- matches dailyPrediction/plot_today_solar.py's own
// load_avg_consumption_profile() window.
static const int DAYS_TO_AVERAGE = 5;

// Created on first use, not at global-construction time -- see
// SolarForecastTask.cpp's identical comment for why.
static SemaphoreHandle_t avgMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (avgMutex == NULL) {
    avgMutex = xSemaphoreCreateMutex();
  }
  return avgMutex;
}

void lockAvgConsumptionProfile() {
  xSemaphoreTake(mutex(), portMAX_DELAY);
}

void unlockAvgConsumptionProfile() {
  xSemaphoreGive(mutex());
}

static volatile bool requestPending = false;

void requestAvgConsumptionFetch() {
  lockAvgConsumptionProfile();
  requestPending = true;
  unlockAvgConsumptionProfile();
}

bool avgConsumptionAttemptRunning() {
  bool result;
  lockAvgConsumptionProfile();
  result = requestPending;
  unlockAvgConsumptionProfile();
  return result;
}

void checkAvgConsumptionPoll() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  if (!avgConsumptionAttemptRunning()) {
    return; // no request pending
  }

  int successCount = 0;
  for (int daysAgo = 1; daysAgo <= DAYS_TO_AVERAGE; daysAgo++) {
    char dateStr[11];
    localDateStringDaysAgo(daysAgo, dateStr, sizeof(dateStr));

    float dayKw[PlotData::MAX_POINTS];
    float dayTotalKwh = 0.0;
    bool ok = fetchEnlightenConsumptionForDate(dateStr, dayKw, &dayTotalKwh);
    if (ok) {
      lockAvgConsumptionProfile();
      avgConsumptionProfile.addDay(dayKw, dayTotalKwh);
      successCount = avgConsumptionProfile.successCount;
      unlockAvgConsumptionProfile();
      if (debugPtr) {
        debugPtr->print(F("AvgConsumption: "));
        debugPtr->print(dateStr);
        debugPtr->print(F(" ("));
        debugPtr->print(dayTotalKwh, 2);
        debugPtr->print(F("kWh) folded in, now averaging "));
        debugPtr->print(successCount);
        debugPtr->println(F(" day(s)"));
      }
    } else if (debugPtr) {
      debugPtr->print(F("AvgConsumption: "));
      debugPtr->print(dateStr);
      debugPtr->println(F(" fetch failed, skipping"));
    }
  }

  lockAvgConsumptionProfile();
  requestPending = false;
  unlockAvgConsumptionProfile();

  if (debugPtr) {
    debugPtr->print(F("AvgConsumption: batch done, "));
    debugPtr->print(successCount);
    debugPtr->print(F(" of "));
    debugPtr->print(DAYS_TO_AVERAGE);
    debugPtr->println(F(" day(s) successful"));
  }
}
// ================= end of AvgConsumptionTask.cpp  file
