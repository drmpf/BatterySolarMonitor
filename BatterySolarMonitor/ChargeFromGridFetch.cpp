// ChargeFromGridFetch.cpp  file  =================
#include "ChargeFromGridFetch.h"
#include "EnphaseAuth.h" // enlightenSession(), enlightenSystemId()
#include "EnlightenLoginFetch.h" // enlightenHostIP(), notifyAuthTokenRejected()
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

// Same timeout bounds, same reasoning, as DischargeToGridFetch.cpp's own
// HANDSHAKE_TIMEOUT_S/RESPONSE_TIMEOUT_MS.
static const unsigned long HANDSHAKE_TIMEOUT_S = 5;
static const uint32_t RESPONSE_TIMEOUT_MS = 8000;

// Same account identifiers as DischargeToGridFetch.cpp's own USER_ID/
// USER_AGENT -- duplicated here rather than shared, same reasoning as that
// file's own copy of this comment: not available from site_settings.csv,
// so hardcoded, matching test/enphase_cfg_toggle.py's own USER_ID.
static const char *USER_ID = "3823359";
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
// a time it couldn't actually parse. Same as DischargeToGridFetch.cpp's
// own parseHHMMToMinutes().
static int parseHHMMToMinutes(const String &hhmm) {
  int colon = hhmm.indexOf(':');
  if (colon < 0) {
    return -1;
  }
  int h = hhmm.substring(0, colon).toInt();
  int m = hhmm.substring(colon + 1).toInt();
  return h * 60 + m;
}

// Builds a v4-shaped UUID for the "Requestid" header -- same as
// DischargeToGridFetch.cpp's own randomRequestId(); see that file's own
// comment for why.
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

// Sends one request to the batteryConfig API -- same shared shape as
// DischargeToGridFetch.cpp's own sendBatteryConfigRequest() (not reused
// directly across files: duplicated here rather than factoring out a
// shared helper, per this project's own "don't refactor working code"
// rule -- this is a straight copy of that already-proven implementation).
// See that file's own comment for the full header/cookie reasoning (the
// double-submit CSRF check via the BP-XSRF-Token cookie in particular).
static bool sendBatteryConfigRequest(const char *method, const String &path,
                                      const char *jsonBody, String &xsrfToken,
                                      String &bodyOut) {
  dbg();
  IPAddress ip = enlightenHostIP();
  if (ip == IPAddress((uint32_t)0)) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: host not resolved yet"));
    }
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure(); // no cert validation -- see SolarForecastFetch.h for this project's own reasoning
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S);
  const uint16_t port = 443;
  if (!client.connect(ip, port, HOST, NULL, NULL, NULL)) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: TLS connect failed"));
    }
    return false;
  }
  client.setTimeout(RESPONSE_TIMEOUT_MS);

  HTTPClient http;
  http.setTimeout(RESPONSE_TIMEOUT_MS);
  if (!http.begin(client, HOST, port, path, true)) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: http.begin() failed"));
    }
    return false;
  }
  const char *headerKeys[] = {"X-Csrf-Token"};
  http.collectHeaders(headerKeys, 1);
  http.addHeader("Accept", "application/json");
  String cookie = String("_enlighten_4_session=") + enlightenSession();
  if (xsrfToken.length() > 0) {
    cookie += "; BP-XSRF-Token=" + xsrfToken;
  }
  http.addHeader("Cookie", cookie);
  http.addHeader("e-auth-token", enlightenSession());
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
    debugPtr->print(F("ChargeFromGridFetch: "));
    debugPtr->print(method);
    debugPtr->print(F(" "));
    debugPtr->print(path);
    debugPtr->print(F(" -> "));
    debugPtr->println(httpCode);
    if (httpCode != HTTP_CODE_OK) {
      debugPtr->print(F("ChargeFromGridFetch:   response body: "));
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

bool fetchChargeFromGridStatus(bool &enabledOut, int &startMinutesOut, int &endMinutesOut, float &targetSocOut) {
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
      debugPtr->println(F("ChargeFromGridFetch: schedules JSON parse failed"));
    }
    return false;
  }
  JsonArray details = scheduleDoc["cfg"]["details"];
  if (details.isNull() || details.size() == 0) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: no cfg schedule configured"));
    }
    return false; // nothing to show -- never guess a window
  }
  const char *startRaw = details[0]["startTime"];
  const char *endRaw = details[0]["endTime"];
  if (!startRaw || !endRaw) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: cfg schedule had no startTime/endTime"));
    }
    return false;
  }
  int startMinutes = parseHHMMToMinutes(String(startRaw));
  int endMinutes = parseHHMMToMinutes(String(endRaw));
  if (startMinutes < 0 || endMinutes < 0) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: cfg schedule startTime/endTime didn't parse"));
    }
    return false;
  }
  // "limit" is the cfg schedule's own target SOC% -- the % the battery is
  // charged UP TO by the window's end (the same % Enphase's own
  // schedule-edit page shows/lets you set), the mirror-image of
  // discharge-to-grid's "limit" (charged DOWN to). Validated here, not
  // just read, since it's boundary data from Enphase -- missing, not
  // actually a number, or outside the range this setting can validly hold
  // (0-100%) fails the whole fetch rather than pass through a bogus value
  // for BatteryProjection.cpp to project up to.
  JsonVariant limitVar = details[0]["limit"];
  if (limitVar.isNull() || !limitVar.is<float>()) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: cfg schedule had no numeric limit (target SOC%)"));
    }
    return false;
  }
  float targetSoc = limitVar.as<float>();
  if (targetSoc <= 0.0 || targetSoc > 100.0) {
    if (debugPtr) {
      debugPtr->print(F("ChargeFromGridFetch: cfg schedule limit "));
      debugPtr->print(targetSoc, 1);
      debugPtr->println(F("% is outside the valid range -- rejecting"));
    }
    return false;
  }

  xsrf = ""; // this GET doesn't need the schedules GET's own token
  String settingsPath = String("/service/batteryConfig/api/v1/batterySettings/") + site
                       + "?source=enho&userId=" + USER_ID;
  if (!sendBatteryConfigRequest("GET", settingsPath, NULL, xsrf, body)) {
    return false;
  }
  JsonDocument settingsDoc;
  if (deserializeJson(settingsDoc, body)) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: batterySettings JSON parse failed"));
    }
    return false;
  }
  // Checked for type, not just presence -- same reasoning as
  // DischargeToGridFetch.cpp's own dtgControl.enabled check: a wrong-type
  // value would otherwise silently convert to `false` via ArduinoJson's
  // own default-on-mismatch behaviour, which reads as "Enphase confirms
  // charge-from-grid off" rather than failing visibly.
  JsonVariant enabledVar = settingsDoc["data"]["chargeFromGrid"];
  if (enabledVar.isNull() || !enabledVar.is<bool>()) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: batterySettings had no boolean chargeFromGrid"));
    }
    return false;
  }

  enabledOut = enabledVar.as<bool>();
  startMinutesOut = startMinutes;
  endMinutesOut = endMinutes;
  targetSocOut = targetSoc;
  return true;
}

