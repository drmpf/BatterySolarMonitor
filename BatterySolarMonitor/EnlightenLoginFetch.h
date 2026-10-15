// EnlightenLoginFetch.h  file  =================
// Keeps gatewayJwt()/enlightenSession() (EnphaseAuth.h) current, entirely
// on-device -- the quarterly PC-side pipeline only ever seeds the fixed
// IDENTITY fields (enlighten_email/enlighten_password among them) that
// make this possible in the first place; it no longer exports the live
// tokens themselves.
//
// Each attempt (see checkEnlightenLoginPoll()) first checks whether
// /auth_token.csv already holds a token:
//   - MISSING/EMPTY (no auth_token.csv yet, or a fresh device) -> builds
//     one immediately via a full login -- nothing to test.
//   - PRESENT -> TESTED first, not blindly replaced: a lightweight request
//     against each of the two APIs (checkGatewayTokenValid()/
//     checkEnlightenTokenValid(), EnlightenLoginFetch.cpp) checks whether
//     the current token is still accepted. Only a CONFIRMED 401 counts as
//     invalid and triggers a full rebuild (the same 2-step login flow
//     enphase_token.py does on the PC: email/password -> session_id +
//     _enlighten_4_session cookie -> gateway JWT); an inconclusive result
//     (connect failure, timeout, a different error) is treated the same
//     as "still valid" -- a transient network hiccup during the test
//     should never itself trigger an unnecessary relogin.
//
// First attempt fires after a short startup-uptime floor (WiFi/DNS
// settling time). If it concludes with a valid token (tested-valid, or
// freshly rebuilt), that's it for this boot -- the next attempt is next
// reboot (DailyReboot.h reboots this device once a day, a cadence
// comfortably inside the session cookie's ~14-day freshness window). If a
// rebuild is needed and FAILS, it is retried at RETRY_INTERVAL_MS
// (EnlightenLoginFetch.cpp) indefinitely until one succeeds -- there is no
// "give up until tomorrow": the still-cached gatewayJwt()/
// enlightenSession() from the last successful login remain valid and keep
// working in the meantime, so a failed rebuild costs nothing beyond
// staying on the old (still good, or at worst untested) credential a bit
// longer.
//
// The once-per-boot check above only looks at the token's state AT THAT
// MOMENT -- if it was genuinely still valid then but expires or gets
// revoked later the same day, nothing would notice on its own until
// tomorrow's reboot. notifyAuthTokenRejected() below closes that gap:
// GatewayFetch.cpp/EnlightenHistoryFetch.cpp call it whenever a REAL
// (non-validation) request hits a confirmed 401, which forces a fresh
// check on the very next tick instead of waiting for a reboot.
//
// Split into two calls for the same DNS-crash-avoidance reason
// SolarForecastFetch.h's resolveSolarForecastHost() is: DNS resolution
// (lwip) is only safe on the main thread, never on InternetPollTask.cpp's
// background task.
#ifndef ENLIGHTEN_LOGIN_FETCH_H
#define ENLIGHTEN_LOGIN_FETCH_H

#include <IPAddress.h>

// Resolves enlighten.enphaseenergy.com and entrez.enphaseenergy.com --
// main thread only. Call once from setup(), alongside
// resolveSolarForecastHost().
bool resolveEnlightenLoginHosts();

// The cached IP for enlighten.enphaseenergy.com (0.0.0.0 until
// resolveEnlightenLoginHosts() has succeeded) -- DischargeToGridFetch.cpp
// reuses this instead of resolving the exact same hostname a second time,
// since the discharge-to-grid batteryConfig API lives on this same host.
IPAddress enlightenHostIP();

// Call once from setup(), before loop() ever calls checkEnlightenLoginPoll()
// -- arms attemptTimer with the startup-uptime floor as its first delay.
void initializeEnlightenLoginFetch();

// Runs on InternetPollTask.cpp's shared background task, ticked every
// second like every other job there. See top-of-file comment for the
// test-before-rebuild / once-per-boot-on-success / retry-until-success-
// on-failure behaviour. enlightenEmail()/enlightenPassword() (EnphaseAuth.h)
// are always expected to be present -- quarterly_update.py's export
// always includes them -- so if either is somehow empty, that's logged
// once as a configuration fault (not retried -- a retry can't fix a
// missing credential) rather than treated as a normal disabled-feature
// state.
void checkEnlightenLoginPoll();

// Call this whenever a REAL (non-validation) request using gatewayJwt()/
// enlightenSession() comes back with a confirmed 401 -- GatewayFetch.cpp's
// three endpoints and EnlightenHistoryFetch.cpp's httpGetFromCandidates()
// both do. The once-per-boot check above can be fooled by a token that
// was genuinely still valid at boot-time-check but expired or got revoked
// sometime after -- since that check only runs once per boot on success,
// nothing would otherwise notice until DailyReboot.h's next reboot. This
// is the feedback path that closes that gap: it forces a fresh
// check-or-rebuild on the very next tick instead of waiting for tomorrow.
// Safe to call from multiple places / repeatedly in quick succession
// (e.g. several sub-requests in one poll cycle each hitting a 401) -- it's
// idempotent once the re-check is already pending.
void notifyAuthTokenRejected();

#endif
// ================= end of EnlightenLoginFetch.h  file
