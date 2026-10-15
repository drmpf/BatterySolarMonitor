// GatewayFetch.h  file  =================
// Fetches current solar production, whole-home consumption, and battery
// SOC from the local Enphase IQ Gateway -- same endpoints as
// dailyPrediction/plot_today_solar.py's fetch_battery_status()
// (/ivp/meters + /ivp/meters/readings for solar/consumption,
// /ivp/ensemble/power for battery SOC).
//
// This is the controller's "update" half (pfodAI-guide.md section 6) --
// see GatewayPollTask.h for how this is run on a background thread.
#ifndef GATEWAY_FETCH_H
#define GATEWAY_FETCH_H

// Runs the fetch and fills solarKw/consumeKw/batterySocPercent with the
// result. Returns true if the CORE solar/consumption fetch succeeded
// (WiFi/HTTP error, bad response, or missing production/net-consumption
// readings all return false and leave solarKw/consumeKw untouched).
//
// batterySocPercent is a separate, best-effort fetch (a different
// endpoint, /ivp/ensemble/power) -- it's set to a valid 0-100 value if
// that part also succeeded, or left at -1 (sentinel -- check for a
// negative value before using it) if it failed. A battery-SOC/power
// hiccup never discards an otherwise-good solar/consumption reading,
// since the two aren't fetched atomically anyway -- consumeKw just stays
// at its uncorrected value for that one poll in that case (see below).
//
// consumeKw is true whole-home load, matching plot_today_solar.py's
// fetch_battery_status() houseLoad_w: production + net-consumption
// (the CT-only figure, called "consume_w" there) PLUS battery power
// (positive=discharging, negative=charging -- the Envoy API's own
// convention, same sign fetched alongside batterySocPercent). The
// CT-only figure alone is NOT true load -- while the battery charges,
// that energy never reaches the grid meter, so it undercounts load by
// roughly the charge rate (verified against dailyPrediction's
// today_solar.log, e.g. "grid=+15W solar=965W batt=-857W(charging) ->
// houseLoad=123W", not 980W).
//
// Uses WiFiClientSecure::setInsecure() -- gatewayIp() (EnphaseAuth.h) is a
// local device with a self-signed cert, matching
// dailyPrediction/plot_today_solar.py's own ssl.CERT_NONE for the same
// endpoints.
bool fetchGatewayPower(float &solarKw, float &consumeKw, float &batterySocPercent);

#endif
// ================= end of GatewayFetch.h  file