bool setChargeFromGridEnabled(bool enabled) {
  dbg();
  String site = enlightenSystemId();
  String xsrf, body;

  // Step 1: GET batterySettings for a fresh X-Csrf-Token AND the current
  // cfgControl.forceScheduleOpted flag -- unlike discharge-to-grid (which
  // only needs the token, from GET schedules), the cfg isValid call below
  // must echo this flag back from the server's own current state, per the
  // HAR capture -- never assumed/hardcoded (this project's own
  // no-silent-default rule: boundary data either validates or the whole
  // action fails, it's never guessed).
  String settingsPath = String("/service/batteryConfig/api/v1/batterySettings/") + site
                       + "?source=enho&userId=" + USER_ID;
  if (!sendBatteryConfigRequest("GET", settingsPath, NULL, xsrf, body) || xsrf.length() == 0) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: setChargeFromGridEnabled -- could not get a CSRF token"));
    }
    return false;
  }
  JsonDocument settingsDoc;
  if (deserializeJson(settingsDoc, body)) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: setChargeFromGridEnabled -- batterySettings JSON parse failed"));
    }
    return false;
  }
  JsonVariant forceOptedVar = settingsDoc["data"]["cfgControl"]["forceScheduleOpted"];
  if (forceOptedVar.isNull() || !forceOptedVar.is<bool>()) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: setChargeFromGridEnabled -- batterySettings had no boolean cfgControl.forceScheduleOpted"));
    }
    return false;
  }
  bool forceScheduleOpted = forceOptedVar.as<bool>();

  // Step 2: POST isValid -- the browser always checks this before PUTting
  // a cfg setting change.
  String isValidPath = String("/service/batteryConfig/api/v1/battery/sites/") + site + "/schedules/isValid";
  String isValidBody = String("{\"scheduleType\":\"cfg\",\"forceScheduleOpted\":")
                      + (forceScheduleOpted ? "true" : "false") + "}";
  if (!sendBatteryConfigRequest("POST", isValidPath, isValidBody.c_str(), xsrf, body)) {
    return false;
  }
  JsonDocument isValidDoc;
  if (deserializeJson(isValidDoc, body)) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: setChargeFromGridEnabled -- isValid response JSON parse failed"));
    }
    return false;
  }
  bool isValid = isValidDoc["isValid"]; // missing/wrong-type reads as false -- fails below, never treated as valid
  if (!isValid) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: setChargeFromGridEnabled -- isValid rejected the request"));
    }
    return false;
  }
  if (xsrf.length() == 0) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: setChargeFromGridEnabled -- isValid returned no fresh CSRF token"));
    }
    return false;
  }

  // Step 3: PUT the actual setting -- a TOP-LEVEL "chargeFromGrid" field,
  // not nested under a "cfgControl" object (confirmed from the HAR
  // capture -- see this file's own header comment).
  String putBody = String("{\"chargeFromGrid\":") + (enabled ? "true" : "false") + "}";
  if (!sendBatteryConfigRequest("PUT", settingsPath, putBody.c_str(), xsrf, body)) {
    return false;
  }
  JsonDocument putDoc;
  if (deserializeJson(putDoc, body)) {
    if (debugPtr) {
      debugPtr->println(F("ChargeFromGridFetch: setChargeFromGridEnabled -- PUT response JSON parse failed"));
    }
    return false;
  }
  const char *message = putDoc["message"];
  return message != NULL && strcmp(message, "success") == 0;
}
// ================= end of ChargeFromGridFetch.cpp  file
