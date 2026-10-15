// CarChargerInstances.h  file  =================
#ifndef CAR_CHARGER_INSTANCES_H
#define CAR_CHARGER_INSTANCES_H

#include "CarChargerFetch.h" // CarChargerState

// Most recent car charger reading -- written by CarChargerTask.cpp's
// background poll, read by Dwg_StatusApp.cpp on the main thread. Always
// access under lockCarChargerState()/unlockCarChargerState()
// (CarChargerTask.h) -- same pattern as GatewayInstances.h/GatewayPollTask.h.
extern CarChargerState latestCarChargerState;

// True once the most recent poll succeeded -- false before the first poll,
// and after any poll that failed (latestCarChargerState then still holds
// whatever the last SUCCESSFUL poll saw, stale but not garbage).
extern bool carChargerReachable;

// ESP32-side ESTIMATE of today's car charging energy, in kWh -- there is
// no real energy reading from the charger (its /api/state has no kWh
// field at all), so this is computed exactly the way
// dailyPrediction/plot_today_solar.py's own car_charging_kwh_today() does:
// a fixed assumed load (CAR_CHARGE_W, CarChargerTask.cpp) times how long
// the charger was seen actually drawing power, summed poll by poll. Reset
// to 0 at each local midnight -- see CarChargerTask.cpp. Persisted to
// LittleFS (CarChargeHistory.h, added 2026-09-28) so it survives an
// unplanned reboot instead of just resetting to 0 -- this in-RAM value
// alone has nothing else backing it, unlike the Solar/Consumption/Battery
// charts, which re-backfill from Enphase's own cloud history on every
// boot regardless of this device's own uptime.
extern float carChargingKwhToday;

// Snapshot of carAutoStarted() (LoadControlState.h) -- published by
// LoadControlTask.cpp's checkLoadControlPoll() under the SAME lock as the
// fields above, once per control cycle. Exists so the display
// (Dwg_StatusApp.cpp, LoadStatusFormat.cpp) can show who owns the car
// charger's current on/off state without calling LoadControlState.h's
// unguarded plain-static accessors directly from the main thread (they're
// only ever safe to call from LoadControlTask.cpp's own background task).
extern bool carChargerAutoStarted;

// Snapshot of carNoAcCooldownTimer.remaining()>0 (LoadControlTask.cpp),
// published the same way and for the same reason as carChargerAutoStarted
// above (added 2026-09-11, per explicit correction). carAutoStarted is
// already false throughout this 15-min cooldown (carCheckStop()'s own
// no-240VAC-stop branch clears it the moment it stops the charger, since
// auto-control no longer "owns" a load it isn't actively managing) -- but
// the display still needs to say "Auto off -- Fully charged or unplugged"
// for the WHOLE cooldown window, not just the one instant of the stop
// itself, since that's still the most recent (and only) explanation for
// why the charger is off. Without this, formatCarState() has no way to
// tell "just switched off by auto-control, cooling down" apart from
// "genuinely never charged today at all" -- both look identical as raw
// chargerOn=false/ac240vActive=false/autoStarted=false.
extern bool carInNoAcCooldown;

// Snapshot of carV2hRecoveryPending() (LoadControlState.h -- LittleFS-
// persisted, unlike carInNoAcCooldown's own RAM-only timer), published
// the same way and for the same reason as carInNoAcCooldown above (added
// 2026-09-11, per explicit instruction) -- see LoadControlTask.cpp's
// carCheckStart() (V2H section) for the full trip-recovery sequence this
// drives: when a V2H trip is detected (v2hActive && !ac240vActive), V2H
// is toggled OFF so a later cycle can try turning it back on -- which
// means v2hActive itself reads false while this is true. Without this
// flag, the display would fall back to a plain "off"-style state that
// hides what's actually happening; with it, formatCarState() keeps
// showing "V2H tripped off" throughout the whole recovery attempt.
extern bool carInV2hRecovery;

#endif
// ================= end of CarChargerInstances.h  file
