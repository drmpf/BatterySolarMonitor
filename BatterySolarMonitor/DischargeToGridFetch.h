// DischargeToGridFetch.h  file  =================
// Talks to the SAME Enphase Enlighten cloud batteryConfig API the
// enlighten.enphaseenergy.com browser page uses for its own "Discharge to
// grid" toggle -- confirmed from HAR captures of that page (2026-10-04/07)
// and tested against the live account via
// pfodParser/test/enphase_dtg_toggle.py (same host, same 2 endpoints, same
// GET-for-token -> isValid -> PUT sequence) before being ported here.
//
// Authenticates with the SAME enlightenSession()/enlightenSystemId()
// (EnphaseAuth.h) EnlightenHistoryFetch.cpp already uses -- no separate
// login, and no separate DNS resolve either: enlightenHostIP()
// (EnlightenLoginFetch.h) is the same enlighten.enphaseenergy.com this API
// lives on, already resolved once at boot.
//
// Pure networking -- does not decide anything and does not touch
// DischargeToGridState.h itself; DischargeToGridTask.cpp owns that.
#ifndef DISCHARGE_TO_GRID_FETCH_H
#define DISCHARGE_TO_GRID_FETCH_H

#include <Arduino.h> // String

// GETs the DTG schedule window, its own configured end-of-window SOC%
// (the "limit" field -- the same % the schedule's own edit page on
// enlighten.enphaseenergy.com shows and lets you set), and the current
// dtgControl.enabled flag. On success, fills enabledOut/startMinutesOut/
// endMinutesOut (the window as minutes-since-midnight, e.g. 20:01 -> 1201
// -- BatteryProjection.cpp (§15) needs the raw minutes to align the
// window to its own 15-min plot grid; use formatMinutesOfDay() below to
// display it)/endSocOut and returns true. Leaves all four untouched and
// returns false on any network/parse failure, or if no DTG schedule is
// configured at all -- never guesses a window, limit, or state it
// couldn't actually read.
bool fetchDischargeToGridStatus(bool &enabledOut, int &startMinutesOut, int &endMinutesOut, float &endSocOut);

// Formats a minutes-since-midnight value "8:01pm" style, matching the
// Enlighten web page's own display -- used by pfodMainMenuApp.cpp's
// button text. Returns "?" for a negative input (DischargeToGridState.h's
// "nothing polled yet" sentinel) rather than guessing a time.
String formatMinutesOfDay(int minutesSinceMidnight);

// Runs the GET-schedules (for a fresh X-Csrf-Token) -> POST isValid ->
// PUT batterySettings sequence to set Enphase's own dtgControl.enabled to
// `enabled`. Returns true only once the PUT itself comes back with
// {"message":"success"} -- the site then applies the change asynchronously
// (confirmed by hand: ~5-20s after a successful PUT, see
// pfodParser/test/enphase_dtg_toggle.py's own polling read-back), so a
// caller that needs to see the new state reflected should wait for
// DischargeToGridTask.cpp's own next status poll, not assume it's already
// live the instant this returns.
bool setDischargeToGridEnabled(bool enabled);

#endif
// ================= end of DischargeToGridFetch.h  file
