// EnlightenLoginFetch.cpp  file  =================
#include "EnlightenLoginFetch.h"
#include "EnphaseAuth.h"
#include <Arduino.h> // millis()
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <pfodDebugPtr.h>
#include <pfodDelay.h>
#include <string.h> // strlen()

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const char *LOGIN_HOST = "enlighten.enphaseenergy.com";
static const char *TOKEN_HOST = "entrez.enphaseenergy.com";

// 0.0.0.0 until resolveEnlightenLoginHosts() succeeds -- same
// DNS-crash-avoidance pattern as SolarForecastFetch.cpp's resolvedIP (see
// that file's comment: lwIP DNS calls are only safe on the main thread,
// never on InternetPollTask.cpp's background task).
static IPAddress loginIP((uint32_t)0);
static IPAddress tokenIP((uint32_t)0);

// Same timeout bounds, same reasoning, as SolarForecastFetch.cpp's/
// EnlightenHistoryFetch.cpp's own HANDSHAKE_TIMEOUT_S/RESPONSE_TIMEOUT_MS.
static const unsigned long HANDSHAKE_TIMEOUT_S = 5;
static const uint32_t RESPONSE_TIMEOUT_MS = 8000;

// Give WiFi/DNS a little time to settle after boot before the first
// attempt -- same role as EnlightenHistoryRefresh.cpp's
// MIN_UPTIME_BEFORE_HISTORY_MS, a different (shorter) value only because
// this doesn't also need NTP to have synced (no date= query here).
static const unsigned long MIN_UPTIME_BEFORE_LOGIN_MS = 15000;

// How long to wait before retrying after a FAILED attempt. The
// still-cached gatewayJwt()/enlightenSession() (whatever this device
// logged in with last -- yesterday, most likely) remain valid and
// continue to be used by GatewayFetch.cpp/EnlightenHistoryFetch.cpp in
// the meantime; a single failed refresh doesn't invalidate anything
// already held. Retries keep going at this interval, indefinitely, until
// one succeeds -- there is no daily giving-up; DailyReboot.h's reboot
// just happens to also reset this cycle, but isn't relied on to make
// progress happen. Same 5-minute cadence as this project's other
// POLL_INTERVAL_MS constants (CarChargerTask.cpp/HotWaterTask.cpp/
// LoadControlTask.cpp) rather than EnlightenHistoryRefresh.cpp's much
// shorter 30s RETRY_DELAY_MS -- a login failure (bad password, account
// issue) is less likely to be a transient blip worth hammering quickly
// than a single flaky HTTP fetch is.
static const unsigned long RETRY_INTERVAL_MS = 5UL * 60UL * 1000UL;

// pfodDelay (pfodParser/src/pfodDelay.h), not a plain millis()/nextAttemptAtMs
// comparison -- see GatewayPollTask.cpp's identical comment on its own
// pollTimer for why, and for the isRunning()/justFinished() usage idiom.
//
// Started once, in initializeEnlightenLoginFetch() (called from setup()),
// with MIN_UPTIME_BEFORE_LOGIN_MS as its first delay -- NOT lazily armed
// from inside checkEnlightenLoginPoll() via an isRunning()-gated start().
// That lazy-arm shape is only safe when justFinished() is checked BEFORE
// isRunning() is used to decide whether to arm (GatewayPollTask.cpp etc:
// isRunning() check, then justFinished() second, every call). Checking
// justFinished() unconditionally FIRST (pfodParserAI-guide.md sec 4) and
// then consulting isRunning() breaks that: justFinished() returning true
// already stops the timer as a side effect, so an isRunning()-gated
// re-arm reached right after would misread "just became due" as "never
// started" and reset the timer's real (non-zero) delay every single time
// it fires -- confirmed as a real bug in practice (streaming_2026-09-22T02-19-05-105Z.txt:
// checkEnlightenLoginPoll() never once reached the login logic across a
// 5-minute boot log; RETRY_INTERVAL_MS is long enough that this kind of
// perpetual self-reset is easy to miss over a short log). Arming once via
// init(), like EnlightenHistoryRefresh.cpp's own attemptTimer, avoids the
// whole class of bug by never re-entering an isRunning()-gated arm block
// at all.
static pfodDelay attemptTimer;
static bool loginSucceededThisBoot = false;

