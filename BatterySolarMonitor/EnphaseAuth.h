// EnphaseAuth.h  file  =================
// Loads Enphase credentials from LittleFS at boot, instead of the
// hardcoded constants GatewayAuth.h/EnlightenAuth.h used to carry
// (removed 2026-09-10). Same pattern as ShadeFactorTable.h -- small CSV
// data files, so updating a credential is a data-file write, not a source
// edit + recompile + reflash.
//
// Split (2026-09-11) across TWO files, one per owner:
//   - /site_settings.csv -- IDENTITY: gateway_ip, enlighten_system_id,
//     enlighten_email, enlighten_password, gateway_serial. Fixed
//     account/hardware config that can't be derived on-device. PC-OWNED --
//     seeded by BatterySolarMonitorUpdate/py_programs/quarterly_update.py's
//     export (from auth/.env and data/enphase/_auth_token.json), copied to
//     BatterySolarMonitor/data/ alongside shade_grid.csv. Rarely changes,
//     so it's never rewritten by this device -- loadEnphaseAuth() requires
//     all 5 of these to be present. Named for growth beyond auth: since
//     2026-09-11 this same file also holds further fixed per-site fields
//     (latitude, longitude, panel tilt/azimuth, system kW, timezone, and
//     -- added later -- car_charger_ip/hot_water_ip, 8 in total) that
//     used to be scattered hardcoded constants across several other
//     files -- see SiteSettings.h, which independently loads those 8
//     keys from this identical file (each loader only cares about its
//     own keys, ignoring the other's rows; EnphaseAuth.cpp's own
//     loadSiteSettings() keeps a matching list of SiteSettings' keys
//     purely so it doesn't wrongly flag them as unexpected -- keep that
//     list in sync here if SiteSettings' own schema grows again).
//   - /auth_token.csv -- LIVE TOKENS: gateway_jwt, enlighten_session.
//     Short-lived credentials that expire and need refreshing. DEVICE-OWNED
//     -- EnlightenLoginFetch.h performs the Enlighten login itself once a
//     day (retried at intervals until it succeeds -- see that file) and
//     calls setGatewayJwt()/setEnlightenSession() + saveAuthToken() to
//     persist the result, so it survives past the current boot (this
//     device reboots itself daily -- DailyReboot.h -- so without
//     persisting here, a self-refreshed token would only last until
//     midnight). quarterly_update.py's export does NOT write this file at
//     all -- a PC-side copy would be stale within a day, and only this
//     device's own login ever needs to update it. Rewritten far more often
//     than site_settings.csv, which is exactly why it's a separate file --
//     saveAuthToken() only ever touches these 2 fields, never the 5
//     identity ones. Empty/missing on a freshly-seeded device (the file
//     may not even exist yet) is normal, not a load failure -- gatewayIp()
//     etc. calls just fail gracefully (same as any other transient fetch
//     failure) until the first successful login fills it in, typically
//     well under a minute after boot.
//
// Keep both files out of any public repo/screen-share -- gatewayJwt()
// grants local API access to the whole solar/battery/consumption system,
// and enlightenPassword() is a real account password.
#ifndef ENPHASE_AUTH_H
#define ENPHASE_AUTH_H

// Reads /site_settings.csv (required, all 5 identity keys) and
// /auth_token.csv (best-effort -- see top-of-file comment) from LittleFS
// (mounts it via initializeFS() -- ESP_PicoW_LittleFSsupport.h -- if not
// already mounted). Call once from setup(), before anything that calls the
// accessors below. Returns true only if the identity load succeeded --
// the auth-token file's presence/absence never affects the return value.
bool loadEnphaseAuth();

// Rewrites /auth_token.csv on LittleFS from the current in-RAM
// gatewayJwt()/enlightenSession() values ONLY -- never touches
// site_settings.csv or its 5 identity fields. Called by
// EnlightenLoginFetch.cpp after a successful self-refresh; no other
// caller needs this -- the accessors below are read-only everywhere else.
bool saveAuthToken();

// Local Enphase IQ Gateway (GatewayFetch.cpp) -- GatewayAuth.h's old
// GATEWAY_IP/GATEWAY_JWT. gatewayIp() is PC-seeded identity; gatewayJwt()
// is device-owned (see top-of-file comment).
const char *gatewayIp();
const char *gatewayJwt();
void setGatewayJwt(const char *jwt); // updates in-RAM only -- call saveAuthToken() to persist

// Enlighten cloud web API (EnlightenHistoryFetch.cpp -- backfills today's
// already-elapsed solar/consumption/battery-SOC history once at boot; the
// local gateway above only ever gives "right now", it has no history of
// its own) -- EnlightenAuth.h's old ENLIGHTEN_SESSION/ENLIGHTEN_SYSTEM_ID.
// A DIFFERENT credential to gatewayJwt() above: that one authenticates to
// the local gateway's own API, this one authenticates to the Enlighten
// *cloud* web API (enlighten.enphaseenergy.com) via the
// _enlighten_4_session cookie a browser gets on login. enlightenSystemId()
// is PC-seeded identity; enlightenSession() is device-owned (see
// top-of-file comment) -- this is the one EnlightenLoginFetch.h's daily
// self-refresh matters most for, since it's the one that actually expires
// on a human timescale (~14 days).
const char *enlightenSession();
const char *enlightenSystemId();
void setEnlightenSession(const char *session); // updates in-RAM only -- call saveAuthToken() to persist

// Enlighten account login credentials -- ONLY used by
// EnlightenLoginFetch.cpp to perform the login itself; nothing else reads
// these. PC-seeded identity, always expected to be non-empty
// (loadEnphaseAuth() requires them) -- there's no supported "leave the
// password off this device" configuration.
const char *enlightenEmail();
const char *enlightenPassword();

// Gateway serial number -- the "serial_num"/GATEWAY_SERIAL value
// enphase_token.py's own get_token() sends when exchanging session_id for
// a fresh JWT. ONLY used by EnlightenLoginFetch.cpp. PC-seeded identity --
// a fixed value tied to the physical gateway hardware, not a secret.
const char *gatewaySerial();

#endif
// ================= end of EnphaseAuth.h  file
