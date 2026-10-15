// SolarForecastMapping.h  file  =================
// Maps an hourly SolarForecast (SolarForecast.h) onto a 15-minute PlotData
// chart line (PlotData.h), linearly interpolating between adjacent hours
// to reach PlotData's finer resolution.
//
// A pure function -- no locking, no globals, no pfod types. If the
// SolarForecast passed in is one of the shared instances in
// SolarForecastInstances.h (todaySolarForecast/tomorrowSolarForecast), the
// CALLER must hold lockSolarForecast()/unlockSolarForecast()
// (SolarForecastTask.h) around the call, since the background fetch task
// writes those instances from another thread.
#ifndef SOLAR_FORECAST_MAPPING_H
#define SOLAR_FORECAST_MAPPING_H

#include "SolarForecast.h"
#include "PlotData.h"

// Fills 'plot' across its whole 5am-10pm range from 'forecast', using
// estimatedKw() at each hour and linearly interpolating between the hour
// before and the hour after each 15-minute point. 'color' is applied to
// every point written (this function makes no actual/predicted
// distinction -- that's the caller's choice, e.g. by picking which
// PlotData and which color to pass).
//
// 'skipBeforeMinutes' (default PlotData::START_MINUTES, i.e. no skip)
// leaves points earlier than it untouched -- pass the current
// local-time minutes-since-midnight (LocalTimeMinutes.h) when 'plot' may
// already hold real/measured data for elapsed times (GatewayRefresh.h)
// that this forecast refresh must not overwrite.
void mapSolarForecastToPlotData(const SolarForecast &forecast, PlotData &plot, uint8_t color, int skipBeforeMinutes = PlotData::START_MINUTES);

#endif
// ================= end of SolarForecastMapping.h  file
