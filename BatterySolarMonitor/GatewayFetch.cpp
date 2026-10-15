// GatewayFetch.cpp  file  =================
#include "GatewayFetch.h"
#include "EnphaseAuth.h"
#include "EnlightenLoginFetch.h" // notifyAuthTokenRejected()
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <pfodDebugPtr.h>
#include <string.h> // strcmp()

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// Same timeout bounds, and the same reasoning, as
// EnlightenHistoryFetch.cpp's HANDSHAKE_TIMEOUT_S/RESPONSE_TIMEOUT_MS --
// see that file's comment: WiFiClientSecure's own default TLS handshake
// timeout is 120s, confirmed there as the cause of an observed 135s hang.
// Applies here too even though the gateway is local -- a stalled/half-open
// TCP session to it can still hang a handshake for the full 120s default.
static const unsigned long HANDSHAKE_TIMEOUT_S = 5; // seconds -- setHandshakeTimeout()'s own unit
static const uint32_t RESPONSE_TIMEOUT_MS = 8000;   // response body read timeout

// /ivp/meters' eid -> measurementType mapping is static hardware config --
// fetched once (on first successful call) and cached, rather than
// re-fetched on every poll.
static bool haveMeterIds = false;
static long productionEid = -1;
static long netConsumptionEid = -1;

static bool fetchMeterIds() {
  WiFiClientSecure client;
  client.setInsecure(); // local gateway, self-signed cert -- see GatewayFetch.h
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S); // bounds the TLS handshake phase -- see comment above
  client.setTimeout(RESPONSE_TIMEOUT_MS);          // response body read timeout
  String url = String("https://") + gatewayIp() + "/ivp/meters";

  HTTPClient http;
  http.setTimeout(RESPONSE_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    if (debugPtr) {
      debugPtr->println(F("GatewayFetch: /ivp/meters begin() failed"));
    }
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + gatewayJwt());
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    if (debugPtr) {
      debugPtr->print(F("GatewayFetch: /ivp/meters GET failed, httpCode="));
      debugPtr->print(httpCode);
      debugPtr->print(F(" ("));
      debugPtr->print(HTTPClient::errorToString(httpCode));
      debugPtr->println(F(")"));
    }
    http.end();
    if (httpCode == 401) {
      // A real request confirmed gatewayJwt() is rejected -- see
      // notifyAuthTokenRejected()'s own comment for why this feedback to
      // EnlightenLoginFetch.h is needed (it can't otherwise notice a token
      // that was fine at its own once-per-boot check but has since
      // actually expired/been revoked).
      notifyAuthTokenRejected();
    }
    return false;
  }

  String body = http.getString(); // de-chunks -- see SolarForecastFetch.cpp
  http.end();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    if (debugPtr) {
      debugPtr->print(F("GatewayFetch: /ivp/meters JSON parse failed: "));
      debugPtr->println(err.c_str());
    }
    return false;
  }

  for (JsonVariant m : doc.as<JsonArray>()) {
    const char *type = m["measurementType"] | "";
    long eid = m["eid"] | -1L;
    if (strcmp(type, "production") == 0) {
      productionEid = eid;
    } else if (strcmp(type, "net-consumption") == 0) {
      netConsumptionEid = eid;
    }
  }
  if (productionEid < 0 || netConsumptionEid < 0) {
    if (debugPtr) {
      debugPtr->println(F("GatewayFetch: /ivp/meters missing production/net-consumption eid"));
    }
    return false;
  }
  if (debugPtr) {
    debugPtr->print(F("GatewayFetch: meter eids production="));
    debugPtr->print(productionEid);
    debugPtr->print(F(" net-consumption="));
    debugPtr->println(netConsumptionEid);
  }
  haveMeterIds = true;
  return true;
}