void initializeEnlightenLoginFetch() {
  attemptTimer.start(MIN_UPTIME_BEFORE_LOGIN_MS);
}

IPAddress enlightenHostIP() {
  return loginIP;
}

static Print *dbg() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  return debugPtr;
}

bool resolveEnlightenLoginHosts() {
  dbg();
  IPAddress ip1, ip2;
  bool ok1 = WiFi.hostByName(LOGIN_HOST, ip1);
  bool ok2 = WiFi.hostByName(TOKEN_HOST, ip2);
  if (ok1) {
    loginIP = ip1;
  }
  if (ok2) {
    tokenIP = ip2;
  }
  if (debugPtr) {
    debugPtr->print(F("EnlightenLoginFetch: resolved "));
    debugPtr->print(LOGIN_HOST);
    debugPtr->print(F(" -> "));
    debugPtr->print(ok1 ? loginIP.toString() : String(F("FAILED")));
    debugPtr->print(F(", "));
    debugPtr->print(TOKEN_HOST);
    debugPtr->print(F(" -> "));
    debugPtr->println(ok2 ? tokenIP.toString() : String(F("FAILED")));
  }
  return ok1 && ok2;
}

// RFC 3986 percent-encoding for a application/x-www-form-urlencoded value
// -- needed because enlightenPassword() can contain '&', '=', '+', etc.,
// any of which would corrupt the form body if sent raw (this is exactly
// why enphase_token.py's own Python _login() uses urllib.parse.urlencode()
// rather than plain string concatenation).
static String urlEncode(const char *s) {
  String out;
  for (const char *p = s; *p; p++) {
    char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

// Minimal JSON string escaping (quote + backslash) -- session_id/
// gatewaySerial()/enlightenEmail() are all simple opaque identifiers in
// practice, but this costs nothing and avoids ever emitting invalid JSON
// if one of them ever contains a special character.
static String jsonEscape(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

// Extracts the _enlighten_4_session cookie value from a raw Set-Cookie
// header value (e.g. "_enlighten_4_session=abc123; path=/; HttpOnly").
// Returns "" if not found.
static String extractSessionCookie(const String &setCookieHeader) {
  const char *needle = "_enlighten_4_session=";
  int start = setCookieHeader.indexOf(needle);
  if (start < 0) {
    return String();
  }
  start += strlen(needle);
  int end = setCookieHeader.indexOf(';', start);
  if (end < 0) {
    end = setCookieHeader.length();
  }
  return setCookieHeader.substring(start, end);
}

// Step 1: POST email/password, get back session_id (JSON body) and the
// _enlighten_4_session cookie (Set-Cookie header). Returns true and fills
// both out-params on success.
static bool loginStep(String &sessionIdOut, String &cookieOut) {
  WiFiClientSecure client;
  client.setInsecure(); // no cert validation -- see SolarForecastFetch.h for this project's own reasoning
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S);

  const uint16_t port = 443;
  if (!client.connect(loginIP, port, LOGIN_HOST, NULL, NULL, NULL)) {
    if (debugPtr) {
      debugPtr->println(F("EnlightenLoginFetch: login TLS connect failed"));
    }
    return false;
  }
  client.setTimeout(RESPONSE_TIMEOUT_MS);

  String body = String("user[email]=") + urlEncode(enlightenEmail())
              + "&user[password]=" + urlEncode(enlightenPassword());

  HTTPClient http;
  http.setTimeout(RESPONSE_TIMEOUT_MS);
  if (!http.begin(client, LOGIN_HOST, port, "/login/login.json", true)) {
    if (debugPtr) {
      debugPtr->println(F("EnlightenLoginFetch: login http.begin() failed"));
    }
    return false;
  }
  const char *headerKeys[] = {"Set-Cookie"};
  http.collectHeaders(headerKeys, 1);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  int httpCode = http.POST(body);
  if (httpCode != HTTP_CODE_OK) {
    if (debugPtr) {
      debugPtr->print(F("EnlightenLoginFetch: login POST failed, httpCode="));
      debugPtr->print(httpCode);
      debugPtr->print(F(" ("));
      debugPtr->print(HTTPClient::errorToString(httpCode));
      debugPtr->println(F(")"));
    }
    http.end();
    return false;
  }

  String setCookie = http.header("Set-Cookie");
  cookieOut = extractSessionCookie(setCookie);

  String respBody = http.getString();
  http.end();

  if (cookieOut.length() == 0) {
    if (debugPtr) {
      debugPtr->println(F("EnlightenLoginFetch: no _enlighten_4_session cookie in login response"
                           " -- ESP32 HTTPClient may only be capturing one Set-Cookie header;"
                           " check deviceLog.txt against a real login response if this persists"));
    }
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, respBody);
  if (err) {
    if (debugPtr) {
      debugPtr->print(F("EnlightenLoginFetch: login JSON parse failed: "));
      debugPtr->println(err.c_str());
    }
    return false;
  }
  const char *sid = doc["session_id"];
  if (!sid) {
    if (debugPtr) {
      debugPtr->println(F("EnlightenLoginFetch: login response had no session_id"
                           " -- check email/password in data/site_settings.csv"));
    }
    return false;
  }
  sessionIdOut = sid;
  return true;
}

// Step 2: POST session_id/serial/username, get back the JWT (plain-text body).
static bool tokenStep(const String &sessionId, String &jwtOut) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S);

  const uint16_t port = 443;
  if (!client.connect(tokenIP, port, TOKEN_HOST, NULL, NULL, NULL)) {
    if (debugPtr) {
      debugPtr->println(F("EnlightenLoginFetch: token TLS connect failed"));
    }
    return false;
  }
  client.setTimeout(RESPONSE_TIMEOUT_MS);

  String body = String("{\"session_id\":\"") + jsonEscape(sessionId)
              + "\",\"serial_num\":\"" + jsonEscape(gatewaySerial())
              + "\",\"username\":\"" + jsonEscape(enlightenEmail()) + "\"}";

  HTTPClient http;
  http.setTimeout(RESPONSE_TIMEOUT_MS);
  if (!http.begin(client, TOKEN_HOST, port, "/tokens", true)) {
    if (debugPtr) {
      debugPtr->println(F("EnlightenLoginFetch: token http.begin() failed"));
    }
    return false;
  }
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(body);
  if (httpCode != HTTP_CODE_OK) {
    if (debugPtr) {
      debugPtr->print(F("EnlightenLoginFetch: token POST failed, httpCode="));
      debugPtr->print(httpCode);
      debugPtr->print(F(" ("));
      debugPtr->print(HTTPClient::errorToString(httpCode));
      debugPtr->println(F(")"));
    }
    http.end();
    return false;
  }

  jwtOut = http.getString();
  jwtOut.trim();
  http.end();
  return jwtOut.length() > 0;
}

