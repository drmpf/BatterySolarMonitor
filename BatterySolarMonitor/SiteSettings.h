// SiteSettings.h  file  =================
// Loads fixed per-site physical/location config from /site_settings.csv
// on LittleFS -- the same file EnphaseAuth.h reads its 5 Enphase identity
// fields from (gateway_ip, enlighten_system_id, enlighten_email,
// enlighten_password, gateway_serial); this module independently scans
// the SAME file for a different 6 keys and ignores the rows it doesn't
// recognise, same as EnphaseAuth.h does for these. Two loaders, one file
// -- deliberately not merged into EnphaseAuth.h itself, since these
// values have nothing to do with Enphase authentication (this is site
// geography/hardware config, not a credential).
//
// Until 2026-09-11 these were hardcoded constants duplicated across
// several files (SolarForecastFetch.cpp/SunPosition.cpp each had their
// own LAT/LON; SolarForecast.h its own SYSTEM_KW; ntpSupport.cpp's weak
// get_ntpSupport_DefaultTZ() its own POSIX TZ string; panel tilt/azimuth
// were bare literals in SolarForecastFetch.cpp's URI string, not even
// named). All now load from one place, matching ShadeFactorTable.h/
// EnphaseAuth.h's own LittleFS-data-file pattern -- changing a site
// constant is now a data-file re-upload, not a source edit + recompile +
// reflash, and LAT/LON in particular can no longer silently drift out of
// sync between the two files that used to each hardcode their own copy.
// (locality/locality_state, the site's BOM forecast locality, were also
// here from 2026-09-11 until the same-day BOM removal folded weather
// forecasting into SolarForecastFetch.cpp's Open-Meteo request, which
// needs no locality lookup at all -- see WeatherForecast.h.)
//
// Source of truth: ../site_settings.csv (one level above
// BatterySolarMonitorUpdate/), a genuine, entirely human-maintained config
// file -- nothing in that pipeline writes to it (changed 2026-09-27; see
// quarterly_update.py's own module docstring). Copied into
// BatterySolarMonitor/data/site_settings.csv by hand when it changes,
// before the next firmware+data upload.
#ifndef SITE_SETTINGS_H
#define SITE_SETTINGS_H

// Reads /site_settings.csv from LittleFS (mounts it via initializeFS() --
// ESP_PicoW_LittleFSsupport.h -- if not already mounted). Call once from
// setup(), before anything that calls the accessors below (in particular,
// before resolveSolarForecastHost()/resetDefaultTZstr(), which both need
// values this loads). Returns true only if all 8 keys were parsed.
bool loadSiteConfig();

// Site coordinates -- SolarForecastFetch.cpp's Open-Meteo query and
// SunPosition.cpp's own sun-elevation/azimuth calculation both need these
// (previously two independent hardcoded copies, now one).
float siteLatitude();
float siteLongitude();

// Panel installation -- SolarForecastFetch.cpp's Open-Meteo query
// (tilt=/azimuth= parameters, degrees; azimuth 0=South, -90=East, 90=West,
// Open-Meteo's own convention).
float sitePanelTilt();
float sitePanelAzimuth();

// Nameplate DC capacity, kW -- SolarForecast.h's estimatedKw()/totalKwh().
float siteSystemKw();

// POSIX TZ string, from site_settings.csv's posix_timezone key -- this
// device's OWN strong override of ntpSupport.cpp's weak
// get_ntpSupport_DefaultTZ() (SiteSettings.cpp) returns this, so
// ntpSupport.cpp itself is never edited -- see that function's own
// comment for why that matters. If posix_timezone is present in the file
// but left BLANK, this returns "GMT0" (plain UTC, no DST), not an empty
// string -- see loadSiteConfig()'s own posix_timezone handling. Also THE
// ONLY timezone source
// SolarForecastFetch.cpp's Open-Meteo query needs (converts each
// returned UTC hourly timestamp to local wall-clock time itself, via
// localtime_r() -- the device's global TZ is already set to this same
// string by ntpSupport.cpp, so no separate setenv()/tzset() call is
// needed there). Until 2026-09-28 there was also a siteOpenMeteoTimezone()
// (an IANA name, from a since-removed open_meteo_timezone key) --
// SolarForecastFetch.cpp used to hand that straight to Open-Meteo's own
// timezone= parameter and trust its "local time" output, until Open-Meteo's
// archive API (used by the PC-side pipeline) was found to apply a single
// FIXED, non-DST-aware UTC offset for an entire IANA-timezone request --
// see BatterySolarMonitorUpdate/docs/data_sources.md's "Timezone handling"
// section for the full story. SolarForecastFetch.cpp now requests UTC
// directly and does its own DST-correct local conversion, the same fix
// applied there, so this second timezone representation is no longer
// needed at all -- one POSIX TZ string now serves both this device's own
// clock (ntpSupport.cpp) and its Open-Meteo forecast query.
const char *sitePosixTimezone();

// Fixed LAN endpoints for the two other local controllers this device
// talks to directly (CarChargerFetch.cpp/HotWaterFetch.cpp) -- these are
// per-site network facts, not Enphase credentials, so they live here
// rather than in EnphaseAuth.h, same reasoning as the rest of this file.
const char *siteCarChargerIp();
const char *siteHotWaterIp();

#endif
// ================= end of SiteSettings.h  file
