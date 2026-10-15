// ChargeFromGridState.h  file  =================
// Holds the latest polled Enphase "charge battery from grid to %"
// (chargeFromGrid) status -- the shared state HotDayTask.cpp's own status
// poll publishes and its resend-on-mismatch check reads. Added
// 2026-10-13, per explicit instruction, as the charge-from-grid
// counterpart of DischargeToGridState.h, but deliberately simpler: unlike
// discharge-to-grid, there is no separate user-facing enable/disable
// button/latch for this feature -- whether it SHOULD be on is entirely
// HotDayState.h's hotDayActive() (HotDayTask.cpp owns reconciling the two).
// This file is pure polled-status storage, RAM only, not persisted --
// after a reboot these just read as their "nothing polled yet" defaults
// until the first poll after that reboot completes, same convention as
// DischargeToGridState.h's own dtgPolledEnabled/dtgScheduleStartMinutes.
#ifndef CHARGE_FROM_GRID_STATE_H
#define CHARGE_FROM_GRID_STATE_H

// The most recently polled top-level chargeFromGrid flag straight from
// Enphase (ChargeFromGridFetch.cpp's fetchChargeFromGridStatus()).
// Defaults to false until the first successful poll.
bool cfgPolledEnabled();

// The charge-from-grid schedule's own configured window, straight from
// Enphase, as minutes-since-midnight (e.g. 13:01 -> 781, 14:00 -> 840) --
// -1 until the first successful poll. HotDayTask.cpp's checkHotDayLogic()
// adds 2 minutes to cfgScheduleEndMinutes() to get the daily decision
// trigger time ("2mins after the charge from grid schedule ends"), per
// explicit instruction -- read dynamically rather than hardcoded, so it
// tracks whatever window is actually configured on Enphase's own
// schedule-edit page, the same way DischargeToGridState.h's
// dtgScheduleStartMinutes()/dtgScheduleEndMinutes() already do for
// discharge-to-grid. BatteryProjection.cpp (§8) uses the whole window for
// its own charge-from-grid override, added 2026-10-13 alongside this pair.
int cfgScheduleStartMinutes();
int cfgScheduleEndMinutes();

// The cfg schedule's own configured target SOC% (Enphase's "limit"
// field -- the same % its own schedule-edit page on
// enlighten.enphaseenergy.com shows/lets you set) -- the % the battery is
// charged UP TO by the window's end, the mirror-image of
// DischargeToGridState.h's dtgScheduleEndSoc() (charged DOWN to). -1 until
// the first successful poll. BatteryProjection.cpp (§8) projects up to
// this % by the window's end instead of a hardcoded value, same shape as
// the discharge-to-grid override.
float cfgScheduleTargetSoc();

// Updates cfgPolledEnabled()/cfgScheduleStartMinutes()/cfgScheduleEndMinutes()/
// cfgScheduleTargetSoc() together from one poll result (HotDayTask.cpp,
// after a successful fetchChargeFromGridStatus()). Unlike
// DischargeToGridState.h's setDtgPolledStatus(), this never forces any
// latch -- there is none to force; see this file's own top comment.
void setCfgPolledStatus(bool enabled, int startMinutes, int endMinutes, float targetSoc);

// Optimistically updates cfgPolledEnabled() alone, straight after
// HotDayTask.cpp issues a setChargeFromGridEnabled() call -- same
// "assume success, let the next real poll correct it either way"
// convention as DischargeToGridState.h's setDtgPolledEnabledAssumed(),
// same reasoning (the GET/POST/PUT chain can take several seconds; no
// reason to leave any display reading the stale value for that whole
// round trip when the outcome is being assumed anyway). Does NOT touch
// cfgScheduleEndMinutes() (this action never changes the schedule
// window, only whether it's acted on).
void setCfgPolledEnabledAssumed(bool enabled);

#endif
// ================= end of ChargeFromGridState.h  file
