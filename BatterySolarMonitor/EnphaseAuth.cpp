// EnphaseAuth.cpp  file  =================
#include "EnphaseAuth.h"
#include <ESP_PicoW_LittleFSsupport.h> // initializeFS()
#include <LittleFS.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// Two files, two owners -- see EnphaseAuth.h's top-of-file comment.
static const char *SITE_SETTINGS_PATH = "/site_settings.csv"; // PC-seeded, rarely changes
static const char *AUTH_TOKEN_PATH = "/auth_token.csv";     // device-owned, rewritten ~daily

static String gGatewayIp;
static String gGatewayJwt;
static String gEnlightenSession;
static String gEnlightenSystemId;
static String gEnlightenEmail;
static String gEnlightenPassword;
static String gGatewaySerial;

static Print *dbg() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  return debugPtr;
}

// Reads the 5 PC-seeded identity keys from site_settings.csv. Required --
// returns false (and logs which keys were short) if any is missing.
static bool loadSiteSettings() {
  File f = LittleFS.open(SITE_SETTINGS_PATH, "r");
  if (!f) {
    if (debugPtr) {
      debugPtr->print(F("EnphaseAuth: could not open "));
      debugPtr->println(SITE_SETTINGS_PATH);
    }
    return false;
  }

  f.readStringUntil('\n'); // header row (key,value)
  int found = 0;
  // Any key not in this loader's own 5, AND not one of SiteSettings.cpp's
  // own 8 keys in this SAME file (see EnphaseAuth.h's "two loaders, one
  // file" comment) -- e.g. open_meteo_timezone, removed from that schema
  // 2026-09-28 and left behind in a stale file never re-uploaded after
  // the firmware change. Added the same day: confirmed by direct report
  // that reflashing new firmware against an old, stale site_settings.csv
  // produced no warning at all from either loader. Purely diagnostic --
  // does NOT fail loadSiteSettings() itself, see the return statement.
  //
  // car_charger_ip/hot_water_ip added to this recognised list 2026-10-08,
  // per direct report: SiteSettings.cpp's own schema grew these 2 keys at
  // some earlier point (its own warning already said "expected all 8
  // keys"), but this list was never updated to match, so a perfectly
  // current, complete site_settings.csv (both keys present and correct)
  // still got wrongly flagged here as "unexpected extra" on every boot.
  String extraKeys;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    int comma = line.indexOf(',');
    if (comma < 0) {
      continue; // malformed line -- skip
    }
    String key = line.substring(0, comma);
    String value = line.substring(comma + 1);
    value.trim();

    if (key == "gateway_ip") {
      gGatewayIp = value;
      found++;
    } else if (key == "enlighten_system_id") {
      gEnlightenSystemId = value;
      found++;
    } else if (key == "enlighten_email") {
      gEnlightenEmail = value;
      found++;
    } else if (key == "enlighten_password") {
      gEnlightenPassword = value;
      found++;
    } else if (key == "gateway_serial") {
      gGatewaySerial = value;
      found++;
    } else if (key == "latitude" || key == "longitude" || key == "panel_tilt" ||
               key == "panel_azimuth" || key == "system_kw" || key == "posix_timezone" ||
               key == "car_charger_ip" || key == "hot_water_ip") {
      // SiteSettings.cpp's own 8 SITE keys, in this SAME file -- recognised
      // here only so they're not wrongly flagged as an unexpected extra
      // key below; SiteSettings.cpp's own loadSiteConfig() is what
      // actually reads and validates them.
    } else {
      if (extraKeys.length() > 0) {
        extraKeys += F(", ");
      }
      extraKeys += key;
    }
  }
  f.close();

  if (debugPtr) {
    debugPtr->print(F("EnphaseAuth: loaded "));
    debugPtr->print(found);
    debugPtr->print(F("/5 keys from "));
    debugPtr->println(SITE_SETTINGS_PATH);
    if (found != 5) {
      debugPtr->println(F("EnphaseAuth: WARNING -- expected all 5 keys "
                           "(gateway_ip, enlighten_system_id, enlighten_email, "
                           "enlighten_password, gateway_serial); "
                           "site_settings.csv is a genuine, entirely "
                           "human-maintained config file (nothing "
                           "regenerates it), so a short file here means "
                           "it's stale or was hand-edited -- fix and "
                           "re-upload."));
    }
    if (extraKeys.length() > 0) {
      debugPtr->print(F("EnphaseAuth: WARNING -- unexpected extra key(s) in "));
      debugPtr->print(SITE_SETTINGS_PATH);
      debugPtr->print(F(": "));
      debugPtr->print(extraKeys);
      debugPtr->println(F(" -- silently ignored (a stale file from before a "
                           "schema change, most likely -- re-upload the "
                           "current site_settings.csv to clear this)."));
    }
  }
  // Extra keys alone don't fail this -- all 5 REQUIRED values are still
  // present and correct either way -- see SiteSettings.cpp's own
  // loadSiteConfig() for the identical reasoning.
  return found == 5;
}

