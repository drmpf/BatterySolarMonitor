// SolarForecastFetch.cpp  file  =================
#include "SolarForecastFetch.h"
#include "LocalTimeMinutes.h" // localMonthAndDayOfYear() -- see estimatedKw()/setDate()
#include "SiteSettings.h" // siteLatitude()/siteLongitude()/sitePanelTilt()/sitePanelAzimuth()/sitePosixTimezone()
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <pfodDebugPtr.h>
#include <time.h> // time_t/struct tm/localtime_r()/gmtime_r()/mktime() -- see parseIsoUtcToEpoch()/dayOffsetFromToday()
#include <stdio.h> // sscanf()/snprintf()
#include <string.h> // strcmp()

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// Days since the 1970-01-01 epoch for a UTC (proleptic Gregorian) civil
// date -- Howard Hinnant's well-known days_from_civil algorithm. Used
// ONLY to turn Open-Meteo's UTC timestamps (see below) into a real epoch
// without going anywhere near mktime()/tzset(), which assume LOCAL time --
// a UTC date must never be handed to those.
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);                 // [0, 399]
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;      // [0, 146096]
  return era * 146097 + (int64_t)doe - 719468;
}

// Parses one Open-Meteo hourly.time[] entry, "YYYY-MM-DDTHH:MM" (always
// UTC here -- see fetchSolarForecast()'s timezone=UTC request), into the
// UTC epoch second it represents. Returns false (epochOut untouched) if
// the string doesn't match -- caller must then skip this entry, same as
// every other malformed/missing-field guard already in this function.
static bool parseIsoUtcToEpoch(const char *iso, time_t &epochOut) {
  if (!iso) {
    return false;
  }
  int y, mo, d, h, mi;
  if (sscanf(iso, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) {
    return false;
  }
  epochOut = (time_t)(daysFromCivil(y, mo, d) * 86400LL + h * 3600LL + mi * 60LL);
  return true;
}

// Local calendar-day offset of a local struct tm (already run through
// localtime_r()) from local "today" (0 = today, 1 = tomorrow,
// negative/other = neither -- caller discards).
static int dayOffsetFromToday(const struct tm &localTm, const struct tm &todayLocalTm) {
  int64_t day   = daysFromCivil(localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday);
  int64_t today = daysFromCivil(todayLocalTm.tm_year + 1900, todayLocalTm.tm_mon + 1, todayLocalTm.tm_mday);
  return (int)(day - today);
}

static const char *HOSTNAME = "api.open-meteo.com";
// 0.0.0.0 until resolveSolarForecastHost() succeeds -- see SolarForecastFetch.h.
static IPAddress resolvedIP((uint32_t)0);

// Same timeout bounds, and the same reasoning, as
// EnlightenHistoryFetch.cpp's HANDSHAKE_TIMEOUT_S/RESPONSE_TIMEOUT_MS --
// see that file's comment: WiFiClientSecure's own default TLS handshake
// timeout is 120s, confirmed there as the cause of an observed 135s hang.
static const unsigned long HANDSHAKE_TIMEOUT_S = 5; // seconds -- setHandshakeTimeout()'s own unit
static const uint32_t RESPONSE_TIMEOUT_MS = 8000;   // response body read timeout

bool resolveSolarForecastHost() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  IPAddress ip;
  if (!WiFi.hostByName(HOSTNAME, ip)) {
    if (debugPtr) {
      debugPtr->print(F("SolarForecastFetch: could not resolve "));
      debugPtr->println(HOSTNAME);
    }
    return false;
  }
  resolvedIP = ip;
  if (debugPtr) {
    debugPtr->print(F("SolarForecastFetch: resolved "));
    debugPtr->print(HOSTNAME);
    debugPtr->print(F(" -> "));
    debugPtr->println(resolvedIP);
  }
  return true;
}

