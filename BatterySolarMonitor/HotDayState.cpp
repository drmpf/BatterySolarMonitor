// HotDayState.cpp  file  =================
#include "HotDayState.h"
#include "BatteryChargeSettings.h" // hwSocOnPercent()/evSocOnPercent()/hotDayHwOnPercent()/hotDayCarOnPercent()/tomorrowsMaxTempTriggerC()
#include "WeatherForecastInstances.h" // tomorrowWeatherForecast
#include "SolarForecastTask.h" // lockSolarForecast()/unlockSolarForecast() -- also guards tomorrowWeatherForecast
#include <ESP_PicoW_LittleFSsupport.h> // initializeFS()
#include <LittleFS.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const char *STATE_PATH = "/hot_day_state.txt";

static bool loaded = false;
static bool active = false; // first-boot/missing-file default -- see header comment

// Read from both InternetPollTask.cpp's background task (HotDayTask.cpp's
// own checkHotDayLogic(), and LoadControlTask.cpp's
// effectiveHwSocOnPercent()/effectiveEvSocOnPercent() callers) and
// whatever task services pfodWeb HTTP requests (pfodMainMenuApp.cpp's
// onAutoBatteryChargeDisPressed(), when the master switch is turned off)
// -- same two-caller situation, same bounded-wait mutex idiom, as
// DischargeToGridState.cpp's own mutex(). Created on first use, not at
// global-construction time -- see CarChargerTask.cpp's identical
// lazy-init comment for why.
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
      debugPtr->println(F("HotDayState: LittleFS mount failed -- could not save"));
    }
    return;
  }
  File f = LittleFS.open(STATE_PATH, "w");
  if (!f) {
    if (debugPtr) {
      debugPtr->println(F("HotDayState: could not open state file for writing"));
    }
    return;
  }
  f.print(active ? F("active=1\n") : F("active=0\n"));
  f.close();
}

// Loads the saved latch -- NO boot-time override, unlike
// DischargeToGridState.cpp's identically-shaped loadIfNeeded(): per
// explicit instruction this must survive exactly as left across a reboot,
// including this device's own daily midnight one.
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

  bool savedActive = false; // first-boot/missing-file default
  bool haveSaved = false;
  if (initializeFS()) {
    File f = LittleFS.open(STATE_PATH, "r");
    if (f) {
      while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line == "active=0") {
          savedActive = false;
          haveSaved = true;
        } else if (line == "active=1") {
          savedActive = true;
          haveSaved = true;
        }
      }
      f.close();
    } else if (debugPtr) {
      debugPtr->println(F("HotDayState: no saved state file yet -- defaulting to inactive"));
    }
  } else if (debugPtr) {
    debugPtr->println(F("HotDayState: LittleFS mount failed -- defaulting to inactive"));
  }

  active = savedActive;
  if (!haveSaved) {
    save(); // first boot -- write the default so it's on disk from here on
  }
  if (debugPtr) {
    debugPtr->print(F("HotDayState: hotDayActive loaded as "));
    debugPtr->println(active ? F("true") : F("false"));
  }
}

bool hotDayActive() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  bool result = active;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

void setHotDayActive(bool newActive) {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  if (active == newActive) {
    if (haveLock) {
      xSemaphoreGive(mutex());
    }
    return; // no real transition -- skip the LittleFS write
  }
  active = newActive;
  save();
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("HotDayState: hotDayActive -> "));
    debugPtr->println(newActive ? F("true") : F("false"));
  }
}

int effectiveHwSocOnPercent() {
  return hotDayActive() ? hotDayHwOnPercent() : hwSocOnPercent();
}

int effectiveHwSocOffPercent() {
  return hotDayActive() ? hotDayHwOffPercent() : hwSocOffPercent();
}

int effectiveEvSocOnPercent() {
  return hotDayActive() ? hotDayCarOnPercent() : evSocOnPercent();
}

int effectiveEvSocOffPercent() {
  return hotDayActive() ? hotDayCarOffPercent() : evSocOffPercent();
}

bool readHotDayForecast(bool &isHotOut) {
  lockSolarForecast();
  bool ready = tomorrowWeatherForecast.received;
  float tempMaxC = tomorrowWeatherForecast.tempMaxC;
  unlockSolarForecast();
  if (!ready) {
    return false; // forecast not in yet -- never guess
  }
  isHotOut = (tempMaxC >= (float)tomorrowsMaxTempTriggerC());
  return true;
}
// ================= end of HotDayState.cpp  file
