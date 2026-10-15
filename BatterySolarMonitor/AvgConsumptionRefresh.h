// AvgConsumptionRefresh.h  file  =================
// Call every loop(). Once local time is NTP-synced, triggers
// AvgConsumptionTask.h's up-to-5-day fetch batch (re-resolves candidate
// IPs -- EnlightenHistoryFetch.h's resolveEnlightenHosts() -- on the main
// thread and posts the request). Normally this happens ONCE per boot, not
// recurring (unlike EnlightenHistoryRefresh.h's "today" backfill, a 5-day
// average doesn't go stale within a single boot session) -- EXCEPT if the
// whole batch comes back with ZERO successful days, which is retried every
// 5 minutes until at least one day succeeds (added 2026-09-11, see
// checkAvgConsumptionRefresh()'s own comment for why: a device that boots
// without an existing /auth_token.csv can race this batch's own readiness
// gate against EnlightenLoginFetch.h's boot-time self-refresh still being
// in flight, guaranteeing every date fetch fails with an empty session
// cookie -- confirmed in deviceLog.txt -- and without a retry that
// permanently blocks the consumption forecast, and so
// BatteryProjection.cpp's SOC forecast too, for the rest of the day).
//
// From when the batch is first requested, every call compares
// avgConsumptionProfile.successCount (AvgConsumptionInstances.h) against
// what was painted last time and, if it's gone up, immediately repaints
// consumptionData's future slots AND tomorrowAvgConsumptionData's whole
// day with the LATEST average -- this happens as soon as the FIRST of the
// up-to-5 days succeeds (on the background task, while the rest of the
// batch may still be running), not just once the whole batch finishes, so
// the forecast is usable (and keeps improving) well before all 5 days are
// in. If NO day has succeeded yet (successCount still 0), both are left
// untouched (no forecast plotted) rather than painting a meaningless
// all-zero average.
#ifndef AVG_CONSUMPTION_REFRESH_H
#define AVG_CONSUMPTION_REFRESH_H

void checkAvgConsumptionRefresh();

// True once at least one successful day has actually been painted onto
// consumptionData's future slots (i.e. the first repaint described above
// has happened) -- deliberately NOT the same as
// avgConsumptionProfile.hasData() (AvgConsumptionInstances.h), which
// flips true the instant the background task's addDay() call returns,
// slightly before checkAvgConsumptionRefresh() next runs and actually
// paints it. Callers that read consumptionData's future slots as a
// forecast (BatteryProjection.cpp) must wait for THIS, not hasData().
// Single main-thread flag -- both the writer (checkAvgConsumptionRefresh(),
// called from loop()) and every reader (also called from loop()) run on
// the same thread, so no lock is needed.
bool avgConsumptionForecastReady();

#endif
// ================= end of AvgConsumptionRefresh.h  file
