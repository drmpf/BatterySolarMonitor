// SolarForecastTask.h  file  =================
// Runs fetchSolarForecast() (SolarForecastFetch.h) on a fixed interval,
// so the HTTPS request -- which can take seconds -- never blocks the
// main loop() that services the pfod connection. pfodAI-guide.md section
// 7 rule 8: "Don't block in loop()."
//
// Since 2026-09-11 this single fetch/poll also covers the daily weather
// forecast (today's/tomorrow's temp max/min + rainfall) -- Open-Meteo
// returns both the hourly solar and daily weather blocks in one response,
// so the BOM-specific WeatherForecastTask.h/WeatherForecastFetch.h that
// used to run a second, separate fetch for this were removed; this file's
// lock now guards WeatherForecastInstances.h's shared instances too, not
// just SolarForecastInstances.h's.
//
// checkSolarForecastPoll() runs on InternetPollTask.cpp's single shared
// background task, alongside the other internet-facing polls (Gateway,
// Enlighten history) -- see that file for why they all now share one task
// instead of each having its own.
//
// The fetch itself runs with NO lock held (see SolarForecastFetch.h's
// design -- it writes into local instances, not the shared globals
// directly), so the mutex below is only ever held for the brief moment of
// copying a finished result into todaySolarForecast/tomorrowSolarForecast
// (SolarForecastInstances.h) and todayWeatherForecast/tomorrowWeatherForecast
// (WeatherForecastInstances.h) -- not for the whole HTTP round-trip.
// Anything else that reads those shared instances (e.g. mapping solar onto
// a PlotData chart, SolarForecastMapping.h, or Dwg_StatusApp.cpp reading
// the weather instances) must take the same lock, briefly, so a read
// can't run concurrently with that copy.
#ifndef SOLAR_FORECAST_TASK_H
#define SOLAR_FORECAST_TASK_H

// Runs fetchSolarForecast() if due (every FETCH_INTERVAL_MS -- see
// SolarForecastTask.cpp). Call repeatedly from InternetPollTask.cpp's
// shared background task -- a cheap millis() check and an immediate
// return if not due yet; does the actual (multi-second) network I/O
// inline when it is.
void checkSolarForecastPoll();

// Takes/releases the mutex guarding todaySolarForecast/tomorrowSolarForecast.
// Hold it only briefly (a struct copy or a PlotData mapping loop) --
// never around network I/O or anything else slow.
void lockSolarForecast();
void unlockSolarForecast();

// Returns true the first time this is called after a successful fetch,
// then false again until the next one -- a one-shot "new data arrived"
// signal for loop() to react to (see SolarForecastRefresh.h), rather than
// remapping onto PlotData on every loop() iteration regardless of whether
// anything changed. Thread-safe.
bool solarForecastUpdated();

#endif
// ================= end of SolarForecastTask.h  file
