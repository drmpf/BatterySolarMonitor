// EnlightenHistoryFetch.cpp  file  =================
#include "EnlightenHistoryFetch.h"
#include "EnphaseAuth.h"
#include "EnlightenLoginFetch.h" // notifyAuthTokenRejected()
#include "LocalTimeMinutes.h"
#include <Arduino.h> // millis(), SemaphoreHandle_t -- see badIPs[]'s mutex
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <pfodDebugPtr.h>
#include <lwip/netdb.h> // lwip_getaddrinfo()/lwip_freeaddrinfo() -- see resolveEnlightenHosts()

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const char *HOSTNAME = "enlighten.enphaseenergy.com";

// TCP-connect-phase timeout: NetworkClientSecure::connect(ip, port, host,
// CA, cert, key) -- the IP+host overload used below, needed to keep SNI
// correct -- doesn't take a timeout argument and always uses the
// library's fixed 30-second default (only the OTHER connect() overloads,
// which don't accept a separate SNI hostname, let a timeout be passed
// in). That 30s is not shortened here since there is no public API for
// it on this overload. What IS configurable, and was actually the cause
// of an observed 135-second hang, is the TLS HANDSHAKE-phase timeout,
// which defaults to 120 seconds (NetworkClientSecure's own default) --
// set explicitly and much shorter below via setHandshakeTimeout().
static const unsigned long HANDSHAKE_TIMEOUT_S = 5; // seconds -- setHandshakeTimeout()'s own unit
static const uint32_t RESPONSE_TIMEOUT_MS = 8000;   // response body read timeout

// enlighten.enphaseenergy.com is a CNAME to an AWS Network Load Balancer
// with several backend IPs (confirmed via nslookup: 3 IPs behind
// enl-defender-enlam-prod-NLB-*.elb.us-east-1.amazonaws.com). One of them
// (98.87.141.54, at diagnosis time) was confirmed unreachable from this
// network (raw TCP connect hung >5s) while the other two connected in
// under 1.5s -- see resolveEnlightenHosts() (EnlightenHistoryFetch.h).
static const int MAX_CANDIDATE_IPS = 4;
static IPAddress candidateIPs[MAX_CANDIDATE_IPS];
static int candidateIPCount = 0;

// Guards candidateIPs[]/candidateIPCount -- written by resolveEnlightenHosts()
// (main thread) and read by httpGetFromCandidates() (background task,
// InternetPollTask.cpp -- confirmed genuinely concurrent with the main
// thread, xTaskCreatePinnedToCore()'d to core 0 while loop() runs on core
// 1). Previously unprotected: EnlightenHistoryRefresh.cpp and
// AvgConsumptionRefresh.cpp each independently call resolveEnlightenHosts()
// from their own main-thread state machines, so a fresh resolve can start
// (and its old candidateIPCount=0/repopulate sequence isn't atomic) while
// the background task is mid-iteration inside httpGetFromCandidates()'s
// own for(i<candidateIPCount) loop reading candidateIPs[i] -- confirmed
// reachable, not hypothetical: deviceLog.txt shows two independent
// resolveEnlightenHosts() calls landing 4 seconds apart while a fetch was
// still in flight. Same shared-mutable-state class of bug as
// TimestampStream.h's atLineStart/haveLock race. Created on first use, not
// at global-construction time -- see SolarForecastTask.cpp's identical
// reasoning for its own mutex.
static SemaphoreHandle_t candidateIPMutex = NULL;

static SemaphoreHandle_t candidateIPMutexHandle() {
  if (candidateIPMutex == NULL) {
    candidateIPMutex = xSemaphoreCreateMutex();
  }
  return candidateIPMutex;
}

// Remembers IPs a connect attempt has already failed against, so
// resolveEnlightenHosts() (main thread) can skip them next time instead
// of handing fetchEnlightenHistoryToday() (background task) the same
// known-bad IP again -- observed on real hardware: the router's DNS
// cache can keep returning the SAME single answer for minutes, so
// re-resolving alone isn't enough to escape a bad one. Written from the
// background task (a connect failure) and read from the main thread (the
// next resolve), so this needs its own lock -- separate from
// EnlightenHistoryTask.h's, to keep this file self-contained.
// Entries expire after BAD_IP_FORGET_MS so a transient problem doesn't
// blacklist an IP forever.
struct BadIPEntry {
  IPAddress ip;
  unsigned long markedBadAtMs;
};
static const int MAX_BAD_IPS = 4;
static BadIPEntry badIPs[MAX_BAD_IPS];
static int badIPCount = 0;
static const unsigned long BAD_IP_FORGET_MS = 15UL * 60UL * 1000UL; // 15 minutes

