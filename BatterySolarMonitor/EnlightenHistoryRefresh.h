// EnlightenHistoryRefresh.h  file  =================
// Call every loop(). Once local time is NTP-synced, drives the whole
// Enlighten history fetch from the main thread: re-resolves candidate IPs
// (EnlightenHistoryFetch.h's resolveEnlightenHosts()) and starts one
// background fetch attempt (EnlightenHistoryTask.h); if that attempt
// fails, waits a short delay and tries again (fresh resolve each time --
// see EnlightenHistoryTask.h for why re-resolving matters); once an
// attempt succeeds, merges its result into solarData/consumptionData/
// batteryChargeData (PlotDataInstances.h) for whatever elapsed slots it
// covers -- overwriting GatewayRefresh.cpp's/SolarForecastRefresh.cpp's
// own more-recent-but-instantaneous points with Enphase's true 15-min
// average for the same slots -- then waits REFRESH_INTERVAL_MS (5
// minutes, per explicit instruction -- shorter than Enphase's own
// 15-minute history slot resolution, specifically to pick up a
// newly-available slot sooner: at the old 15min cadence, a refresh
// landing just before the cloud side advanced to a new slot meant
// waiting almost a full 15 more minutes to pick it up; 5min caps that
// worst-case delay at ~5min, even though most individual refreshes still
// just re-confirm a slot that hasn't advanced yet) before doing it all
// again, indefinitely -- this is a recurring smoothing/backfill pass,
// not a one-time boot backfill.
#ifndef ENLIGHTEN_HISTORY_REFRESH_H
#define ENLIGHTEN_HISTORY_REFRESH_H

// Call once from setup(), before loop() ever calls checkEnlightenHistory()
// -- arms the first-attempt uptime floor. See BatterySolarMonitor.ino.
void initializeEnlightenHistoryRefresh();

void checkEnlightenHistory();

#endif
// ================= end of EnlightenHistoryRefresh.h  file
