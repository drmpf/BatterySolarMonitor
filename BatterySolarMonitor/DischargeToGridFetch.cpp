// DischargeToGridFetch.cpp  file  =================
#include "DischargeToGridFetch.h"
#include "EnphaseAuth.h" // enlightenSession(), enlightenSystemId()
#include "EnlightenLoginFetch.h" // enlightenHostIP(), notifyAuthTokenRejected()
#include "BatteryConstants.h" // BATT_BACKUP_SOC -- the "limit" field can never validly be below this
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <pfodDebugPtr.h>
#include <string.h> // strcmp()
#include <esp_system.h> // esp_random() -- randomRequestId() below

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const char *HOST = "enlighten.enphaseenergy.com";

// Same timeout bounds, same reasoning, as EnlightenLoginFetch.cpp's own
// HANDSHAKE_TIMEOUT_S/RESPONSE_TIMEOUT_MS.
static const unsigned long HANDSHAKE_TIMEOUT_S = 5;
static const uint32_t RESPONSE_TIMEOUT_MS = 8000;

// The Enlighten account's own numeric user id, as sent by the browser
// (Username header, and the userId= query parameter) -- not available from
// site_settings.csv (only enlighten_system_id -- the SITE id -- is), so
// hardcoded here, same as test/enphase_dtg_toggle.py's own USER_ID.
static const char *USER_ID = "3823359";

// Matches test/enphase_dtg_toggle.py's own USER_AGENT exactly -- the real
// browser's HAR captures use the same shape (a plain desktop Chrome UA).
static const char *USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";

static Print *dbg() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  return debugPtr;
}

// Parses a 24h "HH:MM" schedule time (as Enphase returns it) to minutes-
// since-midnight. Returns -1 if it doesn't contain a ':' -- never invents
// a time it couldn't actually parse.
static int parseHHMMToMinutes(const String &hhmm) {
  int colon = hhmm.indexOf(':');
  if (colon < 0) {
    return -1;
  }
  int h = hhmm.substring(0, colon).toInt();
  int m = hhmm.substring(colon + 1).toInt();
  return h * 60 + m;
}

// Builds a v4-shaped UUID for the "Requestid" header -- the browser sends
// a fresh one on every request (confirmed from the HAR captures) and
// test/enphase_dtg_toggle.py generates one with Python's uuid.uuid4().
// Enphase almost certainly only uses it for server-side request tracing,
// not validation, but matching the shape costs nothing and removes one
// more difference from the two known-working references.
static String randomRequestId() {
  uint8_t b[16];
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = esp_random();
    b[i] = r & 0xFF;
    b[i + 1] = (r >> 8) & 0xFF;
    b[i + 2] = (r >> 16) & 0xFF;
    b[i + 3] = (r >> 24) & 0xFF;
  }
  b[6] = (b[6] & 0x0F) | 0x40; // version 4
  b[8] = (b[8] & 0x3F) | 0x80; // variant 10xx
  char buf[37];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
           b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return String(buf);
}

String formatMinutesOfDay(int minutesSinceMidnight) {
  if (minutesSinceMidnight < 0) {
    return "?";
  }
  int h = (minutesSinceMidnight / 60) % 24;
  int m = minutesSinceMidnight % 60;
  int h12 = h % 12;
  if (h12 == 0) {
    h12 = 12;
  }
  return String(h12) + ":" + (m < 10 ? "0" : "") + String(m) + (h < 12 ? "am" : "pm");
}

