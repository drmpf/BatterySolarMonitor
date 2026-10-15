// SolarForecastInstances.h  file  =================
// One SolarForecast instance per day -- declared here, defined once in
// SolarForecastInstances.cpp, so both the (not yet written) fetch code and
// whatever later maps this onto solarData/tomorrowSolarData (PlotData.h)
// can reach the same instances.
#ifndef SOLAR_FORECAST_INSTANCES_H
#define SOLAR_FORECAST_INSTANCES_H

#include "SolarForecast.h"

extern SolarForecast todaySolarForecast;
extern SolarForecast tomorrowSolarForecast;

#endif
// ================= end of SolarForecastInstances.h  file
