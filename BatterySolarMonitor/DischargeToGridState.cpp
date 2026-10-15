// DischargeToGridState.cpp  file  =================
#include "DischargeToGridState.h"
#include <ESP_PicoW_LittleFSsupport.h> // initializeFS()
#include <LittleFS.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const char *STATE_PATH = "/discharge_to_grid_state.txt";

static bool loaded = false;
static bool autoEnabled = true;  // first-boot/missing-file default -- see header comment; NOT forced after that
static bool polledEnabled = false;
static int scheduleStartMinutes = -1; // -1 == "nothing polled yet" -- see header comment
static int scheduleEndMinutes = -1;
static float scheduleEndSoc = -1.0;

// Read from both InternetPollTask.cpp's background task (the poll) and
// whatever task services pfodWeb HTTP requests (the button press, and
// every sendMainMenu()/sendMainMenuUpdate()) -- same two-caller situation,
// same bounded-wait mutex idiom, as LoadControlState.cpp's own mutex().
// Created on first use, not at global-construction time -- see
// CarChargerTask.cpp's identical lazy-init comment for why.
static SemaphoreHandle_t stateMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (stateMutex == NULL) {
    stateMutex = xSemaphoreCreateMutex();
  }
  return stateMutex;
}

static void save() {
  if (!initializeFS()) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridState: LittleFS mount failed -- could not save"));
    }
    return;
  }
  File f = LittleFS.open(STATE_PATH, "w");
  if (!f) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridState: could not open state file for writing"));
    }
    return;
  }
  f.print(autoEnabled ? F("enabled=1\n") : F("enabled=0\n"));
  f.close();
}

// Loads the saved latch -- NO boot-time override any more (changed
// 2026-10-13, per explicit instruction; see this file's own header
// comment and DischargeToGridState.h's top comment). Mirrors
// LoadControlState.cpp's loadIfNeeded() shape, and now also
// HotDayState.cpp's identically-shaped loadIfNeeded() for the companion
// Hot Day latch.
static void loadIfNeeded() {
  if (loaded) {
    return;
  }
  loaded = true;

  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  bool savedEnabled = true; // first-boot/missing-file default
  bool haveSaved = false;
  if (initializeFS()) {
    File f = LittleFS.open(STATE_PATH, "r");
    if (f) {
      while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line == "enabled=0") {
          savedEnabled = false;
          haveSaved = true;
        } else if (line == "enabled=1") {
          savedEnabled = true;
          haveSaved = true;
        }
      }
      f.close();
    } else if (debugPtr) {
      debugPtr->println(F("DischargeToGridState: no saved state file yet -- defaulting to enabled"));
    }
  } else if (debugPtr) {
    debugPtr->println(F("DischargeToGridState: LittleFS mount failed -- defaulting to enabled"));
  }

  autoEnabled = savedEnabled;
  if (!haveSaved) {
    save(); // first boot -- write the default so it's on disk from here on
  }
  if (debugPtr) {
    debugPtr->print(F("DischargeToGridState: autoDischargeEnabled loaded as "));
    debugPtr->println(autoEnabled ? F("enabled") : F("disabled"));
  }
}

bool autoDischargeEnabled() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  bool result = autoEnabled;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

void setAutoDischargeEnabled(bool enabled) {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  if (autoEnabled == enabled) {
    if (haveLock) {
      xSemaphoreGive(mutex());
    }
    return; // no real transition -- skip the LittleFS write
  }
  autoEnabled = enabled;
  save();
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("DischargeToGridState: autoDischargeEnabled -> "));
    debugPtr->println(enabled ? F("true") : F("false"));
  }
}

bool dtgPolledEnabled() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  bool result = polledEnabled;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

int dtgScheduleStartMinutes() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  int result = scheduleStartMinutes;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

int dtgScheduleEndMinutes() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  int result = scheduleEndMinutes;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

float dtgScheduleEndSoc() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  float result = scheduleEndSoc;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

void setDtgPolledStatus(bool enabled, int startMinutes, int endMinutes, float endSoc) {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  polledEnabled = enabled;
  scheduleStartMinutes = startMinutes;
  scheduleEndMinutes = endMinutes;
  scheduleEndSoc = endSoc;
  // Never touches autoEnabled -- see header comment (this used to force
  // it true whenever Enphase reported discharge on; removed 2026-10-13,
  // per explicit instruction).
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("DischargeToGridState: polled enabled="));
    debugPtr->print(enabled ? F("true") : F("false"));
    debugPtr->print(F(" window(min)="));
    debugPtr->print(startMinutes);
    debugPtr->print(F(".."));
    debugPtr->print(endMinutes);
    debugPtr->print(F(" endSoc="));
    debugPtr->print(endSoc, 1);
    debugPtr->println();
  }
}

void setDtgPolledEnabledAssumed(bool enabled) {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  polledEnabled = enabled;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("DischargeToGridState: polledEnabled optimistically set to "));
    debugPtr->print(enabled ? F("true") : F("false"));
    debugPtr->println(F(" (assumed success -- next status poll confirms or corrects it)"));
  }
}
// ================= end of DischargeToGridState.cpp  file