// Fetches battery SOC (%, averaged across devices) and battery power (W,
// summed across devices, positive=discharging negative=charging -- the
// Envoy API's own convention) -- matches
// dailyPrediction/plot_today_solar.py's fetch_battery_status():
// devices:[].soc / devices:[].real_power_mw (milliwatts -- divide by
// 1000 for watts, per Python's own "power_values.append(...real_power_mw
// / 1000.0)"). Note the literal trailing colon in the JSON key
// "devices:" -- that's the Envoy API's own (unusual) field name, not a
// typo. A separate endpoint/request from solar+consumption, so kept as
// its own function -- failing here shouldn't discard an otherwise-good
// solar/consumption reading (see fetchGatewayPower()).
static bool fetchBatteryStatus(float &socPercent, float &powerW) {
  String url = String("https://") + gatewayIp() + "/ivp/ensemble/power";

  // The gateway's own embedded HTTPS server appears to briefly refuse a
  // new connection right after the previous one (fetchGatewayPower()'s
  // own /ivp/meters/readings call, immediately before this) closes --
  // confirmed via deviceLog.txt showing httpCode=-1 (connection refused)
  // on every single poll cycle, always right after that prior call
  // succeeds -- even though dailyPrediction/plot_today_solar.py's
  // fetch_battery_status() hits the exact same 3 endpoints in the exact
  // same order successfully every time (its own per-call Python/JSON
  // overhead likely just happens to leave the gateway more time to finish
  // tearing down the previous connection than this tight ESP32 loop
  // does). A short retry after a brief pause reliably gets past it.
  const int MAX_ATTEMPTS = 3;
  const unsigned long RETRY_DELAY_MS = 300;
  int httpCode = -1;
  String body;
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    WiFiClientSecure client;
    client.setInsecure(); // local gateway, self-signed cert -- see GatewayFetch.h
    client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S); // bounds the TLS handshake phase -- see comment above
    client.setTimeout(RESPONSE_TIMEOUT_MS);          // response body read timeout

    HTTPClient http;
    http.setTimeout(RESPONSE_TIMEOUT_MS);
    if (!http.begin(client, url)) {
      if (debugPtr) {
        debugPtr->print(F("GatewayFetch: /ivp/ensemble/power begin() failed, attempt "));
        debugPtr->println(attempt);
      }
      httpCode = -1;
    } else {
      http.addHeader("Authorization", String("Bearer ") + gatewayJwt());
      httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        body = http.getString();
        http.end();
        break;
      }
      if (debugPtr) {
        debugPtr->print(F("GatewayFetch: /ivp/ensemble/power GET failed, attempt "));
        debugPtr->print(attempt);
        debugPtr->print(F(", httpCode="));
        debugPtr->print(httpCode);
        debugPtr->print(F(" ("));
        debugPtr->print(HTTPClient::errorToString(httpCode));
        debugPtr->println(F(")"));
      }
      http.end();
      if (httpCode == 401) {
        notifyAuthTokenRejected(); // see fetchMeterIds()'s identical comment
      }
    }
    if (attempt < MAX_ATTEMPTS) {
      delay(RETRY_DELAY_MS);
    }
  }
  if (httpCode != HTTP_CODE_OK) {
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    if (debugPtr) {
      debugPtr->print(F("GatewayFetch: /ivp/ensemble/power JSON parse failed: "));
      debugPtr->println(err.c_str());
    }
    return false;
  }

  JsonArray devices = doc["devices:"]; // trailing colon is correct -- see comment above
  if (devices.isNull() || devices.size() == 0) {
    if (debugPtr) {
      debugPtr->println(F("GatewayFetch: /ivp/ensemble/power no devices"));
    }
    return false;
  }

  float socSum = 0.0;
  float powerMwSum = 0.0;
  int count = 0;
  for (JsonVariant d : devices) {
    // d["soc"] | 0.0 silently defaults to 0.0 if "soc" is missing or null
    // -- confirmed as a real bug in practice (streaming_2026-09-25T09-31-12-797Z.txt):
    // right after a run of connection failures, one attempt got an
    // HTTP-200 response listing 2 devices but with no usable "soc" field
    // on them (gateway not fully back up yet), which this defaulting
    // turned into a plausible-looking "soc=0.0% power=0.0W (2 device(s))"
    // -- treated as a genuine reading, not a failure, which triggered the
    // low-SOC alarm and plotted a false 0% point on the Battery Charge
    // chart. A device entry with no numeric "soc" tells us nothing
    // trustworthy about the whole reading, so treat it the same as any
    // other failure -- return false rather than average in a fabricated
    // 0.0 for it.
    // Checked for TYPE, not just presence -- same reasoning as
    // DischargeToGridFetch.cpp's/ChargeFromGridFetch.cpp's own boundary-data
    // checks: a "soc" present but not actually a JSON number (e.g. a string
    // placeholder) would pass isNull() yet still silently cast to 0.0 below,
    // exactly the same class of bug the isNull() check was already added to
    // close.
    if (d["soc"].isNull() || !d["soc"].is<float>()) {
      if (debugPtr) {
        debugPtr->println(F("GatewayFetch: /ivp/ensemble/power device missing/non-numeric soc -- treating as failed"));
      }
      return false;
    }
    // Same reasoning as "soc" above, applied to its sibling field -- a
    // device could in principle report a valid soc but a missing
    // real_power_mw; defaulting that to 0.0 would silently zero out this
    // device's contribution to the whole-home load correction
    // (fetchGatewayPower()'s consumeKw += batteryPowerW) rather than
    // signal that the reading is incomplete.
    if (d["real_power_mw"].isNull() || !d["real_power_mw"].is<float>()) {
      if (debugPtr) {
        debugPtr->println(F("GatewayFetch: /ivp/ensemble/power device missing/non-numeric real_power_mw -- treating as failed"));
      }
      return false;
    }
    float deviceSoc = (float)d["soc"];
    float devicePowerMw = (float)d["real_power_mw"];
    // Confirmed as a SECOND, distinct recurrence of the same class of bug
    // the isNull() checks above were already added for
    // (zero_pereentBattery_streaming_2026-10-13T05-30-25-543Z.txt): "soc"
    // and "real_power_mw" both present, both a genuine JSON number (so the
    // type check above doesn't catch it), but BOTH exactly 0.0 -- right
    // after several consecutive polls read a stable ~91-93%/nonzero-power,
    // with solar actively flowing in at the time (so a real battery
    // couldn't plausibly have both discharged to 0% AND be drawing/
    // supplying exactly zero power simultaneously). This is the signature
    // of an uninitialized/zeroed device record (the gateway reporting a
    // placeholder, not a real reading), same "nothing trustworthy about
    // this device" situation as a missing field -- treated the same way:
    // fail the whole fetch rather than average in a fabricated 0%, which
    // previously plotted a false 0% point AND fired the low-SOC alarm.
    // Does not reject a device that's merely LOW (nonzero power while
    // discharging/charging at a genuinely low SOC) -- only the exact
    // double-zero combination, which a real battery essentially never
    // reports under normal grid-connected operation (Enphase's own
    // configured backup reserve keeps it from legitimately reaching 0%
    // at all). If the battery is ever genuinely stuck unreadable, repeated
    // failures here still reach socFetchStuck()'s own alarm path
    // (BuzzerControl.cpp) after enough consecutive polls -- this isn't
    // silently suppressing a real emergency, just not trusting one
    // suspicious instantaneous reading.
    if (deviceSoc == 0.0 && devicePowerMw == 0.0) {
      if (debugPtr) {
        debugPtr->println(F("GatewayFetch: /ivp/ensemble/power device reports soc=0.0 AND real_power_mw=0.0 -- treating as an uninitialized/zeroed record, not a genuine reading"));
      }
      return false;
    }
    socSum += deviceSoc;
    powerMwSum += devicePowerMw;
    count++;
  }
  socPercent = socSum / count;
  powerW = powerMwSum / 1000.0; // milliwatts -> watts
  if (debugPtr) {
    debugPtr->print(F("GatewayFetch: battery soc="));
    debugPtr->print(socPercent, 1);
    debugPtr->print(F("% power="));
    debugPtr->print(powerW, 1);
    debugPtr->print(F("W ("));
    debugPtr->print(count);
    debugPtr->println(F(" device(s))"));
  }
  return true;
}