static SemaphoreHandle_t badIPMutex = NULL;

static SemaphoreHandle_t badIPMutexHandle() {
  if (badIPMutex == NULL) {
    badIPMutex = xSemaphoreCreateMutex();
  }
  return badIPMutex;
}

// Returns true if 'ip' failed recently enough to still be avoided.
// Expires (and forgets) stale entries as a side effect.
static bool isKnownBadIP(const IPAddress &ip) {
  bool result = false;
  xSemaphoreTake(badIPMutexHandle(), portMAX_DELAY);
  for (int i = 0; i < badIPCount; i++) {
    if (badIPs[i].ip == ip) {
      if (millis() - badIPs[i].markedBadAtMs < BAD_IP_FORGET_MS) {
        result = true;
      } else {
        // expired -- forget it, so it can be tried again
        badIPs[i] = badIPs[badIPCount - 1];
        badIPCount--;
      }
      break;
    }
  }
  xSemaphoreGive(badIPMutexHandle());
  return result;
}

static void markIPBad(const IPAddress &ip) {
  xSemaphoreTake(badIPMutexHandle(), portMAX_DELAY);
  int idx = -1;
  for (int i = 0; i < badIPCount; i++) {
    if (badIPs[i].ip == ip) {
      idx = i;
      break;
    }
  }
  if (idx < 0) {
    if (badIPCount < MAX_BAD_IPS) {
      idx = badIPCount++;
    } else {
      // list full -- reuse the oldest entry
      idx = 0;
      for (int i = 1; i < badIPCount; i++) {
        if (badIPs[i].markedBadAtMs < badIPs[idx].markedBadAtMs) {
          idx = i;
        }
      }
    }
  }
  badIPs[idx].ip = ip;
  badIPs[idx].markedBadAtMs = millis();
  xSemaphoreGive(badIPMutexHandle());
}

// Matches the ACTUAL_COLOR already established on each chart for
// measured data -- see Dwg_SolarPlot1App.cpp, Dwg_ConsumptionPlot1App.cpp,
// Dwg_BatteryPlot1App.cpp.
static const uint8_t SOLAR_ACTUAL_COLOR = 2;        // GREEN
static const uint8_t CONSUMPTION_ACTUAL_COLOR = 12; // BLUE
static const uint8_t BATTERY_ACTUAL_COLOR = 208;    // matches the original decorative curve's colour

// Repeats the lookup up to this many times, gathering distinct IPs, since
// each individual lwip_getaddrinfo() call only ever returns one answer on
// this ESP-IDF/lwIP build (confirmed on real hardware -- unlike a full OS
// resolver such as `nslookup` uses). In principle repeated calls could
// return different IPs (the underlying round-robin rotating which answer
// comes back) -- in practice, observed on real hardware, the ROUTER's own
// DNS cache/relay can stay stuck returning the exact same single answer
// for many minutes regardless of how many times this is queried, which
// FALLBACK_IPS below exists to work around. Stops early once
// MAX_CANDIDATE_IPS distinct IPs are found. All main-thread-only, cheap
// (local-network DNS lookups).
static const int MAX_RESOLVE_ATTEMPTS = 8;

// Last-resort fallback when repeated DNS lookups keep returning only
// known-bad IPs (see MAX_RESOLVE_ATTEMPTS comment -- a sticky router DNS
// cache can defeat re-resolving entirely). These are the 3 backend IPs
// behind enlighten.enphaseenergy.com's AWS Network Load Balancer, seen
// via nslookup and confirmed in real device logs. NLB backend IPs are
// fairly stable (unlike ALB/CloudFront, which rotate more), but could
// still change in principle -- normal DNS resolution is always tried
// first and preferred; this list is only consulted when that resolution
// produces nothing but IPs already known bad.
static IPAddress FALLBACK_IPS[] = {
  IPAddress(98, 87, 141, 54),
  IPAddress(54, 198, 201, 60),
  IPAddress(52, 5, 24, 121),
};
static const int FALLBACK_IP_COUNT = sizeof(FALLBACK_IPS) / sizeof(FALLBACK_IPS[0]);

