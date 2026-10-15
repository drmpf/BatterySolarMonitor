// SolarForecastTask.cpp  file  =================
#include "SolarForecastTask.h"
#include "SolarForecastFetch.h"
#include "SolarForecastInstances.h"
#include "WeatherForecastInstances.h"
#include <Arduino.h>
#include <pfodDebugPtr.h>
#include <pfodDelay.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// How often to re-fetch the forecast.
static const unsigned long FETCH_INTERVAL_MS = 30UL * 60UL * 1000UL; // 30 minutes

// Created on first use (by whichever of checkSolarForecastPoll()/
// lockSolarForecast() runs first) rather than at global-construction time,
// since FreeRTOS primitives aren't safe to create before the scheduler/heap
// are up.
static SemaphoreHandle_t solarForecastMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (solarForecastMutex == NULL) {
    solarForecastMutex = xSemaphoreCreateMutex();
  }
  return solarForecastMutex;
}

void lockSolarForecast() {
  xSemaphoreTake(mutex(), portMAX_DELAY);
}

void unlockSolarForecast() {
  xSemaphoreGive(mutex());
}

// Set true (under the lock) right after a successful fetch is copied into
// the shared instances; cleared by solarForecastUpdated() once whoever
// reacts to it (SolarForecastRefresh.cpp) has picked it up.
static volatile bool forecastUpdatedFlag = false;

bool solarForecastUpdated() {
  bool result = false;
  lockSolarForecast();
  if (forecastUpdatedFlag) {
    result = true;
    forecastUpdatedFlag = false;
  }
  unlockSolarForecast();
  return result;
}

// pfodDelay (pfodParser/src/pfodDelay.h), not a plain millis()/nextDueMs
// comparison -- see GatewayPollTask.cpp's identical comment on its own
// pollTimer for why, and for the isRunning()/justFinished() usage idiom
// (https://www.forward.com.au/pfod/ArduinoProgramming/TimingDelaysInArduino.html)
// followed here.
static pfodDelay pollTimer;

// Fetches into LOCAL instances (this is where the multi-second HTTPS
// request happens), then takes the lock only to copy a finished result
// into the shared todaySolarForecast/tomorrowSolarForecast AND
// todayWeatherForecast/tomorrowWeatherForecast -- both come from the same
// Open-Meteo request/response since 2026-09-11 (see SolarForecastFetch.h),
// so one fetch/lock/copy here now serves both WeatherForecastInstances.h
// (no separate WeatherForecastTask.cpp any more) and SolarForecastInstances.h.
void checkSolarForecastPoll() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  if (!pollTimer.isRunning()) {
    pollTimer.start(0); // due immediately the first time InternetPollTask.cpp ticks this
  }
  if (!pollTimer.justFinished()) {
    return;
  }

  unsigned long startMs = millis();
  SolarForecast localToday;
  SolarForecast localTomorrow;
  WeatherForecast localTodayWeather;
  WeatherForecast localTomorrowWeather;
  bool ok = fetchSolarForecast(localToday, localTomorrow, localTodayWeather, localTomorrowWeather);
  unsigned long elapsedMs = millis() - startMs;

  if (ok) {
    lockSolarForecast();
    todaySolarForecast = localToday;       // brief -- struct copy only
    tomorrowSolarForecast = localTomorrow;
    todayWeatherForecast = localTodayWeather;
    tomorrowWeatherForecast = localTomorrowWeather;
    forecastUpdatedFlag = true;
    unlockSolarForecast();
    if (debugPtr) {
      debugPtr->print(F("SolarForecast: fetch OK ("));
      debugPtr->print(elapsedMs);
      debugPtr->println(F("ms)"));
    }
  } else {
    if (debugPtr) {
      debugPtr->print(F("SolarForecast: fetch FAILED ("));
      debugPtr->print(elapsedMs);
      debugPtr->println(F("ms)"));
    }
  }

  pollTimer.start(FETCH_INTERVAL_MS);
}
// ================= end of SolarForecastTask.cpp  file
