// EnlightenHistoryTask.cpp  file  =================
#include "EnlightenHistoryTask.h"
#include "EnlightenHistoryFetch.h"
#include "EnlightenHistoryInstances.h"
#include <Arduino.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// Created on first use, not at global-construction time -- see
// SolarForecastTask.cpp's identical comment for why.
static SemaphoreHandle_t enlightenMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (enlightenMutex == NULL) {
    enlightenMutex = xSemaphoreCreateMutex();
  }
  return enlightenMutex;
}

void lockEnlightenHistory() {
  xSemaphoreTake(mutex(), portMAX_DELAY);
}

void unlockEnlightenHistory() {
  xSemaphoreGive(mutex());
}

// requestPending: set by requestEnlightenHistoryFetch() (main thread),
// cleared by checkEnlightenHistoryPoll() (shared background task) once
// that one request has been serviced, success or failure alike -- same
// role enlightenHistoryAttemptRunning() played when this was a
// dedicated, self-deleting task (true from creation to just before
// vTaskDelete()), just renamed to match what it now tracks.
static volatile bool requestPending = false;
static volatile bool historyReadyFlag = false;

void requestEnlightenHistoryFetch() {
  lockEnlightenHistory();
  requestPending = true;
  unlockEnlightenHistory();
}

bool enlightenHistoryAttemptRunning() {
  bool result;
  lockEnlightenHistory();
  result = requestPending;
  unlockEnlightenHistory();
  return result;
}

bool enlightenHistoryReady() {
  bool result = false;
  lockEnlightenHistory();
  if (historyReadyFlag) {
    result = true;
    historyReadyFlag = false;
  }
  unlockEnlightenHistory();
  return result;
}

// Fetches into LOCAL instances (this is where the multi-second HTTPS
// request happens), then takes the lock only to copy a finished result
// into the shared enlighten*History instances.
void checkEnlightenHistoryPoll() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  if (!enlightenHistoryAttemptRunning()) {
    return; // no request pending
  }

  unsigned long startMs = millis();
  PlotData localSolar, localConsumption, localBattery;
  bool ok = fetchEnlightenHistoryToday(localSolar, localConsumption, localBattery);
  unsigned long elapsedMs = millis() - startMs;

  lockEnlightenHistory();
  if (ok) {
    enlightenSolarHistory = localSolar;
    enlightenConsumptionHistory = localConsumption;
    enlightenBatteryHistory = localBattery;
    historyReadyFlag = true;
  }
  requestPending = false; // this request is serviced either way
  unlockEnlightenHistory();

  if (debugPtr) {
    if (ok) {
      debugPtr->print(F("EnlightenHistory: fetch OK ("));
    } else {
      debugPtr->print(F("EnlightenHistory: fetch FAILED ("));
    }
    debugPtr->print(elapsedMs);
    debugPtr->println(F("ms)"));
  }
}
// ================= end of EnlightenHistoryTask.cpp  file
