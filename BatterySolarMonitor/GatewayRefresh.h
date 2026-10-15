// GatewayRefresh.h  file  =================
// Call every loop(): if GatewayPollTask.h's background task has a new
// gateway reading, writes it into solarData/consumptionData (PlotData.h)
// at the current 15-min slot, coloured as actual/measured data -- mirrors
// SolarForecastRefresh.h's checkSolarForecastRefresh().
#ifndef GATEWAY_REFRESH_H
#define GATEWAY_REFRESH_H

void checkGatewayRefresh();

#endif
// ================= end of GatewayRefresh.h  file
