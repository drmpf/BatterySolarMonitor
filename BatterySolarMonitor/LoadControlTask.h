// LoadControlTask.h  file  =================
// Ports dailyPrediction/plot_today_solar.py's car-charger/hot-water
// SOC-hysteresis auto-control (control_loads() and the 4 phases it calls)
// to the ESP32 -- see LoadControlTask.cpp's own top-of-file comment for
// the full picture, including the ENABLE_LOAD_CONTROL safety switch.
#ifndef LOAD_CONTROL_TASK_H
#define LOAD_CONTROL_TASK_H

// Runs the auto-control decision if due. Reads the already-polled state
// CarChargerTask.cpp/HotWaterTask.cpp/GatewayPollTask.cpp publish
// (CarChargerInstances.h/HotWaterInstances.h/GatewayInstances.h) rather
// than fetching its own -- no new network traffic to either device beyond
// what's already polling for display. Call repeatedly from
// InternetPollTask.cpp's shared background task.
void checkLoadControlPoll();

// Forces the NEXT checkLoadControlPoll() to run its FULL cycle (all 4
// decision phases) within about a second, instead of waiting out this
// task's own independent pollTimer.
//
// Called unconditionally by CarChargerTask.cpp's checkCarChargerPoll()
// after EVERY successful fetch (added 2026-09-19, per explicit report --
// see that function's own "Sync point" comment) -- not just after a V2H
// recovery recheck or schedule-boundary follow-up. CarChargerTask.cpp's
// own poll cadence was never phase-aligned with this file's independent
// ~5min pollTimer; a real incident (streaming_2026-09-19T04-05-33-646Z.txt)
// showed carCheckStart() making a start decision against a car.v2hActive
// reading that was nearly a full poll-interval stale, right at the
// charger's own V2H schedule boundary. Calling this on every successful
// fetch closes that gap generally, not just for the two specific cases
// this used to be restricted to.
//
// Safe to call this often: each call corresponds to genuinely fresh,
// distinct data (CarChargerTask.cpp always updates latestCarChargerState
// before calling this, and InternetPollTask.cpp's tick order guarantees
// checkLoadControlPoll() runs after checkCarChargerPoll()/checkHotWaterPoll()
// in the same tick -- see esp32_control_structure.md sec 2), and the 4
// decision phases are idempotent against repeated fresh evaluation --
// e.g. carCheckStart()'s V2H recovery retry clears carV2hRecoveryPending()
// BEFORE sending its toggle, so a second call immediately after never
// re-sends it. V2H_RECOVERY_RECHECK_MS/EARLY_RECHECK_MS's own reschedules
// remain as fallbacks purely for a persistently-unreachable charger, where
// this is never called at all.
void requestLoadControlPollNow();

#endif
// ================= end of LoadControlTask.h  file