// Sends one request to the batteryConfig API and returns its raw JSON
// body. Shared by every call this file makes (GET schedules/
// batterySettings, POST isValid, PUT batterySettings) -- the host and
// auth headers are identical across all four; only the method/path/body/
// X-XSRF-Token differ.
//
// xsrfToken: pass "" for a GET (the browser never sends one on a GET
// either). On return, updated to whatever fresh X-Csrf-Token the response
// carried (if any) -- isValid/PUT both return a new one, which the next
// call in the same chain must send; see setDischargeToGridEnabled() below.
//
// Returns true only on a 200 response; on a confirmed 401 also calls
// notifyAuthTokenRejected() (EnlightenLoginFetch.h), same as every other
// Enlighten-cloud caller in this project.
static bool sendBatteryConfigRequest(const char *method, const String &path,
                                      const char *jsonBody, String &xsrfToken,
                                      String &bodyOut) {
  dbg();
  IPAddress ip = enlightenHostIP();
  if (ip == IPAddress((uint32_t)0)) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: host not resolved yet"));
    }
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // no cert validation -- see SolarForecastFetch.h for this project's own reasoning
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S);
  const uint16_t port = 443;
  if (!client.connect(ip, port, HOST, NULL, NULL, NULL)) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: TLS connect failed"));
    }
    return false;
  }
  client.setTimeout(RESPONSE_TIMEOUT_MS);

  HTTPClient http;
  http.setTimeout(RESPONSE_TIMEOUT_MS);
  if (!http.begin(client, HOST, port, path, true)) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: http.begin() failed"));
    }
    return false;
  }
  const char *headerKeys[] = {"X-Csrf-Token"};
  http.collectHeaders(headerKeys, 1);
  http.addHeader("Accept", "application/json");
  // Root cause of the persistent 403, found by inspecting a real (working)
  // run's cookie jar directly (none of Origin/Referer/User-Agent/Username/
  // Requestid, added in two earlier guesses, were it): the GET to
  // .../schedules ALSO sets a `BP-XSRF-Token` cookie, carrying the exact
  // same value as the X-Csrf-Token response header/X-XSRF-Token request
  // header -- a double-submit CSRF check that compares the header against
  // THIS cookie, not a server-side session store. xsrfToken already holds
  // that value by the time it's non-empty, so it's mirrored into the
  // Cookie header here -- no separate Set-Cookie parsing needed.
  String cookie = String("_enlighten_4_session=") + enlightenSession();
  if (xsrfToken.length() > 0) {
    cookie += "; BP-XSRF-Token=" + xsrfToken;
  }
  http.addHeader("Cookie", cookie);
  http.addHeader("e-auth-token", enlightenSession());
  // Origin/Referer/User-Agent/Username/Requestid: not the actual fix (see
  // above), but matches the real browser's HAR captures and
  // test/enphase_dtg_toggle.py's own headers regardless, so left in place.
  http.addHeader("Origin", "https://battery-profile-ui.enphaseenergy.com");
  http.addHeader("Referer", "https://battery-profile-ui.enphaseenergy.com/");
  http.addHeader("User-Agent", USER_AGENT);
  http.addHeader("Username", USER_ID);
  http.addHeader("Requestid", randomRequestId());
  if (xsrfToken.length() > 0) {
    http.addHeader("X-XSRF-Token", xsrfToken);
  }
  if (jsonBody != NULL) {
    http.addHeader("Content-Type", "application/json");
  }

  int httpCode;
  if (strcmp(method, "GET") == 0) {
    httpCode = http.GET();
  } else if (strcmp(method, "POST") == 0) {
    httpCode = http.POST(jsonBody != NULL ? jsonBody : "");
  } else {
    httpCode = http.PUT(jsonBody != NULL ? jsonBody : "");
  }
  bodyOut = http.getString();
  String newToken = http.header("X-Csrf-Token");
  http.end();

  if (debugPtr) {
    debugPtr->print(F("DischargeToGridFetch: "));
    debugPtr->print(method);
    debugPtr->print(F(" "));
    debugPtr->print(path);
    debugPtr->print(F(" -> "));
    debugPtr->println(httpCode);
    // On any failure, print the server's own response body (truncated) --
    // without this, a 403/other error gives no clue WHY it was rejected.
    // Added after two rounds of guessing at missing headers for a
    // persistent 403 (streaming_2026-10-07T07-58-05-108Z.txt and
    // streaming_2026-10-07T08-03-58-041Z.txt) turned up nothing from the
    // status code alone.
    if (httpCode != HTTP_CODE_OK) {
      debugPtr->print(F("DischargeToGridFetch:   response body: "));
      debugPtr->println(bodyOut.substring(0, 300));
    }
  }

  if (httpCode == 401) {
    notifyAuthTokenRejected();
    return false;
  }
  if (newToken.length() > 0) {
    xsrfToken = newToken;
  }
  return httpCode == HTTP_CODE_OK;
}