bool fetchGatewayPower(float &solarKw, float &consumeKw, float &batterySocPercent) {
  batterySocPercent = -1.0; // sentinel -- only valid if the battery fetch below succeeds
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  if (!haveMeterIds && !fetchMeterIds()) {
    return false;
  }

  bool haveProduction = false;
  bool haveNetConsumption = false;
  float productionW = 0;
  float netConsumptionW = 0;
  {
    // Own scope so this WiFiClientSecure/HTTPClient are fully DESTRUCTED
    // (not just http.end()'d) before fetchBatteryStatus() below opens its
    // own -- confirmed via deviceLog.txt: /ivp/ensemble/power was failing
    // with httpCode=-1 (connection refused) on 100% of poll cycles, even
    // with dailyPrediction/plot_today_solar.py (the only other client
    // that ever talks to this gateway) shut down entirely, ruling out any
    // OTHER client contending for the gateway's connection. http.end()
    // closes the TCP connection but the WiFiClientSecure object itself --
    // and whatever SSL buffers/resources it still holds -- stayed alive
    // for the rest of this function (it was declared in the OUTER scope,
    // not destructed until fetchGatewayPower() itself returned), so the
    // two WiFiClientSecure instances were coexisting the whole time
    // fetchBatteryStatus() tried to open its own.
    WiFiClientSecure client;
    client.setInsecure(); // local gateway, self-signed cert -- see GatewayFetch.h
    client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S); // bounds the TLS handshake phase -- see comment above
    client.setTimeout(RESPONSE_TIMEOUT_MS);          // response body read timeout
    String url = String("https://") + gatewayIp() + "/ivp/meters/readings";

    HTTPClient http;
    http.setTimeout(RESPONSE_TIMEOUT_MS);
    if (!http.begin(client, url)) {
      if (debugPtr) {
        debugPtr->println(F("GatewayFetch: /ivp/meters/readings begin() failed"));
      }
      return false;
    }
    http.addHeader("Authorization", String("Bearer ") + gatewayJwt());
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
      if (debugPtr) {
        debugPtr->print(F("GatewayFetch: /ivp/meters/readings GET failed, httpCode="));
        debugPtr->print(httpCode);
        debugPtr->print(F(" ("));
        debugPtr->print(HTTPClient::errorToString(httpCode));
        debugPtr->println(F(")"));
      }
      http.end();
      if (httpCode == 401) {
        notifyAuthTokenRejected(); // see fetchMeterIds()'s identical comment
      }
      return false;
    }

    String body = http.getString();
    http.end();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
      if (debugPtr) {
        debugPtr->print(F("GatewayFetch: /ivp/meters/readings JSON parse failed: "));
        debugPtr->println(err.c_str());
      }
      return false;
    }

    for (JsonVariant r : doc.as<JsonArray>()) {
      long eid = r["eid"] | -1L;
      // r["activePower"] | 0.0 would silently accept a matching-eid entry
      // whose "activePower" is missing/null as a genuine "0W" reading --
      // same bug class fixed in fetchBatteryStatus() below (a device
      // present but missing its actual value tells us nothing trustworthy,
      // so skip it -- haveProduction/haveNetConsumption staying false
      // correctly fails the whole poll via the !haveProduction ||
      // !haveNetConsumption check below, instead of plotting a fabricated
      // 0W solar/consumption point as genuine ACTUAL data).
      if (eid == productionEid && !r["activePower"].isNull()) {
        productionW = r["activePower"];
        haveProduction = true;
      } else if (eid == netConsumptionEid && !r["activePower"].isNull()) {
        netConsumptionW = r["activePower"];
        haveNetConsumption = true;
      }
    }
  } // client/http destructed here, before fetchBatteryStatus() runs

  if (!haveProduction || !haveNetConsumption) {
    if (debugPtr) {
      debugPtr->println(F("GatewayFetch: /ivp/meters/readings missing production/net-consumption reading"));
    }
    return false;
  }

  solarKw = productionW / 1000.0;
  // (productionW + netConsumptionW) alone is NOT true whole-home load --
  // it's what plot_today_solar.py calls "consume_w", and while the
  // battery is charging that energy never reaches the grid meter
  // (netConsumptionW), so this undercounts load by roughly the battery's
  // charge power -- confirmed against today_solar.log's own LOAD CONTROL
  // line: e.g. "grid=+15W solar=965W batt=-857W -> houseLoad=123W", NOT
  // 980W. True load = consume_w + batt_w (batt_w positive=discharging,
  // negative=charging, matching fetch_battery_status()'s houseLoad_w).
  // batteryPowerW is added below once fetched.
  consumeKw = (productionW + netConsumptionW) / 1000.0;

  float batteryPowerW = 0.0;
  if (fetchBatteryStatus(batterySocPercent, batteryPowerW)) {
    consumeKw += batteryPowerW / 1000.0; // correct to true whole-home load
  }
  // else: best-effort -- solar/consumption already succeeded above
  // regardless, just left uncorrected (and batterySocPercent at its -1
  // sentinel) for this one poll.

  if (debugPtr) {
    debugPtr->print(F("GatewayFetch: solar="));
    debugPtr->print(solarKw, 3);
    debugPtr->print(F("kW consume="));
    debugPtr->print(consumeKw, 3);
    debugPtr->println(F("kW"));
  }

  return true;
}
// ================= end of GatewayFetch.cpp  file
