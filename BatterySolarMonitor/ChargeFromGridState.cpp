// ChargeFromGridState.cpp  file  =================
#include "ChargeFromGridState.h"
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static bool polledEnabled = false;
static int scheduleStartMinutes = -1; // -1 == "nothing polled yet"
static int scheduleEndMinutes = -1;
static float scheduleTargetSoc = -1.0;

// Mutex-protected as of 2026-10-13, when BatteryProjection.cpp's own
// charge-from-grid override started reading these fields -- a genuine
// cross-task read (HotDayTask.cpp's background-task tick writes via
// setCfgPolledStatus()/setCfgPolledEnabledAssumed(); BatteryProjection.cpp
// reads them from the main thread), same shape as
// DischargeToGridState.h's own mutex-guarded fields. Before that, nothing
// outside HotDayTask.cpp's own tick touched these, so no lock was needed;
// that's no longer true. Created on first use, not at global-construction
// time -- see CarChargerTask.cpp's identical lazy-init comment for why.
static SemaphoreHandle_t stateMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (stateMutex == NULL) {
    stateMutex = xSemaphoreCreateMutex();
  }
  return stateMutex;
}

static Print *dbg() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  return debugPtr;
}

bool cfgPolledEnabled() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  bool result = polledEnabled;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

int cfgScheduleStartMinutes() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  int result = scheduleStartMinutes;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

int cfgScheduleEndMinutes() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  int result = scheduleEndMinutes;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

float cfgScheduleTargetSoc() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  float result = scheduleTargetSoc;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

void setCfgPolledStatus(bool enabled, int startMinutes, int endMinutes, float targetSoc) {
  dbg();
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  polledEnabled = enabled;
  scheduleStartMinutes = startMinutes;
  scheduleEndMinutes = endMinutes;
  scheduleTargetSoc = targetSoc;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("ChargeFromGridState: polled enabled="));
    debugPtr->print(enabled ? F("true") : F("false"));
    debugPtr->print(F(" window(min)="));
    debugPtr->print(startMinutes);
    debugPtr->print(F(".."));
    debugPtr->print(endMinutes);
    debugPtr->print(F(" targetSoc="));
    debugPtr->println(targetSoc, 1);
  }
}

void setCfgPolledEnabledAssumed(bool enabled) {
  dbg();
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  polledEnabled = enabled;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("ChargeFromGridState: polledEnabled optimistically set to "));
    debugPtr->print(enabled ? F("true") : F("false"));
    debugPtr->println(F(" (assumed success -- next status poll confirms or corrects it)"));
  }
}
// ================= end of ChargeFromGridState.cpp  file
