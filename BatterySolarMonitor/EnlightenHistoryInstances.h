// EnlightenHistoryInstances.h  file  =================
#ifndef ENLIGHTEN_HISTORY_INSTANCES_H
#define ENLIGHTEN_HISTORY_INSTANCES_H

#include "PlotData.h"

// Staged copy of today's Enlighten history fetch -- written by
// EnlightenHistoryTask.cpp's background task, read by
// EnlightenHistoryRefresh.cpp on the main thread, which merges these into
// the real solarData/consumptionData/batteryChargeData
// (PlotDataInstances.h). Always access under
// lockEnlightenHistory()/unlockEnlightenHistory() (EnlightenHistoryTask.h)
// -- same pattern as GatewayInstances.h/GatewayPollTask.h.
extern PlotData enlightenSolarHistory;
extern PlotData enlightenConsumptionHistory;
extern PlotData enlightenBatteryHistory;

#endif
// ================= end of EnlightenHistoryInstances.h  file
