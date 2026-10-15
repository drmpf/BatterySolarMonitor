// GatewayInstances.h  file  =================
#ifndef GATEWAY_INSTANCES_H
#define GATEWAY_INSTANCES_H

// Most recent gateway reading -- written by GatewayPollTask.cpp's
// background task, read by GatewayRefresh.cpp on the main thread. Always
// access under lockGatewayReading()/unlockGatewayReading()
// (GatewayPollTask.h) -- same pattern as
// SolarForecastInstances.h/SolarForecastTask.h.
extern float latestSolarKw;
extern float latestConsumeKw;
extern float latestBatterySocPercent; // -1 if the last poll's battery-SOC fetch failed -- see GatewayFetch.h

#endif
// ================= end of GatewayInstances.h  file