bool resolveEnlightenHosts() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  // Built up entirely in LOCAL arrays (goodIPs/badFallbackIPs below) --
  // the shared candidateIPs[]/candidateIPCount are only touched once, at
  // the very end under candidateIPMutexHandle(), so a background-task
  // fetch reading them never sees a mid-rebuild/inconsistent state -- see
  // candidateIPMutex's own comment.

  // Known-bad IPs (isKnownBadIP()) go to the back of the list instead of
  // being dropped entirely -- if every resolve keeps handing back the
  // same bad answer (observed on real hardware), fetchEnlightenHistoryToday()
  // still has *something* to try rather than nothing.
  IPAddress goodIPs[MAX_CANDIDATE_IPS];
  int goodCount = 0;
  IPAddress badFallbackIPs[MAX_CANDIDATE_IPS];
  int badFallbackCount = 0;

  for (int attempt = 0; attempt < MAX_RESOLVE_ATTEMPTS && goodCount < MAX_CANDIDATE_IPS; attempt++) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // IPv4 only -- matches this project's usage elsewhere
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int err = lwip_getaddrinfo(HOSTNAME, "443", &hints, &res);
    if (err != 0 || res == NULL) {
      continue; // transient lookup failure -- try again
    }

    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
      struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
      IPAddress ip(ipv4->sin_addr.s_addr);
      bool alreadyHave = false;
      for (int i = 0; i < goodCount; i++) {
        if (goodIPs[i] == ip) {
          alreadyHave = true;
          break;
        }
      }
      for (int i = 0; !alreadyHave && i < badFallbackCount; i++) {
        if (badFallbackIPs[i] == ip) {
          alreadyHave = true;
        }
      }
      if (alreadyHave) {
        continue;
      }
      if (isKnownBadIP(ip)) {
        if (badFallbackCount < MAX_CANDIDATE_IPS) {
          badFallbackIPs[badFallbackCount++] = ip;
        }
      } else if (goodCount < MAX_CANDIDATE_IPS) {
        goodIPs[goodCount++] = ip;
      }
    }
    lwip_freeaddrinfo(res);
  }

  // DNS (possibly via a sticky router cache) produced nothing but
  // known-bad IPs -- try the hardcoded fallback list so there's a real
  // chance of a different IP instead of just repeating the bad one.
  if (goodCount == 0) {
    for (int i = 0; i < FALLBACK_IP_COUNT && goodCount < MAX_CANDIDATE_IPS; i++) {
      IPAddress ip = FALLBACK_IPS[i];
      bool alreadyHave = false;
      for (int j = 0; j < goodCount; j++) {
        if (goodIPs[j] == ip) {
          alreadyHave = true;
          break;
        }
      }
      for (int j = 0; !alreadyHave && j < badFallbackCount; j++) {
        if (badFallbackIPs[j] == ip) {
          alreadyHave = true;
        }
      }
      if (alreadyHave || isKnownBadIP(ip)) {
        continue;
      }
      goodIPs[goodCount++] = ip;
    }
  }

  xSemaphoreTake(candidateIPMutexHandle(), portMAX_DELAY);
  candidateIPCount = 0;
  for (int i = 0; i < goodCount && candidateIPCount < MAX_CANDIDATE_IPS; i++) {
    candidateIPs[candidateIPCount++] = goodIPs[i];
  }
  for (int i = 0; i < badFallbackCount && candidateIPCount < MAX_CANDIDATE_IPS; i++) {
    candidateIPs[candidateIPCount++] = badFallbackIPs[i];
  }
  int publishedCount = candidateIPCount;
  xSemaphoreGive(candidateIPMutexHandle());

  if (debugPtr) {
    // Prints from the LOCAL goodIPs/badFallbackIPs, not the shared
    // candidateIPs[] just published -- avoids a second, unprotected read
    // of shared state right after releasing the lock above.
    debugPtr->print(F("EnlightenHistoryFetch: resolved "));
    debugPtr->print(goodCount);
    debugPtr->print(F(" new + "));
    debugPtr->print(badFallbackCount);
    debugPtr->print(F(" known-bad-fallback candidate IP(s) for "));
    debugPtr->print(HOSTNAME);
    debugPtr->print(':');
    for (int i = 0; i < goodCount; i++) {
      debugPtr->print(' ');
      debugPtr->print(goodIPs[i]);
    }
    for (int i = 0; i < badFallbackCount; i++) {
      debugPtr->print(' ');
      debugPtr->print(badFallbackIPs[i]);
    }
    debugPtr->println();
  }
  return publishedCount > 0;
}

