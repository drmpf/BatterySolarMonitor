// SolarForecastRefresh.h  file  =================
// Reacts to a new solar forecast (SolarForecastTask.h's
// solarForecastUpdated()) by remapping it onto both the Home chart's
// solarData and the Tomorrow chart's tomorrowSolarData (both PlotData,
// via SolarForecastMapping.h). Call once per loop() iteration -- cheap
// when there is nothing new (a single flag check).
//
// solarData is now fully overwritten by the forecast (green, no
// actual/predicted split) -- there's no real production data source wired
// in yet to split against, so the earlier dummy actual/predicted demo
// (Dwg_SolarPlot1App.cpp's init()) gets replaced here once the first
// forecast fetch succeeds. Revisit the colour split once a real "actual"
// source exists.
#ifndef SOLAR_FORECAST_REFRESH_H
#define SOLAR_FORECAST_REFRESH_H

void checkSolarForecastRefresh();

// True once solarData's future slots have been painted with a real
// forecast at least once this boot (sticky -- stays true after the first
// successful paint, unlike SolarForecastTask.h's own solarForecastUpdated(),
// which clears itself the moment this file consumes it and so can't be
// used as an ongoing "is there a real forecast in there" signal). Callers
// that read solarData's future slots as a forecast (BatteryProjection.cpp)
// must check this first -- same reasoning, and same bug this fixes, as
// AvgConsumptionRefresh.h's avgConsumptionForecastReady() for
// consumptionData.
bool solarForecastReady();

#endif
// ================= end of SolarForecastRefresh.h  file