// Fills 'day' from index 'i' of the daily temperature_2m_max/
// temperature_2m_min/precipitation_sum arrays (0=today, 1=tomorrow).
// Leaves 'day' cleared (received=false) if temperature_2m_max is
// missing/null at that index -- same "day.received means the entry itself
// was found" convention WeatherForecast.h documents. Any of the 3 arrays
// can independently be missing (isNull()) without failing the others --
// mirrors the existing cloud_cover-is-optional handling on the hourly
// side just above.
static void readDailyWeather(JsonArray tempMax, JsonArray tempMin, JsonArray rain,
                              int i, WeatherForecast &day) {
  day.clear();
  if (tempMax.isNull() || i >= (int)tempMax.size() || tempMax[i].isNull()) {
    return;
  }
  day.tempMaxC = tempMax[i].as<float>();
  day.received = true;

  if (!tempMin.isNull() && i < (int)tempMin.size() && !tempMin[i].isNull()) {
    day.tempMinC = tempMin[i].as<float>();
    day.tempMinReceived = true;
  }
  if (!rain.isNull() && i < (int)rain.size() && !rain[i].isNull()) {
    day.rainMm = rain[i].as<float>();
    day.rainReceived = true;
  }
}

bool fetchSolarForecast(SolarForecast &today, SolarForecast &tomorrow,
                         WeatherForecast &todayWeather, WeatherForecast &tomorrowWeather) {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  if (resolvedIP == IPAddress((uint32_t)0)) {
    // resolveSolarForecastHost() hasn't succeeded yet -- must not resolve
    // the hostname here, this runs on the background task (see
    // SolarForecastFetch.h for why). Skip this cycle; the next
    // SolarForecastTask retry will find resolvedIP still unset and skip
    // again until the main task resolves it.
    if (debugPtr) {
      debugPtr->println(F("SolarForecastFetch: no resolved host IP yet, skipping fetch"));
    }
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // no cert validation -- see SolarForecastFetch.h
  // Bounds the TLS handshake phase -- WiFiClientSecure's own default is
  // 120s, which was the confirmed cause of an observed 135s hang in
  // EnlightenHistoryFetch.cpp (see that file's HANDSHAKE_TIMEOUT_S
  // comment for the full explanation); same fix applied here.
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S);

  // Connect via the pre-resolved IP (no hostname lookup here -- this runs
  // on the background task) using the IP+host overload, which passes
  // HOSTNAME through as the TLS SNI/handshake hostname while dialling
  // the IP directly. A plain string-hostname connect (or IP-as-host, as
  // tried first) either resolves DNS itself or sends the IP literal as
  // SNI -- Open-Meteo's edge rejected that (httpCode=-5, connection
  // lost), presumably requiring SNI to match a real hostname.
  const uint16_t port = 443;
  if (!client.connect(resolvedIP, port, HOSTNAME, NULL, NULL, NULL)) {
    if (debugPtr) {
      debugPtr->println(F("SolarForecastFetch: TLS connect failed"));
    }
    return false;
  }
  client.setTimeout(RESPONSE_TIMEOUT_MS); // response body read timeout

  // timezone=UTC, not an IANA name (changed 2026-09-28) -- Open-Meteo's
  // archive API (used by the PC-side pipeline) was found to apply a
  // single FIXED, non-DST-aware UTC offset for an entire IANA-timezone
  // request, silently mislabeling timestamps during daylight saving; see
  // BatterySolarMonitorUpdate/docs/data_sources.md's "Timezone handling"
  // section for the full story. This forecast endpoint is requested with
  // plain UTC instead, and each returned hourly timestamp is converted to
  // local wall-clock time below via localtime_r() (parseIsoUtcToEpoch()/
  // dayOffsetFromToday()) -- the device's global TZ is already set
  // correctly by ntpSupport.cpp's ESP_setTZ() (via SiteSettings.cpp's
  // sitePosixTimezone() override), so no setenv()/tzset() call is needed
  // here. past_days=1&forecast_days=3 (4 raw UTC days) replaces the old
  // forecast_days=2 (2 raw local-aligned days): since UTC day boundaries
  // don't align with local ones, covering local "today" and "tomorrow"
  // needs 1 buffer UTC day on each side, regardless of the site's UTC
  // offset sign -- see fetch_solar_history.py's fetch_chunk_utc()
  // docstring (Python pipeline) for the full derivation of this same
  // buffering rule.
  //
  // daily= rides along on this same request/response (added 2026-09-11,
  // replacing the separate BOM weather fetch) -- Open-Meteo returns both
  // hourly and daily blocks together, so no second HTTPS round-trip is
  // needed just to get today's/tomorrow's temp max/min + rainfall.
  String uri = "/v1/forecast?"
               "latitude=" + String(siteLatitude(), 4) +
               "&longitude=" + String(siteLongitude(), 4) +
               "&hourly=global_tilted_irradiance,cloud_cover"
               "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum"
               "&tilt=" + String(sitePanelTilt(), 0) +
               "&azimuth=" + String(sitePanelAzimuth(), 0) +
               "&timezone=UTC"
               "&past_days=1"
               "&forecast_days=3";

  HTTPClient http;
  http.setTimeout(RESPONSE_TIMEOUT_MS);
  // client is already connected (above) -- HTTPClient::connect() checks
  // connected() first and reuses it rather than dialling _host itself,
  // so this begin() only needs to set the Host header/URI, never a
  // connect target.
  if (!http.begin(client, HOSTNAME, port, uri, true)) {
    if (debugPtr) {
      debugPtr->println(F("SolarForecastFetch: http.begin() failed"));
    }
    return false;
  }
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    if (debugPtr) {
      debugPtr->print(F("SolarForecastFetch: GET failed, httpCode="));
      debugPtr->print(httpCode);
      debugPtr->print(F(" ("));
      debugPtr->print(HTTPClient::errorToString(httpCode));
      debugPtr->println(F(")"));
    }
    http.end();
    return false;
  }

  // getStream() would hand back the raw socket, chunk-framing bytes and
  // all -- Open-Meteo's response is Transfer-Encoding: chunked, and
  // getStream() does NOT de-chunk (that logic lives elsewhere in
  // HTTPClient, only reached via getString()). Read the whole (correctly
  // de-chunked) body as a String first, then parse that.
  String body = http.getString();
  http.end();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    if (debugPtr) {
      debugPtr->print(F("SolarForecastFetch: JSON parse failed: "));
      debugPtr->println(err.c_str());
    }
    return false;
  }

  JsonArray gti = doc["hourly"]["global_tilted_irradiance"];
  if (gti.isNull()) {
    if (debugPtr) {
      debugPtr->println(F("SolarForecastFetch: response had no hourly.global_tilted_irradiance array"));
    }
    return false;
  }
  // Needed now (2026-09-28) to know which LOCAL hour each raw UTC row is --
  // the old index-based split (0-23=today, 24-47=tomorrow) only worked
  // because timezone=<IANA name> used to make Open-Meteo align its own
  // day boundaries to local time; timezone=UTC (see the URI comment
  // above) means this array's own day boundaries are UTC days, not local
  // ones, so each row's actual timestamp must be parsed and converted.
  JsonArray timeArr = doc["hourly"]["time"];
  if (timeArr.isNull()) {
    if (debugPtr) {
      debugPtr->println(F("SolarForecastFetch: response had no hourly.time array"));
    }
    return false;
  }
  // cloud_cover shares the same hourly index range as gti above -- used by
  // MorningFloorTable.h's clear-sky floor correction, not fatal if absent
  // (falls back to HourlyGti's own 0% "full sun" default -- see that
  // struct's own comment for why that's the safer fallback here, unlike
  // dailyPrediction/plot_today_solar.py's 50% null handling).
  JsonArray cloudCover = doc["hourly"]["cloud_cover"];

  today.clear();
  tomorrow.clear();
  todayWeather.clear();
  tomorrowWeather.clear();

  // Lets estimatedKw() compute each hour's sun position for
  // ShadeFactorTable.h's shade-factor lookup -- see SolarForecast.h's
  // dayOfYear/month comment. This is the one place that both knows which
  // instance is "today" vs "tomorrow" and can read the real NTP-synced
  // date; SolarForecast itself stays date-agnostic otherwise.
  int month, dayOfYear;
  localMonthAndDayOfYear(0, month, dayOfYear);
  today.setDate(dayOfYear, month);
  localMonthAndDayOfYear(1, month, dayOfYear);
  tomorrow.setDate(dayOfYear, month);

  // "Today", once, for every row's dayOffsetFromToday() comparison below --
  // NTP-synced local time, same clock LocalTimeMinutes.cpp reads.
  time_t nowEpoch = time(nullptr);
  struct tm todayLocalTm;
  localtime_r(&nowEpoch, &todayLocalTm);

  // Each row: parse its own UTC timestamp (hourly.time[i], always present
  // alongside any requested hourly variable -- not a separate opt-in
  // param), convert to local wall-clock time, and place it in today/
  // tomorrow's OWN local hour slot -- NOT by raw array index, since
  // timezone=UTC (see above) means the raw index no longer lines up with
  // local hours at all. Rows outside local [today, tomorrow] (the
  // past_days=1/forecast_days=3 buffer -- see the URI comment) are
  // silently skipped, same as this loop already skipped nothing extra
  // before; a row whose local time doesn't exist (the ~1 night/year
  // spring-forward gap) or is duplicated (the ~1 night/year fallback
  // repeat) is vanishingly unlikely to matter here (always deep night,
  // see BatterySolarMonitorUpdate's own solar_shading_model.py comments
  // on the equivalent case) and isn't specially handled.
  int used = 0, total = 0;
  for (JsonVariant v : gti) {
    int i = total++;
    // timeArr[i] returns an ElementProxy, not a JsonVariant directly
    // (ArduinoJson v7) -- read through its own JsonVariant first, same
    // as cloudCover[i] below.
    JsonVariant tv = timeArr[i];
    const char *iso = tv.isNull() ? nullptr : tv.as<const char *>();
    time_t epoch;
    if (!parseIsoUtcToEpoch(iso, epoch)) {
      continue; // malformed/missing timestamp -- skip this row
    }
    struct tm localTm;
    localtime_r(&epoch, &localTm);
    int offset = dayOffsetFromToday(localTm, todayLocalTm);
    if (offset != 0 && offset != 1) {
      continue; // buffer row, not local today/tomorrow
    }

    float value = v.isNull() ? 0.0 : v.as<float>();
    // cloudCover[i] returns an ElementProxy, not a JsonVariant directly
    // (ArduinoJson v7), so it's read through its own if instead of folded
    // into a ternary with cloudCover.isNull()'s branch (those two types
    // don't unify).
    float cloudCoverPct = 0.0;
    if (!cloudCover.isNull()) {
      JsonVariant cc = cloudCover[i];
      if (!cc.isNull()) {
        cloudCoverPct = cc.as<float>();
      }
    }
    if (offset == 0) {
      today.setHourGti(localTm.tm_hour, value, cloudCoverPct);
    } else {
      tomorrow.setHourGti(localTm.tm_hour, value, cloudCoverPct);
    }
    used++;
  }

  if (debugPtr) {
    debugPtr->print(F("SolarForecastFetch: got "));
    debugPtr->print(used);
    debugPtr->print(F("/"));
    debugPtr->print(total);
    debugPtr->print(F(" hourly GTI values placed into today/tomorrow"));
    if (cloudCover.isNull()) {
      debugPtr->print(F(" (no cloud_cover in response)"));
    }
    debugPtr->println();
  }

  // daily=[...] -- see the header comment above for why this rides along
  // on the same request as the hourly GTI array instead of a separate
  // fetch. A missing/malformed daily block just leaves both WeatherForecast
  // out-params unreceived (readDailyWeather()'s own isNull() guards) --
  // doesn't fail the whole call, since the hourly solar data above is
  // already good.
  //
  // Index 0/1 (used before 2026-09-28) assumed daily.time[] started at
  // local today -- true only when Open-Meteo aligned its own day
  // boundaries to local time (the old IANA-name timezone=). With
  // timezone=UTC + past_days=1 (see the URI comment above), daily.time[0]
  // is UTC YESTERDAY, not local today, so the target index is found by
  // matching against daily.time[]'s own dates instead of assuming a fixed
  // position: local noon today/tomorrow (a stable instant, never itself
  // ambiguous/repeated across a DST transition) is converted to the UTC
  // calendar date it falls on, then matched against each daily.time[]
  // entry -- a plain string compare against Open-Meteo's own "YYYY-MM-DD"
  // labels, not a day-count computation, so it can't drift out of sync
  // with whatever daily.time[] actually contains.
  JsonArray dailyTime    = doc["daily"]["time"];
  JsonArray dailyTempMax = doc["daily"]["temperature_2m_max"];
  JsonArray dailyTempMin = doc["daily"]["temperature_2m_min"];
  JsonArray dailyRain    = doc["daily"]["precipitation_sum"];
  if (!dailyTime.isNull()) {
    struct tm noonLocal = todayLocalTm; // today's own y/m/d, already known
    noonLocal.tm_hour = 12;
    noonLocal.tm_min  = 0;
    noonLocal.tm_sec  = 0;
    noonLocal.tm_isdst = -1; // let mktime() work out DST for this date
    time_t noonTodayEpoch = mktime(&noonLocal);

    // Buffers sized a few bytes past the exact "YYYY-MM-DD\0" (11) need,
    // and tm_year+1900 masked to a 4-digit year, purely to satisfy GCC's
    // -Wformat-truncation -- it can't know tm_year is a real calendar
    // year, not an arbitrary int, so it assumes a worst case that would
    // need far more than 4 digits. Neither changes the real output: any
    // actual RTC/NTP-derived date is always 4 digits.
    char todayUtcDate[16], tomorrowUtcDate[16];
    time_t t = noonTodayEpoch;
    struct tm utcTm;
    gmtime_r(&t, &utcTm);
    snprintf(todayUtcDate, sizeof(todayUtcDate), "%04d-%02d-%02d",
             (utcTm.tm_year + 1900) % 10000, utcTm.tm_mon + 1, utcTm.tm_mday);
    t = noonTodayEpoch + 86400;
    gmtime_r(&t, &utcTm);
    snprintf(tomorrowUtcDate, sizeof(tomorrowUtcDate), "%04d-%02d-%02d",
             (utcTm.tm_year + 1900) % 10000, utcTm.tm_mon + 1, utcTm.tm_mday);

    int todayIdx = -1, tomorrowIdx = -1;
    int n = 0;
    for (JsonVariant d : dailyTime) {
      const char *s = d.as<const char *>();
      if (s) {
        if (strcmp(s, todayUtcDate) == 0) {
          todayIdx = n;
        } else if (strcmp(s, tomorrowUtcDate) == 0) {
          tomorrowIdx = n;
        }
      }
      n++;
    }
    if (todayIdx >= 0) {
      readDailyWeather(dailyTempMax, dailyTempMin, dailyRain, todayIdx, todayWeather);
    }
    if (tomorrowIdx >= 0) {
      readDailyWeather(dailyTempMax, dailyTempMin, dailyRain, tomorrowIdx, tomorrowWeather);
    }
    if (debugPtr && (todayIdx < 0 || tomorrowIdx < 0)) {
      debugPtr->print(F("SolarForecastFetch: daily.time[] had no match for "));
      if (todayIdx < 0) {
        debugPtr->print(todayUtcDate);
        debugPtr->print(F(" (today) "));
      }
      if (tomorrowIdx < 0) {
        debugPtr->print(tomorrowUtcDate);
        debugPtr->print(F(" (tomorrow) "));
      }
      debugPtr->println();
    }
  }

  if (debugPtr) {
    debugPtr->print(F("SolarForecastFetch: weather today="));
    debugPtr->print(todayWeather.received ? F("OK") : F("--"));
    debugPtr->print(F(" tomorrow="));
    debugPtr->println(tomorrowWeather.received ? F("OK") : F("--"));
  }

  return true;
}
// ================= end of SolarForecastFetch.cpp  file