// Copies one flat watts/percent array into 'plot' as kW (divide by 1000
// when toKw is true) or as-is (battery SOC%, already a percent), one
// point per 15-min slot from midnight -- but only for slots whose whole
// 15-min window has already fully elapsed as of 'nowMinutes'.
//
// Confirmed as a real, not hypothetical, risk by comparing deviceLog.txt
// against dailyPrediction's own today_solar.log: Enlighten's
// power_time_series endpoint hands back a value for the slot "now" is
// CURRENTLY inside (i.e. still accumulating, not yet a finished 15-min
// average) -- e.g. a fetch at 12:54 returning a slot timestamped 12:45
// covers the window 12:45-13:00, which hasn't closed yet at 12:54. The
// old cutoff (minutesSinceMidnight > nowMinutes) happily accepted that
// still-forming slot as if it were a settled ACTUAL reading, which is
// what produced the stray low/incomplete-looking point right at the
// actual-to-forecast boundary on the chart. Requiring the slot's END
// (minutesSinceMidnight + STEP_MINUTES) to be <= nowMinutes excludes that
// still-open slot; GatewayRefresh.cpp's own live "now" point (real-time,
// not a still-accumulating cloud average) is what represents that time
// instead, same as dailyPrediction/plot_today_solar.py's
// fetch_consumption_today() falling back to its intraday gateway CSV to
// fill exactly this gap.
static void copyFlatSeriesToPlot(JsonArray series, PlotData &plot, uint8_t color, bool toKw, int nowMinutes) {
  int i = 0;
  for (JsonVariant v : series) {
    int minutesSinceMidnight = i * PlotData::STEP_MINUTES;
    if (minutesSinceMidnight + PlotData::STEP_MINUTES > nowMinutes) {
      break; // this slot's own 15-min window hasn't fully elapsed yet -- not settled
    }
    // A null slot here means Enlighten hasn't backfilled it yet, not a
    // confirmed zero reading -- a true zero-draw 15-min consumption window
    // is rare, so treating null as 0.0 would silently understate today's
    // actual total (and, via consumptionData.totalKwh(), the "vs average"
    // comparison Dwg_StatusApp.cpp shows) whenever Enlighten has a gap.
    // Simply not calling setPointAtTime() for it leaves this slot at
    // PlotData::clear()'s own -1 "no data yet" sentinel (set by the
    // caller just before this loop runs) -- the same sentinel totalKwh()
    // already excludes from its sum, and every chart's own segment-drawing
    // already skips, so a missing slot here is handled correctly with no
    // other code needing to change.
    if (v.isNull()) {
      i++;
      continue;
    }
    float value = v.as<float>();
    if (toKw) {
      value = value / 1000.0;
    }
    plot.setPointAtTime(minutesSinceMidnight, value, color);
    i++;
  }
}

