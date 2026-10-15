// HotWaterTask.cpp  file  =================
#include "HotWaterTask.h"
#include "HotWaterFetch.h"
#include "HotWaterInstances.h"
#include <Arduino.h>
#include <pfodDelay.h>

// Created on first use, not at global-construction time -- see
// SolarForecastTask.cpp's identical comment for why.
static SemaphoreHandle_t hotWaterMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (hotWaterMutex == NULL) {
    hotWaterMutex = xSemaphoreCreateMutex();
  }
  return hotWaterMutex;
}

void lockHotWaterState() {
  xSemaphoreTake(mutex(), portMAX_DELAY);
}

void unlockHotWaterState() {
  xSemaphoreGive(mutex());
}

// Stage 2 test cadence -- matches the eventual real control cadence
// (dailyPrediction/plot_today_solar.py's control_loads() polls every
// 5 min) so this doesn't need re-tuning once the actual start/stop logic
// lands in a later stage. Same interval CarChargerTask.cpp uses.
static const unsigned long POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;

// How soon requestHotWaterRecheck() re-arms the poll for, after a toggle --
// long enough for the element to settle (a few seconds), short enough
// that the Status screen's optimistic post-toggle guess doesn't stand
// uncorrected for long.
static const unsigned long RECHECK_DELAY_MS = 20UL * 1000UL;

// pfodDelay (pfodParser/src/pfodDelay.h), not a plain millis()/nextDueMs
// comparison -- see GatewayPollTask.cpp's comment on its own pollTimer for
// why, and for the isRunning()/justFinished() usage idiom
// (https://www.forward.com.au/pfod/ArduinoProgramming/TimingDelaysInArduino.html)
// followed here.
static pfodDelay pollTimer;

void checkHotWaterPoll() {
  if (!pollTimer.isRunning()) {
    pollTimer.start(0); // due immediately the first time InternetPollTask.cpp ticks this
  }
  if (!pollTimer.justFinished()) {
    return;
  }
  HotWaterState state;
  bool ok = fetchHotWaterState(state); // logs its own result either way -- see HotWaterFetch.cpp
  if (ok) {
    lockHotWaterState();
    latestHotWaterState = state;
    hotWaterReachable = true;
    unlockHotWaterState();
  } else {
    lockHotWaterState();
    hotWaterReachable = false; // latestHotWaterState left at its last-good value -- see HotWaterInstances.h
    unlockHotWaterState();
  }
  pollTimer.start(POLL_INTERVAL_MS);
}

void requestHotWaterRecheck() {
  pollTimer.start(RECHECK_DELAY_MS);
}
// ================= end of HotWaterTask.cpp  file
