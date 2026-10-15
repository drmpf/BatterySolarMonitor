// SolarForecastFetch.h  file  =================
// Fetches today's and tomorrow's hourly GTI (global tilted irradiance)
// forecast, and today's/tomorrow's daily max/min temperature + rainfall
// estimate, from the Open-Meteo API in ONE request -- same hourly query as
// dailyPrediction/plot_today_solar.py's fetch_today_forecast(), plus a
// daily= parameter added 2026-09-11 (temperature_2m_max,
// temperature_2m_min, precipitation_sum) that replaced the separate BOM
// weather fetch (WeatherForecastFetch.h/.cpp, removed that day) -- Open-Meteo
// already returns both hourly and daily blocks in the same JSON response
// for the same host/connection, so folding the daily fields into this
// existing request needs no second HTTPS round-trip.
//
// This is the controller's "update" half (pfodAI-guide.md section 6):
// reads an external source and writes whatever SolarForecast/WeatherForecast
// instances are passed to it. It knows nothing about pfod, about how the
// data is displayed, or about which instances (e.g. the shared
// todaySolarForecast/tomorrowSolarForecast in SolarForecastInstances.h,
// todayWeatherForecast/tomorrowWeatherForecast in
// WeatherForecastInstances.h) the caller is using -- that separation is
// what lets SolarForecastTask.h run this on a background thread and only
// briefly lock the shared instances to copy the result in, instead of
// holding a lock for the whole HTTP request (see SolarForecastTask.h).
#ifndef SOLAR_FORECAST_FETCH_H
#define SOLAR_FORECAST_FETCH_H

#include "SolarForecast.h"
#include "WeatherForecast.h"

// Runs the fetch and fills 'today'/'tomorrow'/'todayWeather'/
// 'tomorrowWeather' with the result. Returns true on success; on any
// failure (WiFi/HTTP error, bad response) returns false and leaves all
// four untouched. A missing/malformed daily= block alone does not fail the
// whole call -- only the hourly GTI array is required (see .cpp) -- so a
// solar-only response still returns true, with 'todayWeather'/
// 'tomorrowWeather' left cleared but unfilled (received stays false).
//
// Uses WiFiClientSecure::setInsecure() -- no certificate validation. This
// project's own decision (Open-Meteo is public/read-only, so a MITM here
// can only feed false weather data, not steal credentials -- unlike the
// Enlighten Cloud fetch, not yet written, which does carry an auth token).
//
// Connects using the IP address cached by resolveSolarForecastHost() --
// see that function's comment for why. If resolveSolarForecastHost() has
// not yet succeeded, fetchSolarForecast() fails immediately (no fetch
// attempted) rather than resolving the hostname itself.
//
// Dials the IP directly but still passes the real hostname through as
// the TLS SNI (via WiFiClientSecure::connect(ip, port, host, ...)) and
// as the Host header (via HTTPClient::begin(client, host, port, uri,
// https), since the client is already connected by then) -- connecting
// via IP with the IP itself as SNI was tried first and Open-Meteo's edge
// rejected it (httpCode=-5, connection lost), so both must carry the
// real hostname even though the TCP dial itself uses the IP.
bool fetchSolarForecast(SolarForecast &today, SolarForecast &tomorrow,
                         WeatherForecast &todayWeather, WeatherForecast &tomorrowWeather);

// Resolves the Open-Meteo API hostname to an IP address and caches it for
// fetchSolarForecast() to use instead of resolving the hostname itself.
//
// MUST be called from the main Arduino task (e.g. setup(), after WiFi
// connects) -- NEVER from the background task that calls
// fetchSolarForecast() (SolarForecastTask.h). Resolving a hostname (as
// opposed to an IP-literal string) can call into ESP-IDF's
// dns_clear_cache(), which is unsafe from any task other than the one
// that owns the lwIP/TCPIP core lock -- a confirmed match to
// espressif/esp-idf#14287, and the actual root cause (confirmed via a
// symbolicated crash backtrace) of this project's earlier udp_remove
// crash, which always originated from fetchSolarForecast() running on
// the background task. Resolving once, here, on the main task avoids
// that path entirely; fetchSolarForecast() then connects using the
// cached IP (an IP-literal string), which NetworkManager::hostByName()
// short-circuits on before ever reaching dns_clear_cache().
//
// Returns true if resolution succeeded. Safe to call again later (still
// from the main task) to re-resolve, e.g. after a run of fetch failures.
bool resolveSolarForecastHost();

#endif
// ================= end of SolarForecastFetch.h  file