// Reads the 2 device-owned live-token keys from auth_token.csv, if the
// file exists at all. A missing file, or empty values in it, is the
// NORMAL state on a freshly-seeded device (before EnlightenLoginFetch.h's
// first successful login writes it) -- not an error, so this never
// returns false or logs a warning for that case.
static void loadAuthTokenFile() {
  File f = LittleFS.open(AUTH_TOKEN_PATH, "r");
  if (!f) {
    if (debugPtr) {
      debugPtr->print(F("EnphaseAuth: "));
      debugPtr->print(AUTH_TOKEN_PATH);
      debugPtr->println(F(" not found yet -- normal before the first successful login"));
    }
    return;
  }

  f.readStringUntil('\n'); // header row (key,value)
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    int comma = line.indexOf(',');
    if (comma < 0) {
      continue;
    }
    String key = line.substring(0, comma);
    String value = line.substring(comma + 1);
    value.trim();

    if (key == "gateway_jwt") {
      gGatewayJwt = value;
    } else if (key == "enlighten_session") {
      gEnlightenSession = value;
    }
  }
  f.close();

  if (debugPtr) {
    debugPtr->print(F("EnphaseAuth: loaded "));
    debugPtr->print(AUTH_TOKEN_PATH);
    debugPtr->print(F(" (jwt="));
    debugPtr->print(gGatewayJwt.length() > 0 ? F("present") : F("EMPTY"));
    debugPtr->print(F(", session="));
    debugPtr->print(gEnlightenSession.length() > 0 ? F("present") : F("EMPTY"));
    debugPtr->println(F(")"));
  }
}

bool loadEnphaseAuth() {
  dbg();

  if (!initializeFS()) {
    if (debugPtr) {
      debugPtr->println(F("EnphaseAuth: LittleFS mount failed"));
    }
    return false;
  }

  bool identityOk = loadSiteSettings();
  loadAuthTokenFile(); // best-effort -- see its own comment
  return identityOk;
}

bool saveAuthToken() {
  dbg();

  if (!initializeFS()) {
    if (debugPtr) {
      debugPtr->println(F("EnphaseAuth: LittleFS mount failed on save"));
    }
    return false;
  }

  File f = LittleFS.open(AUTH_TOKEN_PATH, "w");
  if (!f) {
    if (debugPtr) {
      debugPtr->print(F("EnphaseAuth: could not open "));
      debugPtr->print(AUTH_TOKEN_PATH);
      debugPtr->println(F(" for writing"));
    }
    return false;
  }

  f.print(F("key,value\n"));
  f.print(F("gateway_jwt,"));       f.print(gGatewayJwt);       f.print('\n');
  f.print(F("enlighten_session,")); f.print(gEnlightenSession); f.print('\n');
  f.close();

  if (debugPtr) {
    debugPtr->print(F("EnphaseAuth: saved "));
    debugPtr->println(AUTH_TOKEN_PATH);
  }
  return true;
}

const char *gatewayIp() { return gGatewayIp.c_str(); }
const char *gatewayJwt() { return gGatewayJwt.c_str(); }
void setGatewayJwt(const char *jwt) { gGatewayJwt = jwt; }

const char *enlightenSession() { return gEnlightenSession.c_str(); }
const char *enlightenSystemId() { return gEnlightenSystemId.c_str(); }
void setEnlightenSession(const char *session) { gEnlightenSession = session; }

const char *enlightenEmail() { return gEnlightenEmail.c_str(); }
const char *enlightenPassword() { return gEnlightenPassword.c_str(); }
const char *gatewaySerial() { return gGatewaySerial.c_str(); }
// ================= end of EnphaseAuth.cpp  file
