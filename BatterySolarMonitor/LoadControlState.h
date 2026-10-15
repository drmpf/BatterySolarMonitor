// LoadControlState.h  file  =================
// Persists the two "did WE start this load" ownership flags to LittleFS,
// so an unscheduled reboot (WiFi-drop watchdog reset, brownout, crash --
// not just the scheduled midnight one, see DailyReboot.h) doesn't lose
// track of a still-running load and leave it running unsupervised.
// Mirrors dailyPrediction/plot_today_solar.py's own
// car_charger_control.json/hw_control.json persistence (there, the fields
// are named hta_started_charging/hta_started_hw -- "hta" being Python's
// own abbreviation for "started by the HTA automation dashboard"; renamed
// to AutoStarted here since there's no separate HTA process on the ESP32,
// this control task IS the automation) -- see the plan in memory
// (project_car_hw_load_control_plan.md) for why this needed to be
// LittleFS and not in-memory-only.
//
// A third flag, carV2hRecoveryPending (added 2026-09-11, per explicit
// instruction), joined these two for the same underlying reason -- see its
// own comment below. It was originally RAM-only (LoadControlTask.cpp's own
// carV2hRecoveryPending static, matching carNoAcCooldownTimer's own
// precedent), but per explicit instruction is now persisted too: unlike
// the cooldown timer (a short transient wait with no real consequence if
// lost -- see below), losing track of an in-progress V2H recovery attempt
// across an unscheduled reboot means the device would forget it just
// toggled V2H off and could leave it off indefinitely rather than
// resuming the recovery sequence.
//
// Deliberately does NOT persist no_ac_until the way Python does -- that's
// a short transient timer (a 15min no-240VAC cooldown,
// LoadControlTask.cpp's carNoAcCooldownTimer) held in RAM via pfodDelay
// instead of a real timestamp. Losing it to a rare unscheduled reboot just
// means a stopped load can be retried a bit sooner than the full 15min --
// low-stakes compared to the AutoStarted/carV2hRecoveryPending flags,
// which actually decide whether this code believes it owns a load or is
// mid-recovery, and avoids needing NTP-synced epoch timestamps for
// something this narrow. (Python's OTHER transient window, started_at's
// 10s startup grace, has no ESP32 equivalent at all -- removed 2026-09-10
// as unreachable dead code; see LoadControlTask.cpp's top-of-file comment
// for why.)
#ifndef LOAD_CONTROL_STATE_H
#define LOAD_CONTROL_STATE_H

// Reads /load_control_state.txt on first call (idempotent after that --
// cached in RAM), defaulting all three flags to false if the file doesn't
// exist yet (first boot) or fails to parse.
bool carAutoStarted();
bool hwAutoStarted();

// True from the moment a V2H trip is detected until a later cycle
// successfully toggles V2H back on -- see LoadControlTask.cpp's
// carCheckStart() (V2H section) for the full recovery sequence this
// drives, and CarChargerInstances.h's carInV2hRecovery for how the
// display (LoadStatusFormat.h/.cpp) learns about it.
bool carV2hRecoveryPending();

// Updates the in-RAM cache AND rewrites the LittleFS file -- but only if
// the value actually changed (no-op, no write, if already at that value).
// carAutoStarted/hwAutoStarted: only call on an actual start/stop
// transition (matches Python's own _car_save()/_hw_save() call sites in
// LoadControlTask.cpp), not every poll -- LittleFS write wear is the
// reason, same reasoning the plan in memory already worked through for
// this decision. setCarV2hRecoveryPending(): per explicit instruction,
// call BEFORE sending either V2H toggle (off, when a trip is first
// detected, or on, when a recovery attempt is made) -- so the persisted
// value always reflects what this code is ABOUT to do, not just what it
// already did, in case a crash/reboot lands between the two.
void setCarAutoStarted(bool started);
void setHwAutoStarted(bool started);
void setCarV2hRecoveryPending(bool pending);

#endif
// ================= end of LoadControlState.h  file