// Connects to each candidate IP in turn (stopping at the first success)
// and GETs 'uri', returning the response body -- shared by
// fetchEnlightenHistoryToday() and fetchEnlightenConsumptionForDate().
// Returns an empty string if every candidate IP failed (including "no
// candidates yet" -- resolveEnlightenHosts() hasn't succeeded, see
// EnlightenHistoryFetch.h).
static String httpGetFromCandidates(const String &uri) {
  // Snapshot into LOCAL variables under the lock, then work from the copy
  // -- this loop can spend several seconds per candidate (TLS
  // handshake+response read), and resolveEnlightenHosts() must be free to
  // publish a fresh list on the main thread at any moment without either
  // side seeing the other's half-finished state. See candidateIPMutex's
  // own comment.
  int localCount;
  IPAddress localIPs[MAX_CANDIDATE_IPS];
  xSemaphoreTake(candidateIPMutexHandle(), portMAX_DELAY);
  localCount = candidateIPCount;
  for (int i = 0; i < localCount; i++) {
    localIPs[i] = candidateIPs[i];
  }
  xSemaphoreGive(candidateIPMutexHandle());

  if (localCount == 0) {
    if (debugPtr) {
      debugPtr->println(F("EnlightenHistoryFetch: no resolved IPs yet, skipping fetch"));
    }
    return String();
  }

  const uint16_t port = 443;
  for (int i = 0; i < localCount; i++) {
    WiFiClientSecure client;
    client.setInsecure(); // no cert validation -- see SolarForecastFetch.h for this project's own reasoning
    // NOTE: Stream::setTimeout() (what client.setTimeout() resolves to)
    // does NOT affect the connect()/handshake timeouts below -- see this
    // file's HANDSHAKE_TIMEOUT_S comment. It's set later, after connect
    // succeeds, purely for reading the response body.
    client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S); // bounds the TLS handshake phase -- see HANDSHAKE_TIMEOUT_S comment

    // Connect via this candidate IP but pass HOSTNAME through as the TLS
    // SNI/handshake hostname -- see SolarForecastFetch.cpp's
    // fetchSolarForecast() for why this specific IP+host connect()
    // overload is used instead of a plain hostname connect (which would
    // resolve DNS itself -- unsafe here, this runs on a background task).
    // The TCP-connect phase itself is bounded by the library's fixed 30s
    // default (see HANDSHAKE_TIMEOUT_S comment) -- not shortened here.
    if (!client.connect(localIPs[i], port, HOSTNAME, NULL, NULL, NULL)) {
      markIPBad(localIPs[i]); // so the next resolveEnlightenHosts() call skips it
      if (debugPtr) {
        debugPtr->print(F("EnlightenHistoryFetch: TLS connect to "));
        debugPtr->print(localIPs[i]);
        debugPtr->println(F(" failed, remembering it as bad"));
      }
      continue; // try the next candidate IP
    }
    client.setTimeout(RESPONSE_TIMEOUT_MS); // now governs reading the response body

    HTTPClient http;
    http.setTimeout(RESPONSE_TIMEOUT_MS);
    if (!http.begin(client, HOSTNAME, port, uri, true)) {
      if (debugPtr) {
        debugPtr->println(F("EnlightenHistoryFetch: http.begin() failed"));
      }
      continue;
    }
    http.addHeader("Cookie", String("_enlighten_4_session=") + enlightenSession());
    http.addHeader("e-auth-token", enlightenSession());
    http.addHeader("Accept", "application/json");
    // Matches dailyPrediction/plot_today_solar.py's fetch_consumption_today()
    // -- the Enlighten API has been observed to care about these headers
    // looking like a real browser request.
    http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    http.addHeader("X-Requested-With", "XMLHttpRequest");

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String body = http.getString(); // de-chunks -- see SolarForecastFetch.cpp
      http.end();
      return body;
    }

    if (debugPtr) {
      debugPtr->print(F("EnlightenHistoryFetch: "));
      debugPtr->print(localIPs[i]);
      debugPtr->print(F(" GET failed, httpCode="));
      debugPtr->print(httpCode);
      debugPtr->print(F(" ("));
      debugPtr->print(HTTPClient::errorToString(httpCode));
      debugPtr->println(F(")"));
    }
    if (httpCode == 401) {
      // A REAL request (not EnlightenLoginFetch.cpp's own validation
      // ping) hit a confirmed 401 -- enlightenSession() has genuinely
      // stopped working. Tell EnlightenLoginFetch.h so it re-checks (and,
      // if still actually invalid, rebuilds) on its very next tick,
      // rather than only finding out via tomorrow's reboot -- see
      // notifyAuthTokenRejected()'s own comment for why this feedback
      // path is needed at all.
      notifyAuthTokenRejected();
      if (debugPtr) {
        debugPtr->println(F("EnlightenHistoryFetch: 401 -- enlightenSession() (data/auth_token.csv) "
                             "is likely expired -- notified EnlightenLoginFetch.h to re-check"));
      }
    }
    http.end();
  }
  return String(); // every candidate IP failed
}

bool fetchEnlightenHistoryToday(PlotData &solar, PlotData &consumption, PlotData &battery) {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  char dateStr[11];
  localTodayDateString(dateStr, sizeof(dateStr));
  int nowMinutes = localTimeMinutesSinceMidnight();

  String uri = String("/systems/") + enlightenSystemId() +
               "/power_time_series?all_production_sources=true&days=1&date=" + dateStr;
  String body = httpGetFromCandidates(uri);
  if (body.length() == 0) {
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    if (debugPtr) {
      debugPtr->print(F("EnlightenHistoryFetch: JSON parse failed: "));
      debugPtr->println(err.c_str());
    }
    return false;
  }

  solar.clear();
  consumption.clear();
  battery.clear();

  JsonArray productionMeter = doc["production"]["meter"];
  JsonArray consumptionSeries = doc["consumption"];
  JsonArray batterySoc = doc["acb"]["acb_soc_ts"];

  int solarPoints = 0, consumptionPoints = 0, batteryPoints = 0;
  if (!productionMeter.isNull()) {
    copyFlatSeriesToPlot(productionMeter, solar, SOLAR_ACTUAL_COLOR, true, nowMinutes);
    solarPoints = productionMeter.size();
  }
  if (!consumptionSeries.isNull()) {
    copyFlatSeriesToPlot(consumptionSeries, consumption, CONSUMPTION_ACTUAL_COLOR, true, nowMinutes);
    consumptionPoints = consumptionSeries.size();
  }
  if (!batterySoc.isNull()) {
    copyFlatSeriesToPlot(batterySoc, battery, BATTERY_ACTUAL_COLOR, false, nowMinutes);
    batteryPoints = batterySoc.size();
  }

  if (debugPtr) {
    debugPtr->print(F("EnlightenHistoryFetch: got "));
    debugPtr->print(solarPoints);
    debugPtr->print(F(" solar, "));
    debugPtr->print(consumptionPoints);
    debugPtr->print(F(" consumption, "));
    debugPtr->print(batteryPoints);
    debugPtr->println(F(" battery-SOC slots"));
  }

  return (solarPoints > 0) || (consumptionPoints > 0) || (batteryPoints > 0);
}

