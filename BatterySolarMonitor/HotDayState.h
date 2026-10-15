// HotDayState.h  file  =================
// Holds the "Hot Day" applied-state latch (hotDayActive) -- whether the
// HW/Car start% currently in effect are BatteryChargeSettings.h's Hot Day
// settings (hotDayHwOnPercent()/hotDayCarOnPercent()) or its ordinary
// defaults (hwSocOnPercent()/evSocOnPercent()), and whether Enphase's own
// charge-from-grid setting should currently be on. Added 2026-10-13, per
// explicit instruction, alongside the HW/EV - Battery Charge% sub-menu's
// new "Set Hot Day trigger Temperature..." controls.
//
// hotDayActive is PERSISTED to LittleFS and, unlike
// DischargeToGridState.h's autoDischargeEnabled, is NEVER forced back to
// any particular value at boot -- per explicit instruction ("This switch
// is maintained through a reboot, and in particular over the midnight
// reboot to start the next day"), it must survive exactly as it was left,
// across this device's own daily midnight reboot (DailyReboot.h), until
// HotDayTask.cpp's own 2:02pm decision (checkHotDayLogic()) or the master
// "Auto Battery" switch being disabled
// (DischargeToGridState.h's autoDischargeEnabled(), see
// pfodMainMenuApp.cpp's onAutoBatteryChargeDisPressed()) next changes it.
//
// This file only stores/exposes the applied state and the HW/Car%
// override it implies -- it does not decide WHEN to flip hotDayActive
// (HotDayTask.cpp's checkHotDayLogic(), on InternetPollTask.cpp's shared
// background task, owns that) and does not talk to Enphase itself
// (ChargeFromGridFetch.h/ChargeFromGridState.h do).
#ifndef HOT_DAY_STATE_H
#define HOT_DAY_STATE_H

// True while the Hot Day HW/Car% settings (and Enphase charge-from-grid)
// are the ones actually in effect, rather than the ordinary defaults.
// Loads /hot_day_state.txt on first call (idempotent after that) --
// defaults to false on a genuinely first boot (no file yet), and is
// otherwise whatever was last persisted, with NO boot-time override.
bool hotDayActive();

// Sets the latch and persists it. No-op (no LittleFS write) if the value
// isn't actually changing. Called from:
//   - HotDayTask.cpp's checkHotDayLogic(), at the 2:02pm decision point
//     (the normal, once-a-day path).
//   - pfodMainMenuApp.cpp's onAutoBatteryChargeDisPressed(), with `false`,
//     the moment the master "Auto Battery" switch is turned off -- per
//     explicit instruction ("switch to Auto Battery Disabled => ... switch
//     HW/car back to the default setting"), this reversion is immediate,
//     not left for the next 2:02pm.
void setHotDayActive(bool active);

// The HW/Car start%/stop% actually in effect right now -- hotDayActive()'s
// own Hot Day values (BatteryChargeSettings.h's hotDayHwOnPercent()/
// hotDayCarOnPercent()) if true, otherwise the ordinary defaults
// (hwSocOnPercent()/evSocOnPercent()). LoadControlTask.cpp's
// hwCheckStop()/hwCheckStart()/carCheckStart() call these INSTEAD of
// BatteryChargeSettings.h's own plain accessors, so the actual
// load-control gating transparently follows whichever settings are
// currently active -- none of those three call sites needs to know about
// Hot Day at all. carCheckStop() doesn't call these itself -- it only
// sees their effect secondhand, via the CarCtx.keepCharging flag
// carCheckStart() already computed from them earlier in the same cycle.
int effectiveHwSocOnPercent();
int effectiveHwSocOffPercent();
int effectiveEvSocOnPercent();
int effectiveEvSocOffPercent();

// Compares tomorrow's forecast max temp (WeatherForecastInstances.h's
// tomorrowWeatherForecast, under SolarForecastTask.h's own
// lockSolarForecast()/unlockSolarForecast() -- the same lock that forecast
// is always read under) against BatteryChargeSettings.h's
// tomorrowsMaxTempTriggerC(). Returns false (isHotOut left untouched) if
// the forecast hasn't actually arrived yet -- never guesses hot/not-hot
// from a not-yet-received forecast. On true, isHotOut is set to whether
// tomorrow's forecast max temp is >= the trigger.
//
// Called from TWO places, per explicit instruction's own "the test for hot
// day tomorrow happens throughout the day from midnight" -- both re-test
// fresh every call, nothing here is sticky/cached:
//   - DischargeToGridTask.cpp's checkDischargeToGridLogic(), on every
//     logic tick: "if forecast max temp tomorrow is >= setting then
//     disable discharge to grid" -- continuous, immediate, independent of
//     the 2:02pm gate below.
//   - HotDayTask.cpp's checkHotDayLogic(), but ONLY once per day, gated to
//     the 2:02pm decision point -- "the actual change in the hw/car
//     settings and the charge from grid only happens at 2:02pm".
bool readHotDayForecast(bool &isHotOut);

#endif
// ================= end of HotDayState.h  file