bool fetchDischargeToGridStatus(bool &enabledOut, int &startMinutesOut, int &endMinutesOut, float &endSocOut) {
  dbg();
  String site = enlightenSystemId();
  String xsrf; // GETs don't need one
  String body;

  String schedulesPath = String("/service/batteryConfig/api/v1/battery/sites/") + site + "/schedules";
  if (!sendBatteryConfigRequest("GET", schedulesPath, NULL, xsrf, body)) {
    return false;
  }
  JsonDocument scheduleDoc;
  if (deserializeJson(scheduleDoc, body)) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: schedules JSON parse failed"));
    }
    return false;
  }
  JsonArray details = scheduleDoc["dtg"]["details"];
  if (details.isNull() || details.size() == 0) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: no DTG schedule configured"));
    }
    return false; // nothing to show -- never guess a window
  }
  const char *startRaw = details[0]["startTime"];
  const char *endRaw = details[0]["endTime"];
  if (!startRaw || !endRaw) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: DTG schedule had no startTime/endTime"));
    }
    return false;
  }
  int startMinutes = parseHHMMToMinutes(String(startRaw));
  int endMinutes = parseHHMMToMinutes(String(endRaw));
  if (startMinutes < 0 || endMinutes < 0) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: DTG schedule startTime/endTime didn't parse"));
    }
    return false;
  }
  // "limit" is the DTG schedule's own end-of-window SOC% (the same %
  // its own edit page on enlighten.enphaseenergy.com shows/lets you
  // set) -- validated here, not just read, since it's boundary data from
  // Enphase: missing, not actually a number, or outside the range the
  // setting can validly hold (never below BATT_BACKUP_SOC -- the
  // battery's own backup reserve, which Enphase itself enforces as the
  // schedule's own minimum -- and never above 100%) all fail the whole
  // fetch rather than pass through a bogus value for
  // BatteryProjection.cpp to project down to.
  JsonVariant limitVar = details[0]["limit"];
  if (limitVar.isNull() || !limitVar.is<float>()) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: DTG schedule had no numeric limit (end SOC%)"));
    }
    return false;
  }
  float endSoc = limitVar.as<float>();
  if (endSoc < BATT_BACKUP_SOC || endSoc > 100.0) {
    if (debugPtr) {
      debugPtr->print(F("DischargeToGridFetch: DTG schedule limit "));
      debugPtr->print(endSoc, 1);
      debugPtr->println(F("% is outside the valid range -- rejecting"));
    }
    return false;
  }

  xsrf = ""; // this GET doesn't need the schedules GET's own token -- see comment above
  String settingsPath = String("/service/batteryConfig/api/v1/batterySettings/") + site
                       + "?source=enho&userId=" + USER_ID;
  if (!sendBatteryConfigRequest("GET", settingsPath, NULL, xsrf, body)) {
    return false;
  }
  JsonDocument settingsDoc;
  if (deserializeJson(settingsDoc, body)) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: batterySettings JSON parse failed"));
    }
    return false;
  }
  // Checked for type, not just presence -- same reasoning as "limit"
  // above. A wrong-type value (e.g. a string) would otherwise silently
  // convert to `false` via ArduinoJson's own default-on-mismatch
  // behaviour, which reads as "Enphase confirms discharge off" -- the
  // ONE specific wrong answer here that would be acted on (skipping the
  // force-latch-to-enabled rule, DischargeToGridTask.cpp's resend check,
  // and BatteryProjection.cpp's drop-to-limit% override, all of which key
  // off this flag) rather than just failing visibly.
  JsonVariant enabledVar = settingsDoc["data"]["dtgControl"]["enabled"];
  if (enabledVar.isNull() || !enabledVar.is<bool>()) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: batterySettings had no boolean dtgControl.enabled"));
    }
    return false;
  }

  enabledOut = enabledVar.as<bool>();
  startMinutesOut = startMinutes;
  endMinutesOut = endMinutes;
  endSocOut = endSoc;
  return true;
}

bool setDischargeToGridEnabled(bool enabled) {
  dbg();
  String site = enlightenSystemId();
  String xsrf, body;

  // Step 1: GET schedules purely to obtain a fresh X-Csrf-Token -- same
  // first step the browser's own page does before either isValid or PUT.
  String schedulesPath = String("/service/batteryConfig/api/v1/battery/sites/") + site + "/schedules";
  if (!sendBatteryConfigRequest("GET", schedulesPath, NULL, xsrf, body) || xsrf.length() == 0) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: setDischargeToGridEnabled -- could not get a CSRF token"));
    }
    return false;
  }

  // Step 2: POST isValid -- the browser always checks this before PUTting
  // a dtg setting change.
  String isValidPath = String("/service/batteryConfig/api/v1/battery/sites/") + site + "/schedules/isValid";
  if (!sendBatteryConfigRequest("POST", isValidPath, "{\"scheduleType\":\"dtg\"}", xsrf, body)) {
    return false;
  }
  JsonDocument isValidDoc;
  if (deserializeJson(isValidDoc, body)) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: setDischargeToGridEnabled -- isValid response JSON parse failed"));
    }
    return false;
  }
  bool isValid = isValidDoc["isValid"]; // missing/wrong-type reads as false -- fails below, never treated as valid
  if (!isValid) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: setDischargeToGridEnabled -- isValid rejected the request"));
    }
    return false;
  }
  if (xsrf.length() == 0) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: setDischargeToGridEnabled -- isValid returned no fresh CSRF token"));
    }
    return false;
  }

  // Step 3: PUT the actual setting.
  String putBody = String("{\"dtgControl\":{\"enabled\":") + (enabled ? "true" : "false") + "}}";
  String settingsPath = String("/service/batteryConfig/api/v1/batterySettings/") + site
                       + "?userId=" + USER_ID + "&source=enho";
  if (!sendBatteryConfigRequest("PUT", settingsPath, putBody.c_str(), xsrf, body)) {
    return false;
  }
  JsonDocument putDoc;
  if (deserializeJson(putDoc, body)) {
    if (debugPtr) {
      debugPtr->println(F("DischargeToGridFetch: setDischargeToGridEnabled -- PUT response JSON parse failed"));
    }
    return false;
  }
  const char *message = putDoc["message"];
  return message != NULL && strcmp(message, "success") == 0;
}
// ================= end of DischargeToGridFetch.cpp  file