bool fetchEnlightenConsumptionForDate(const char *dateStr, float dayKw[PlotData::MAX_POINTS], float *totalKwhOut) {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  String uri = String("/systems/") + enlightenSystemId() +
               "/power_time_series?all_production_sources=true&days=1&date=" + dateStr;
  String body = httpGetFromCandidates(uri);
  if (body.length() == 0) {
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    if (debugPtr) {
      debugPtr->print(F("EnlightenHistoryFetch: consumption-for-date JSON parse failed: "));
      debugPtr->println(err.c_str());
    }
    return false;
  }

  JsonArray consumptionSeries = doc["consumption"];
  if (consumptionSeries.isNull()) {
    if (debugPtr) {
      debugPtr->println(F("EnlightenHistoryFetch: consumption-for-date response had no consumption array"));
    }
    return false;
  }

  // -1.0, matching PlotData's own "no data" sentinel -- not 0.0, which
  // AvgConsumptionProfile::addDay() would otherwise blend in as a genuine
  // zero-consumption reading for this slot (see that method's own comment
  // for why that's a real, confirmed risk, not hypothetical).
  for (int i = 0; i < PlotData::MAX_POINTS; i++) {
    dayKw[i] = -1.0;
  }

  // 'dateStr' is always a fully-elapsed past day here (see
  // EnlightenHistoryFetch.h) -- no "stop at now" cutoff needed, unlike
  // copyFlatSeriesToPlot()'s use for TODAY. Maps Enphase's own
  // midnight-based 15-min slots directly onto PlotData's 5am-10pm index
  // scheme (300 is itself an exact multiple of 15, so this is an exact
  // mapping, not a rounding one).
  //
  // totalKwh sums every NON-NULL slot in the response (the full
  // midnight-to-midnight day, not just the ones landing in dayKw's
  // 5am-10pm range) -- each slot is an average kW over one STEP_MINUTES
  // window, so kWh = kW * (STEP_MINUTES/60). A null slot (Enlighten hasn't
  // backfilled it yet, not a confirmed zero -- same reasoning as
  // copyFlatSeriesToPlot()'s own comment) is skipped entirely rather than
  // counted as 0kWh, so a day with a few gaps still reports an accurate
  // (if slightly incomplete) total instead of an artificially low one.
  int i = 0;
  int points = 0;
  float totalKwh = 0.0;
  for (JsonVariant v : consumptionSeries) {
    int minutesSinceMidnight = i * PlotData::STEP_MINUTES;
    if (v.isNull()) {
      i++;
      continue;
    }
    float kw = v.as<float>() / 1000.0;
    totalKwh += kw * (PlotData::STEP_MINUTES / 60.0);
    if (minutesSinceMidnight >= PlotData::START_MINUTES && minutesSinceMidnight <= PlotData::END_MINUTES) {
      int index = (minutesSinceMidnight - PlotData::START_MINUTES) / PlotData::STEP_MINUTES;
      dayKw[index] = kw;
      points++;
    }
    i++;
  }

  if (debugPtr) {
    debugPtr->print(F("EnlightenHistoryFetch: got "));
    debugPtr->print(points);
    debugPtr->print(F(" consumption slots for "));
    debugPtr->print(dateStr);
    debugPtr->print(F(", total "));
    debugPtr->print(totalKwh, 2);
    debugPtr->println(F("kWh"));
  }
  if (points == 0) {
    return false;
  }
  if (totalKwhOut) {
    *totalKwhOut = totalKwh;
  }
  return true;
}
// ================= end of EnlightenHistoryFetch.cpp  file
