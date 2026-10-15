// SiteSettings.cpp  file  =================
#include "SiteSettings.h"
#include <ESP_PicoW_LittleFSsupport.h> // initializeFS()
#include <LittleFS.h>
#include <pfodDebugPtr.h>
#include "tzPosix.h" // cleanUpPosixTZStr()

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const char *SITE_SETTINGS_PATH = "/site_settings.csv";

static float gLatitude = 0.0;
static float gLongitude = 0.0;
static float gPanelTilt = 0.0;
static float gPanelAzimuth = 0.0;
static float gSystemKw = 0.0;
static String gPosixTimezone;
static bool gHasPosixTimezoneKey = false; // true once the posix_timezone KEY
// itself has been seen in site_settings.csv, even if its VALUE was blank --
// distinct from gPosixTimezone.length()==0, which is also true before
// loadSiteConfig() has ever run at all. get_ntpSupport_DefaultTZ() below
// needs this distinction: key-missing-entirely defers to the shared
// library's own weak-method default, but key-present-but-blank is a
// deliberate "use plain UTC" choice, cleaned up to "GMT0" below instead.
static String gCarChargerIp;
static String gHotWaterIp;

static Print *dbg() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  return debugPtr;
}

bool loadSiteConfig() {
  dbg();

  if (!initializeFS()) {
    if (debugPtr) {
      debugPtr->println(F("SiteSettings: LittleFS mount failed"));
    }
    return false;
  }

  File f = LittleFS.open(SITE_SETTINGS_PATH, "r");
  if (!f) {
    if (debugPtr) {
      debugPtr->print(F("SiteSettings: could not open "));
      debugPtr->println(SITE_SETTINGS_PATH);
    }
    return false;
  }

  f.readStringUntil('\n'); // header row (key,value)
  int found = 0;
  // Any key NOT in the 8 recognised below -- e.g. open_meteo_timezone,
  // removed 2026-09-28, left behind in a stale file that was never
  // re-uploaded after this firmware's own schema change. Added the same
  // day: confirmed by direct report that reflashing with the new (6-key)
  // firmware against an old (7-key) LittleFS file produced NO warning at
  // all -- the old if/else-if chain below just silently skipped the one
  // line it didn't recognise, and since the other 6 keys were all still
  // present, found==6 and loadSiteConfig() reported success. Purely
  // diagnostic -- does NOT fail loadSiteConfig() itself (see the return
  // statement's own comment for why), matching this project's own
  // PC-side equivalent (BatterySolarMonitorUpdate/py_programs/
  // quarterly_update.py's _check_config_csv(), which has always checked
  // for exactly this).
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
    // Only the FIRST comma splits key from value -- posix_timezone's own
    // value (a POSIX TZ string) contains further commas, which stay part
    // of the value here, same as every other loader in this project's
    // CSV-not-JSON convention (see EnphaseAuth.cpp).
    String value = line.substring(comma + 1);
    value.trim();

    if (key == "latitude") {
      gLatitude = value.toFloat();
      found++;
    } else if (key == "longitude") {
      gLongitude = value.toFloat();
      found++;
    } else if (key == "panel_tilt") {
      gPanelTilt = value.toFloat();
      found++;
    } else if (key == "panel_azimuth") {
      gPanelAzimuth = value.toFloat();
      found++;
    } else if (key == "system_kw") {
      gSystemKw = value.toFloat();
      found++;
    } else if (key == "posix_timezone") {
      gPosixTimezone = value;
      if (gPosixTimezone.length() == 0) {
        // Deliberately blank in the CSV -- clean up to a valid, explicit
        // "UTC, no DST" POSIX string (same library routine
        // setInitialTimeZoneConfig()/webPages.cpp already use for this)
        // rather than handing NTP/Open-Meteo an empty TZ string.
        cleanUpPosixTZStr(gPosixTimezone); // "" -> "GMT0"
      }
      gHasPosixTimezoneKey = true;
      found++;
    } else if (key == "car_charger_ip") {
      gCarChargerIp = value;
      found++;
    } else if (key == "hot_water_ip") {
      gHotWaterIp = value;
      found++;
    } else if (key == "gateway_ip" || key == "enlighten_system_id" ||
               key == "enlighten_email" || key == "enlighten_password" ||
               key == "gateway_serial") {
      // EnphaseAuth.cpp's own 5 IDENTITY keys, in this SAME file (see
      // this file's own header comment: "two loaders, one file") --
      // recognised here only so they're NOT wrongly flagged as an
      // unexpected extra key below; EnphaseAuth.cpp's own loadEnphaseAuth()
      // is what actually reads and validates them.
    } else {
      if (extraKeys.length() > 0) {
        extraKeys += F(", ");
      }
      extraKeys += key;
    }
  }
  f.close();

  if (debugPtr) {
    debugPtr->print(F("SiteSettings: loaded "));
    debugPtr->print(found);
    debugPtr->print(F("/8 keys from "));
    debugPtr->println(SITE_SETTINGS_PATH);
    if (found != 8) {
      debugPtr->println(F("SiteSettings: WARNING -- expected all 8 keys "
                           "(latitude, longitude, panel_tilt, panel_azimuth, "
                           "system_kw, posix_timezone, car_charger_ip, "
                           "hot_water_ip); site_settings.csv is "
                           "a genuine, entirely human-maintained config file "
                           "(nothing regenerates it), so a short file here "
                           "means it's stale or was hand-edited -- fix and "
                           "re-upload."));
    }
    if (extraKeys.length() > 0) {
      debugPtr->print(F("SiteSettings: WARNING -- unexpected extra key(s) in "));
      debugPtr->print(SITE_SETTINGS_PATH);
      debugPtr->print(F(": "));
      debugPtr->print(extraKeys);
      debugPtr->println(F(" -- silently ignored (a stale file from before a "
                           "schema change, most likely -- re-upload the "
                           "current site_settings.csv to clear this)."));
    }
  }
  // Extra keys alone don't fail this -- all 8 REQUIRED values are still
  // present and correct either way, so the device can keep operating
  // normally on a stale-but-otherwise-valid file; the warning above is
  // enough to surface the problem without treating harmless leftover data
  // as a hard config failure the way a genuinely MISSING required key is.
  return found == 8;
}

