// EnlightenHistoryFetch.h  file  =================
// Fetches today's already-elapsed solar/consumption/battery-SOC history
// (15-min slots from midnight to now) from the Enlighten cloud -- same
// endpoint as dailyPrediction/plot_today_solar.py's
// fetch_consumption_today()/fetch_battery_soc_today() (one call with
// days=1&date=today returns all three: production.meter, consumption and
// acb.acb_soc_ts share the same start_date + i*900s slot scheme).
//
// Runs once near boot (backfilling the day-so-far before
// GatewayPollTask.h's live 5-min poll has had a chance to build up its
// own real-time points) and then recurring every 15 minutes after --
// EnlightenHistoryRefresh.cpp's REFRESH_INTERVAL_MS. The recurring
// refresh matters because GatewayPollTask.h's own points are
// INSTANTANEOUS local CT readings, spikier than Enphase's true 15-min
// AVERAGE for the same slot -- this periodically smooths them back to
// the cloud's true average, and (since it goes to Enphase's cloud, not
// the local gateway) also backfills any stretch where the local gateway
// connection was down -- see EnlightenHistoryTask.h.
#ifndef ENLIGHTEN_HISTORY_FETCH_H
#define ENLIGHTEN_HISTORY_FETCH_H

#include "PlotData.h"

// Resolves enlighten.enphaseenergy.com and caches the result(s) for
// fetchEnlightenHistoryToday()/fetchEnlightenConsumptionForDate() to use
// (both connect via the same shared candidate-IP list). MUST be called
// from the main
// Arduino task (e.g. EnlightenHistoryRefresh.cpp, from loop()) -- NEVER
// from EnlightenHistoryTask.h's background task -- same
// DNS-from-background-task hazard as SolarForecastFetch.h's
// resolveSolarForecastHost(), same fix.
//
// Calls lwip_getaddrinfo() directly rather than WiFi.hostByName(), which
// would only be a cosmetic difference (both return one answer per call
// on this ESP-IDF/lwIP build, confirmed on real hardware -- lwIP's DNS
// resolver only ever returns a single address, unlike a full OS resolver
// such as the one `nslookup` uses) EXCEPT that it means resolveEnlightenHosts()
// is written to hold more than one candidate if a future lwIP version
// ever returns more.
//
// enlighten.enphaseenergy.com is a CNAME to an AWS Network Load Balancer
// with several backend IPs, DNS round-robined between them, and one of
// them was confirmed (via a direct TCP connect test) to just hang rather
// than connect, from this network -- a real, external routing problem
// for that one specific IP, unrelated to the ESP32 or credentials. Since
// only one IP is ever cached per call here, EnlightenHistoryRefresh.cpp
// re-runs this before every retry (not just the first attempt) rather
// than relying on a single call's cache -- each fresh lookup has a
// decent chance of returning a different, working IP.
//
// Returns true if at least one IP was resolved.
bool resolveEnlightenHosts();

// Runs the fetch and fills 'solar'/'consumption'/'battery' (each cleared
// first) with today's elapsed history so far, coloured as
// actual/measured data (matching the ACTUAL_COLOR already established on
// each chart -- Dwg_SolarPlot1App.cpp's GREEN, Dwg_ConsumptionPlot1App.cpp's
// BLUE, Dwg_BatteryPlot1App.cpp's 208). Returns true on success; on any
// failure returns false and leaves all three untouched.
//
// Safe to call from a background task -- connects via each IP cached by
// resolveEnlightenHosts() in turn (stopping at the first success), with
// the real hostname passed separately as the TLS SNI/Host header (same
// IP+host connect trick as SolarForecastFetch.cpp's fetchSolarForecast()),
// so it never triggers a DNS lookup itself. Each candidate gets its own
// connect+response timeout, so a single bad IP can't block the caller
// for more than that long.
bool fetchEnlightenHistoryToday(PlotData &solar, PlotData &consumption, PlotData &battery);

// Fetches ONE fully-elapsed PAST day's true home consumption (kW per
// 15-min slot), for AvgConsumptionTask.h's 5-day rolling average --
// 'dateStr' must be "YYYY-MM-DD" (LocalTimeMinutes.h's
// localDateStringDaysAgo()) and strictly before today, since a day
// that's still in progress belongs to fetchEnlightenHistoryToday()
// instead. Unlike that function, this fetches the WHOLE day (no "stop at
// now" cutoff -- the whole thing has already elapsed) and only extracts
// consumption; solar/battery aren't needed for this. 'dayKw' must be
// PlotData::MAX_POINTS long and is fully overwritten on success -- -1.0
// (PlotData's own "no data" sentinel, not a fabricated 0.0) for any slot
// missing/null in the response, so AvgConsumptionProfile::addDay() can
// correctly exclude it from that slot's average instead of blending in a
// fake zero reading. If 'totalKwhOut' is non-NULL, it
// receives that day's WHOLE-day total energy (kWh, summed over every
// midnight-to-midnight slot the response has -- not just the 5am-10pm
// range 'dayKw' is clipped to, so this correctly includes overnight
// standby/off-peak load that 'dayKw' alone would miss) -- for
// AvgConsumptionProfile.h's avgDailyKwh/maxDailyKwh. Returns true on
// success; on any failure returns false and leaves 'dayKw'/'totalKwhOut'
// untouched. Safe to call from a background task, same connection
// reasoning as fetchEnlightenHistoryToday().
bool fetchEnlightenConsumptionForDate(const char *dateStr, float dayKw[PlotData::MAX_POINTS], float *totalKwhOut = nullptr);

#endif
// ================= end of EnlightenHistoryFetch.h  file