enum TokenCheck { TOKEN_VALID, TOKEN_INVALID, TOKEN_UNKNOWN };

// GET gatewayIp()/ivp/meters with the current gatewayJwt() -- the same
// request GatewayFetch.cpp's own fetchMeterIds() makes, reused here purely
// to read the status code back. A confirmed 401 means the JWT itself was
// rejected (TOKEN_INVALID); anything else -- success, connect failure,
// timeout, a different HTTP error -- is NOT treated as proof the token is
// bad (TOKEN_UNKNOWN) -- see checkEnlightenLoginPoll()'s own reasoning for
// why only a confirmed 401 should trigger a full rebuild.
static TokenCheck checkGatewayTokenValid() {
  WiFiClientSecure client;
  client.setInsecure(); // local gateway, self-signed cert -- see GatewayFetch.h
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S);
  client.setTimeout(RESPONSE_TIMEOUT_MS);
  String url = String("https://") + gatewayIp() + "/ivp/meters";

  HTTPClient http;
  http.setTimeout(RESPONSE_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    return TOKEN_UNKNOWN;
  }
  http.addHeader("Authorization", String("Bearer ") + gatewayJwt());
  int httpCode = http.GET();
  http.getString(); // drain before end() -- matches this project's convention elsewhere
  http.end();
  if (httpCode == HTTP_CODE_OK) {
    return TOKEN_VALID;
  }
  if (httpCode == 401) {
    return TOKEN_INVALID;
  }
  return TOKEN_UNKNOWN;
}