float siteLatitude() { return gLatitude; }
float siteLongitude() { return gLongitude; }
float sitePanelTilt() { return gPanelTilt; }
float sitePanelAzimuth() { return gPanelAzimuth; }
float siteSystemKw() { return gSystemKw; }
const char *sitePosixTimezone() { return gPosixTimezone.c_str(); }
const char *siteCarChargerIp() { return gCarChargerIp.c_str(); }
const char *siteHotWaterIp() { return gHotWaterIp.c_str(); }

// Strong override of ntpSupport.cpp's weak get_ntpSupport_DefaultTZ() --
// see that file's own `__attribute__((weak))` declaration, a deliberate
// extension point so callers can supply a site-specific default WITHOUT
// editing ntpSupport.cpp itself (a personal cross-project library file --
// this project's own convention is to leave it alone unless explicitly
// asked; this weak-function hook is exactly how that's meant to happen).
// Falls back to ntpSupport.cpp's own original hardcoded default (its own
// weak get_ntpSupport_DefaultTZ() body -- copied verbatim here, since a
// strong override like this one entirely replaces the weak symbol at link
// time, so there's no way to actually call through to it) if
// loadSiteConfig() hasn't run yet or the posix_timezone KEY itself wasn't
// even present in site_settings.csv, so a missing/incomplete file degrades
// to the shared library's own generic fixed behaviour rather than handing
// NTP an empty TZ string. A key that IS present but deliberately left
// BLANK is a different case, already cleaned up to "GMT0" above -- that's
// an explicit site choice (plain UTC), not a missing-config fallback.
const char *get_ntpSupport_DefaultTZ() {
  if (!gHasPosixTimezoneKey) {
    return "AEST-10AEDT,M10.1.0,M4.1.0/3"; // ntpSupport.cpp's own original default
  }
  return gPosixTimezone.c_str();
}
// ================= end of SiteSettings.cpp  file
