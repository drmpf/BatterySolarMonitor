// DischargeToGridState.h  file  =================
// Holds the "Auto Battery Charge/Discharge" main-menu button's own
// enable/disable latch, plus the latest polled Enphase discharge-to-grid
// status (its own dtgControl.enabled flag and DTG schedule window) -- the
// shared state pfodMainMenuApp.cpp's button/status-label text and
// DischargeToGridTask.cpp's poll both need.
//
// Two different lifetimes:
//   - autoDischargeEnabled -- PERSISTED to LittleFS (mirrors
//     LoadControlState.cpp's pattern), and -- per explicit instruction,
//     changed 2026-10-13 -- genuinely persists across every reboot,
//     including this device's own daily midnight one: NOT reset/forced
//     to any particular value at boot any more. Now the overall "Auto
//     Battery Charge/Discharge" master switch: also gates
//     HotDayTask.cpp's own Hot Day decision (checkHotDayLogic()), not
//     just DischargeToGridTask.cpp's discharge decision. Changed by the
//     main menu button (pfodMainMenuApp.cpp's
//     onAutoBatteryChargeDisPressed()) -- and ONLY by that button; per
//     explicit instruction, Enphase reporting discharge as currently
//     enabled no longer forces this latch back to true (setDtgPolledStatus()
//     below used to do this; removed 2026-10-13 -- "if disabled, just
//     report discharge to grid and charge from grid settings, but do not
//     automatically re-enable"). DischargeToGridTask.cpp's own
//     runStatusPoll() still actively resends a disable while this is
//     false and Enphase reports it on -- that's "report and keep
//     correcting toward off", not "re-enable the switch", and is
//     unaffected by this change.
//   - dtgPolledEnabled / dtgScheduleStartMinutes / dtgScheduleEndMinutes /
//     dtgScheduleEndSoc -- RAM only, NOT persisted. Entirely recomputed
//     every ~5min poll (DischargeToGridTask.cpp); after a reboot these
//     just read as their "nothing polled yet" defaults
//     (false / -1 / -1 / -1) until the first poll after that reboot
//     completes.
//
// The actual decision of WHEN Enphase's own discharge-to-grid setting
// should be turned on is separate logic (DischargeToGridTask.cpp's
// checkDischargeToGridLogic(), on InternetPollTask.cpp's shared background
// task) -- this file only stores current state for display and for the
// button's own latch, it doesn't decide anything itself.
#ifndef DISCHARGE_TO_GRID_STATE_H
#define DISCHARGE_TO_GRID_STATE_H

#include <Arduino.h> // String

// True = the overall "Auto Battery Charge/Discharge" master switch is
// enabled (also gates HotDayTask.cpp's own Hot Day decision, not just
// discharge-to-grid); false = disabled entirely. Loads
// /discharge_to_grid_state.txt on first call (idempotent after that) --
// defaults to true on a genuinely first boot (no file yet), and is
// otherwise exactly whatever was last persisted, with NO boot-time
// override (changed 2026-10-13, per explicit instruction -- see this
// file's own top comment).
bool autoDischargeEnabled();

// Sets the button's own enable/disable latch and persists it -- call from
// onAutoBatteryChargeDisPressed() only. No-op (no LittleFS write) if the
// value isn't actually changing.
void setAutoDischargeEnabled(bool enabled);

// The most recently polled dtgControl.enabled flag straight from Enphase
// (DischargeToGridFetch.cpp's fetchDischargeToGridStatus()) -- NOT the
// same thing as autoDischargeEnabled() above: this is what Enphase is
// actually doing right now; that one is the button's own allow/disallow
// latch. Defaults to false until the first successful poll.
bool dtgPolledEnabled();

// The current DTG schedule window straight from Enphase, as minutes-
// since-midnight (e.g. 20:01 -> 1201) -- -1 until the first successful
// poll. Raw minutes, not a display string, so BatteryProjection.cpp (§15)
// can align the window to its own 15-min plot grid without re-parsing a
// formatted time; pfodMainMenuApp.cpp's status label (updateBatteryStatusLabel())
// calls DischargeToGridFetch.h's formatMinutesOfDay() on these for display.
int dtgScheduleStartMinutes();
int dtgScheduleEndMinutes();

// The DTG schedule's own configured end-of-window SOC% (Enphase's
// "limit" field -- the same % its own schedule edit page on
// enlighten.enphaseenergy.com shows/lets you set) -- -1 until the first
// successful poll. BatteryProjection.cpp (§15) projects down to this %
// by the window's end instead of a hardcoded value.
float dtgScheduleEndSoc();

// Updates dtgPolledEnabled/dtgScheduleStartMinutes/dtgScheduleEndMinutes/
// dtgScheduleEndSoc together from one poll result (DischargeToGridTask.cpp,
// after a successful fetchDischargeToGridStatus()). Never touches
// autoDischargeEnabled() -- until 2026-10-13 this forced it back to true
// whenever Enphase reported discharge as genuinely on, even with the
// switch currently disabled; removed per explicit instruction ("if
// disabled, just report discharge to grid and charge from grid settings,
// but do not automatically re-enable"). DischargeToGridTask.cpp's own
// runStatusPoll() still actively resends a disable in that situation --
// this function only ever updates the DISPLAY/poll-result fields, it
// never changes the switch itself.
void setDtgPolledStatus(bool enabled, int startMinutes, int endMinutes, float endSoc);

// Optimistically updates dtgPolledEnabled() alone, straight after
// DischargeToGridTask.cpp issues a setDischargeToGridEnabled() call --
// every one of them: the boot/button one-shot disable
// (checkDischargeToGridPoll()), the status poll's own resend-on-mismatch
// (runStatusPoll()), and the auto-discharge decision
// (checkDischargeToGridLogic()). Per explicit instruction: the button
// text/BatteryProjection's DTG override should reflect each action
// immediately, assumed to have succeeded, rather than sitting stale
// ("will" instead of "can"/no override after a disable, or vice versa
// after an enable) for up to ~5min until the next real status poll
// happens to run -- confirmed as a real bug in practice for BOTH
// directions (streaming_2026-10-08T03-13-54-188Z.txt/
// streaming_2026-10-08T03-17-52-546Z.txt for enable;
// streaming_2026-10-08T03-36-12-135Z.txt for disable). Does NOT touch
// dtgScheduleStartMinutes/dtgScheduleEndMinutes/dtgScheduleEndSoc (none
// of these actions change the schedule window, only whether it's acted
// on) or autoDischargeEnabled() (every caller either already knows the
// latch is in the right state, or is the one-shot disable path, which
// owns the latch separately via setAutoDischargeEnabled()). If the
// assumption turns out wrong (the call silently failed, or Enphase's own
// asynchronous apply hasn't landed yet), the next real status poll's own
// setDtgPolledStatus() call unconditionally overwrites this guess with
// Enphase's actual state regardless -- same "assume success, let the
// next real poll correct it either way" convention this whole feature
// already relies on throughout.
void setDtgPolledEnabledAssumed(bool enabled);

#endif
// ================= end of DischargeToGridState.h  file
