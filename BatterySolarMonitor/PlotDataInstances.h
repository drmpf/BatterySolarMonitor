// PlotDataInstances.h  file  =================
// One PlotData model instance per plotted line, one for each chart plot
// across the three charts in this design (dwgs/BatteryChartGrid,
// dwgs/PowerChartGrid and dwgs/TomorrowChart). Declared here, defined once
// in PlotDataInstances.cpp, so any controller code (the Dwg_<Name>
// subclasses' sendIndexedItems() overrides, and whatever writes real
// sensor/forecast values into these) can reach the same instances.
#ifndef PLOT_DATA_INSTANCES_H
#define PLOT_DATA_INSTANCES_H

#include "PlotData.h"

extern PlotData batteryChargeData;          // Battery Charge (%) chart
extern PlotData solarData;                  // Solar & Consumption chart -- Solar line
extern PlotData consumptionData;            // Solar & Consumption chart -- Consumption line
extern PlotData tomorrowSolarData;          // Tomorrow's Solar and Avg Consumption chart -- Solar line
extern PlotData tomorrowAvgConsumptionData; // Tomorrow's Solar and Avg Consumption chart -- Avg Consumption line

// Protects tomorrowSolarData/tomorrowAvgConsumptionData TOGETHER WITH their
// own readiness flags (SolarForecastRefresh.cpp's everPainted, read via
// solarForecastReady(); AvgConsumptionRefresh.cpp's lastPaintedSuccessCount,
// read via avgConsumptionForecastReady()) -- the only two instances here
// ever read from a thread other than the one that writes them.
// DischargeToGridTask.cpp's own checkDischargeToGridLogic(), on
// InternetPollTask.cpp's shared background task, holds this across BOTH
// readiness checks AND both totalKwh() reads as one atomic snapshot, so it
// can never see a readiness flag that's already true while the matching
// paint is still in progress. SolarForecastRefresh.cpp/
// AvgConsumptionRefresh.cpp (main thread) hold it the same way around
// painting their own instance AND setting its own readiness flag together.
// batteryChargeData/solarData/consumptionData are only ever touched from
// the main thread, so deliberately NOT covered by this lock -- no reason
// to make every main-thread-only chart pay for a mutex it never needs.
//
// RECURSIVE (xSemaphoreCreateRecursiveMutex, not a plain mutex):
// solarForecastReady()/avgConsumptionForecastReady() also take this lock
// internally, so they stay safe to call standalone from their existing
// main-thread-only callers (BatteryProjection.cpp, Dwg_StatusApp.cpp) that
// have no reason to know this lock exists. A plain mutex would deadlock
// the moment DischargeToGridTask.cpp holds the lock across its own call to
// those same functions.
void lockTomorrowForecastData();
void unlockTomorrowForecastData();

#endif
// ================= end of PlotDataInstances.h  file
