// ChargeFromGridFetch.h  file  =================
// Talks to the SAME Enphase Enlighten cloud batteryConfig API
// DischargeToGridFetch.h/.cpp uses, but for the "charge battery from grid
// to %" (chargeFromGrid) toggle instead of discharge-to-grid -- confirmed
// from a HAR capture of the enlighten.enphaseenergy.com browser page
// (2026-10-13) and tested against the live account via
// pfodParser/test/enphase_cfg_toggle.py (same host, same batteryConfig
// endpoints) before being ported here. The request/response shape differs
// from discharge-to-grid in two ways, both confirmed from that HAR and
// mirrored exactly here:
//   - the schedule type is "cfg", not "dtg", and its isValid call must
//     ALSO echo a "forceScheduleOpted" flag back from the current
//     batterySettings state (cfgControl.forceScheduleOpted) -- discharge-
//     to-grid's isValid call needs no such flag.
//   - the toggle itself is a TOP-LEVEL "chargeFromGrid" boolean in
//     batterySettings, not nested under a "cfgControl" object the way
//     discharge-to-grid nests under "dtgControl".
//
// Authenticates with the SAME enlightenSession()/enlightenSystemId()
// (EnphaseAuth.h) DischargeToGridFetch.cpp already uses -- no separate
// login, and no separate DNS resolve either.
//
// Pure networking -- does not decide anything and does not touch
// ChargeFromGridState.h/HotDayState.h itself; HotDayTask.cpp owns that.
#ifndef CHARGE_FROM_GRID_FETCH_H
#define CHARGE_FROM_GRID_FETCH_H

// GETs the charge-from-grid schedule's own configured window (the "cfg"
// schedule's startTime/endTime, as minutes-since-midnight --
// HotDayTask.cpp adds 2 minutes to endMinutesOut for its own daily
// decision trigger; BatteryProjection.cpp uses the whole window for its
// own charge-from-grid override, mirroring discharge-to-grid's), its own
// configured target SOC% (the "limit" field -- the same % Enphase's own
// schedule-edit page shows/lets you set; UNLIKE discharge-to-grid's
// "limit", which is the % the battery is discharged DOWN TO, this is the
// % it's charged UP TO), and the current top-level chargeFromGrid flag.
// On success, fills enabledOut/startMinutesOut/endMinutesOut/targetSocOut
// and returns true. Leaves all four untouched and returns false on any
// network/parse failure, an invalid/out-of-range limit, or if no cfg
// schedule is configured at all -- never guesses a window, target%, or
// state it couldn't actually read.
bool fetchChargeFromGridStatus(bool &enabledOut, int &startMinutesOut, int &endMinutesOut, float &targetSocOut);

// Runs the GET batterySettings (for a fresh X-Csrf-Token and the current
// cfgControl.forceScheduleOpted flag) -> POST isValid -> PUT batterySettings
// sequence to set Enphase's own chargeFromGrid flag to `enabled`. Returns
// true only once the PUT itself comes back with {"message":"success"} --
// the site then applies the change asynchronously, same as discharge-to-
// grid (see DischargeToGridFetch.h's own comment), so a caller that needs
// to see the new state reflected should wait for HotDayTask.cpp's own
// next status poll, not assume it's already live the instant this
// returns.
bool setChargeFromGridEnabled(bool enabled);

#endif
// ================= end of ChargeFromGridFetch.h  file
