// CarChargerFetch.cpp  file  =================
#include "CarChargerFetch.h"
#include "SiteSettings.h" // siteCarChargerIp()
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <pfodDebugPtr.h>
#include <string.h> // strcmp()
#include <stdio.h> // sscanf()

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const uint32_t RESPONSE_TIMEOUT_MS = 5000;

// Parses the charger's "HH:mm" time-of-day strings (v2h_time_on/
// v2h_time_off) into minutes since midnight. Returns 0 (midnight) on any
// malformed input -- matches this field's own "0 == disabled/unset"
// default in EV_Charger_V2H_switch/switchSettings.cpp.
static int parseHHmmToMinutes(const char *hhmm) {
  int hh = 0, mm = 0;
  if (sscanf(hhmm, "%d:%d", &hh, &mm) != 2) {
    return 0;
  }
  return hh * 60 + mm;
}

bool fetchCarChargerState(CarChargerState &state) {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(RESPONSE_TIMEOUT_MS);
  String uri = String("http://") + siteCarChargerIp() + "/api/state";
  if (!http.begin(client, uri)) {
    if (debugPtr) {
      debugPtr->println(F("CarChargerFetch: http.begin() failed"));
    }
    return false;
  }
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    if (debugPtr) {
      debugPtr->print(F("CarChargerFetch: GET /api/state failed, httpCode="));
      debugPtr->print(httpCode);
      debugPtr->print(F(" ("));
      debugPtr->print(HTTPClient::errorToString(httpCode));
      debugPtr->println(F(")"));
    }
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    if (debugPtr) {
      debugPtr->print(F("CarChargerFetch: JSON parse failed: "));
      debugPtr->println(err.c_str());
    }
    return false;
  }

  // charger_state/ac240v_status/v2h_state feed straight into
  // LoadControlTask.cpp's car-charger/V2H start-stop-toggle decisions --
  // "| """" would silently accept a malformed/empty-but-valid-JSON
  // response (e.g. "{}") as a confident "charger off, 240VAC inactive,
  // V2H off" reading, with the whole fetch still reporting success.
  // Require the three core status strings to actually be present before
  // trusting any of them; a response missing even one is treated as
  // unreachable (matching carReachable=false's existing "ESP32 error:
  // unreachable -- take no action" handling), not "definitely off".
  // v2h_auto_active/v2h_time_on/v2h_time_off are deliberately NOT checked
  // here: v2h_auto_active missing already fails safe to "false" (matches
  // nearV2hWindow()'s own established fail-safe direction -- "schedule not
  // in use, nothing to block/confirm against"), and v2h_time_on/
  // v2h_time_off already have their own documented "0 == disabled/unset"
  // convention (parseHHmmToMinutes()'s own comment).
  if (doc["charger_state"].isNull() || doc["ac240v_status"].isNull() || doc["v2h_state"].isNull()) {
    if (debugPtr) {
      debugPtr->println(F("CarChargerFetch: /api/state response missing charger_state/ac240v_status/v2h_state -- treating as failed"));
    }
    return false;
  }
  const char *chargerState = doc["charger_state"] | "";
  const char *ac240vStatus = doc["ac240v_status"] | "";
  const char *v2hState = doc["v2h_state"] | "";
  const char *v2hAutoActiveStr = doc["v2h_auto_active"] | "";
  const char *v2hTimeOn = doc["v2h_time_on"] | "";
  const char *v2hTimeOff = doc["v2h_time_off"] | "";
  state.chargerOn = (strcmp(chargerState, "on") == 0);
  state.ac240vActive = (strcmp(ac240vStatus, "active") == 0);
  state.v2hActive = (strcmp(v2hState, "on") == 0);
  state.v2hAutoActive = (strcmp(v2hAutoActiveStr, "active") == 0);
  state.v2hOnTimeMinutes = parseHHmmToMinutes(v2hTimeOn);
  state.v2hOffTimeMinutes = parseHHmmToMinutes(v2hTimeOff);

  if (debugPtr) {
    debugPtr->print(F("CarChargerFetch: charger="));
    debugPtr->print(state.chargerOn ? F("on") : F("off"));
    debugPtr->print(F(" 240vac="));
    debugPtr->print(state.ac240vActive ? F("active") : F("inactive"));
    debugPtr->print(F(" v2h="));
    debugPtr->print(state.v2hActive ? F("on") : F("off"));
    debugPtr->print(F(" v2hAuto="));
    debugPtr->print(state.v2hAutoActive ? F("active") : F("inactive"));
    debugPtr->print(F(" ("));
    debugPtr->print(v2hTimeOn);
    debugPtr->print(F("-"));
    debugPtr->print(v2hTimeOff);
    debugPtr->println(F(")"));
  }
  return true;
}

bool toggleCarCharger() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(RESPONSE_TIMEOUT_MS);
  String uri = String("http://") + siteCarChargerIp() + "/chargerPower";
  if (!http.begin(client, uri)) {
    if (debugPtr) {
      debugPtr->println(F("CarChargerFetch: http.begin() failed (toggle)"));
    }
    return false;
  }
  // Empty body, matching dailyPrediction/plot_today_solar.py's own
  // _car_toggle() (urllib.request.Request(..., data=b"", method="POST")).
  int httpCode = http.POST("");
  http.end();
  if (httpCode != HTTP_CODE_OK) {
    if (debugPtr) {
      debugPtr->print(F("CarChargerFetch: POST /chargerPower failed, httpCode="));
      debugPtr->println(httpCode);
    }
    return false;
  }
  if (debugPtr) {
    debugPtr->println(F("CarChargerFetch: /chargerPower toggled"));
  }
  return true;
}

bool toggleV2h() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(RESPONSE_TIMEOUT_MS);
  String uri = String("http://") + siteCarChargerIp() + "/v2hPower";
  if (!http.begin(client, uri)) {
    if (debugPtr) {
      debugPtr->println(F("CarChargerFetch: http.begin() failed (v2h toggle)"));
    }
    return false;
  }
  // Empty body, matching toggleCarCharger()'s own convention above.
  int httpCode = http.POST("");
  http.end();
  if (httpCode != HTTP_CODE_OK) {
    if (debugPtr) {
      debugPtr->print(F("CarChargerFetch: POST /v2hPower failed, httpCode="));
      debugPtr->println(httpCode);
    }
    return false;
  }
  if (debugPtr) {
    debugPtr->println(F("CarChargerFetch: /v2hPower toggled"));
  }
  return true;
}
// ================= end of CarChargerFetch.cpp  file