// GET enlighten.enphaseenergy.com/systems/{system_id}/energy_lifetime with
// the current enlightenSession() cookie. Chosen specifically because it
// needs no date= parameter (unlike power_time_series) -- so, like the rest
// of this file, this check doesn't depend on NTP having synced yet (see
// MIN_UPTIME_BEFORE_LOGIN_MS's own comment). Same interpretation as
// checkGatewayTokenValid(): only a confirmed 401 counts as TOKEN_INVALID.
static TokenCheck checkEnlightenTokenValid() {
  if (loginIP == IPAddress((uint32_t)0)) {
    return TOKEN_UNKNOWN; // hosts not resolved yet
  }
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S);
  const uint16_t port = 443;
  if (!client.connect(loginIP, port, LOGIN_HOST, NULL, NULL, NULL)) {
    return TOKEN_UNKNOWN;
  }
  client.setTimeout(RESPONSE_TIMEOUT_MS);

  String uri = String("/systems/") + enlightenSystemId() + "/energy_lifetime";
  HTTPClient http;
  http.setTimeout(RESPONSE_TIMEOUT_MS);
  if (!http.begin(client, LOGIN_HOST, port, uri, true)) {
    return TOKEN_UNKNOWN;
  }
  http.addHeader("Cookie", String("_enlighten_4_session=") + enlightenSession());
  http.addHeader("e-auth-token", enlightenSession());
  int httpCode = http.GET();
  http.getString(); // drain before end()
  http.end();
  if (httpCode == HTTP_CODE_OK) {
    return TOKEN_VALID;
  }
  if (httpCode == 401) {
    return TOKEN_INVALID;
  }
  return TOKEN_UNKNOWN;
}

static bool performEnlightenLogin() {
  dbg();

  if (loginIP == IPAddress((uint32_t)0) || tokenIP == IPAddress((uint32_t)0)) {
    if (debugPtr) {
      debugPtr->println(F("EnlightenLoginFetch: hosts not resolved yet, skipping"));
    }
    return false;
  }

  String sessionId, cookie, jwt;
  if (!loginStep(sessionId, cookie)) {
    return false;
  }
  if (!tokenStep(sessionId, jwt)) {
    // Login itself succeeded (cookie is valid) even though the JWT step
    // failed -- still worth saving the refreshed session cookie, since
    // EnlightenHistoryFetch.cpp only needs that one, not the JWT.
    setEnlightenSession(cookie.c_str());
    saveAuthToken();
    if (debugPtr) {
      debugPtr->println(F("EnlightenLoginFetch: session cookie refreshed but JWT step failed"
                           " -- gatewayJwt() unchanged this run"));
    }
    return false;
  }

  setEnlightenSession(cookie.c_str());
  setGatewayJwt(jwt.c_str());
  bool saved = saveAuthToken();
  if (debugPtr) {
    debugPtr->print(F("EnlightenLoginFetch: self-refresh "));
    debugPtr->println(saved ? F("OK -- session + JWT saved to /auth_token.csv")
                             : F("got new session + JWT but LittleFS save FAILED"));
  }
  return saved;
}

