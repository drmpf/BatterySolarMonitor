// WeatherForecastInstances.h  file  =================
// One WeatherForecast instance per day -- declared here, defined once in
// WeatherForecastInstances.cpp, so both SolarForecastTask.h's fetch loop
// (fetches solar + weather together in one Open-Meteo request -- see
// SolarForecastFetch.h) and whatever displays it (Dwg_StatusApp.cpp) can
// reach the same instances. Guarded by SolarForecastTask.h's
// lockSolarForecast() -- see that file.
#ifndef WEATHER_FORECAST_INSTANCES_H
#define WEATHER_FORECAST_INSTANCES_H

#include "WeatherForecast.h"

extern WeatherForecast todayWeatherForecast;
extern WeatherForecast tomorrowWeatherForecast;

#endif
// ================= end of WeatherForecastInstances.h  file