void checkEnlightenLoginPoll() {
  dbg();

  // Checked unconditionally, at the outermost level, on every single
  // call -- per pfodParserAI-guide.md sec 4, "the one rule that matters"
  // -- before loginSucceededThisBoot (below) can skip it. No isRunning()-
  // gated arming here at all -- attemptTimer is armed exactly once, in
  // initializeEnlightenLoginFetch() (called from setup()), which is what
  // makes checking justFinished() first, unconditionally, safe (see that
  // timer's own top-of-file comment for the bug this avoids).
  bool attemptDue = attemptTimer.justFinished();

  if (loginSucceededThisBoot) {
    return; // done for now -- next attempt is next reboot (DailyReboot.h, daily), or sooner via notifyAuthTokenRejected()
  }
  if (!attemptDue) {
    return; // waiting out the startup floor or a retry delay
  }

  if (strlen(enlightenEmail()) == 0 || strlen(enlightenPassword()) == 0) {
    // Should not happen -- enlighten_email/enlighten_password are always
    // expected in /site_settings.csv (loadEnphaseAuth() requires all 5
    // identity keys to even report success), so getting here means that
    // check itself failed, or the file was hand-edited/truncated after
    // boot. Retrying won't fix a missing credential, so this one case
    // does NOT retry -- log loudly once and stop, rather than either
    // silently giving up (the old behaviour) or retrying forever for
    // something a retry can't solve.
    if (debugPtr) {
      debugPtr->println(F("EnlightenLoginFetch: enlighten_email/enlighten_password "
                           "missing from /site_settings.csv -- re-export via "
                           "quarterly_update.py and re-upload the LittleFS data"));
    }
    loginSucceededThisBoot = true; // not really "succeeded" -- just stop trying this boot
    return;
  }

  // Existing tokens (from a previous boot's login, persisted in
  // auth_token.csv) get TESTED, not blindly discarded -- only a confirmed
  // TOKEN_INVALID (401) triggers a rebuild. TOKEN_UNKNOWN (network hiccup,
  // timeout -- inconclusive) is treated the same as TOKEN_VALID here: it's
  // not proof the token is bad, and rebuilding on every transient
  // connectivity blip would defeat the point of testing first. A missing/
  // empty token (auth_token.csv didn't exist, or this is a fresh device)
  // skips straight to building one -- there's nothing to test.
  bool haveExistingTokens = strlen(gatewayJwt()) > 0 && strlen(enlightenSession()) > 0;
  if (haveExistingTokens) {
    TokenCheck gw = checkGatewayTokenValid();
    TokenCheck en = checkEnlightenTokenValid();
    bool confirmedInvalid = (gw == TOKEN_INVALID) || (en == TOKEN_INVALID);
    if (!confirmedInvalid) {
      if (debugPtr) {
        debugPtr->print(F("EnlightenLoginFetch: existing tokens OK (gateway="));
        debugPtr->print(gw == TOKEN_VALID ? F("valid") : F("unknown"));
        debugPtr->print(F(", enlighten="));
        debugPtr->print(en == TOKEN_VALID ? F("valid") : F("unknown"));
        debugPtr->println(F(") -- skipping login"));
      }
      loginSucceededThisBoot = true;
      return;
    }
    if (debugPtr) {
      debugPtr->print(F("EnlightenLoginFetch: existing token(s) confirmed invalid (gateway="));
      debugPtr->print(gw == TOKEN_INVALID ? F("INVALID") : F("ok/unknown"));
      debugPtr->print(F(", enlighten="));
      debugPtr->print(en == TOKEN_INVALID ? F("INVALID") : F("ok/unknown"));
      debugPtr->println(F(") -- rebuilding"));
    }
  } else if (debugPtr) {
    debugPtr->println(F("EnlightenLoginFetch: no existing tokens (auth_token.csv "
                         "missing or empty) -- building fresh"));
  }

  if (performEnlightenLogin()) {
    loginSucceededThisBoot = true;
    return;
  }

  // Failed -- the cached gatewayJwt()/enlightenSession() from the last
  // successful login are untouched and still valid, so nothing is broken
  // in the meantime. Retry again in RETRY_INTERVAL_MS, indefinitely, until
  // one succeeds (see RETRY_INTERVAL_MS's own comment).
  if (debugPtr) {
    debugPtr->print(F("EnlightenLoginFetch: attempt failed, retrying in "));
    debugPtr->print(RETRY_INTERVAL_MS / 60000UL);
    debugPtr->println(F("min"));
  }
  attemptTimer.start(RETRY_INTERVAL_MS);
}

void notifyAuthTokenRejected() {
  dbg();
  if (!loginSucceededThisBoot) {
    return; // already mid-check-or-retry-cycle -- nothing new to do
  }
  if (debugPtr) {
    debugPtr->println(F("EnlightenLoginFetch: a real request got 401 after this boot's "
                         "check already concluded the token was OK -- re-checking now"));
  }
  loginSucceededThisBoot = false;
  attemptTimer.start(0); // due immediately on the next checkEnlightenLoginPoll() tick
}
// ================= end of EnlightenLoginFetch.cpp  file
