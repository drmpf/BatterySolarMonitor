// CarChargeHistory.h  file  =================
// Persists CarChargerInstances.h's carChargingKwhToday to LittleFS, and
// keeps a rolling 5-day history of each day's final total, so both
// survive an unplanned reboot -- crash, brownout, watchdog, anything
// other than DailyReboot.cpp's own intentional midnight cleanup restart.
//
// Found necessary 2026-09-28: carChargingKwhToday is a pure ESP32-side
// RAM estimate (the charger has no real energy reading at all -- see
// CarChargerInstances.h's own comment), with nothing backing it. Unlike
// the Solar/Consumption/Battery charts (which re-backfill from Enphase's
// cloud history on every boot, so they look continuous across a reboot
// regardless of uptime), a reboot silently zeroes this one value with no
// way to tell afterwards that real charging happened earlier that day --
// confirmed as the likely explanation for a user-observed "car charge
// kWh dropped to 0" report that turned out NOT to correlate with the V2H
// trip nearby in the log (kwhToday was already 0 for the whole captured
// window, before the trip), while the Solar/Consumption chart's own
// spiky mid-morning load pattern (consistent with real car charging)
// showed no matching accumulated total by early afternoon.
//
// Two small LittleFS files:
//   /car_charge_today.csv    -- today's own (date, running kwh so far),
//                                overwritten every time kwhToday changes
//                                (at most once per ~5min poll -- see
//                                CarChargerTask.cpp). Read back at boot
//                                (well, at the first poll with a synced
//                                clock -- see loadOrResumeCarChargeToday())
//                                to RESUME today's total after an
//                                unplanned reboot, instead of losing it.
//   /car_charge_history.csv  -- up to 5 completed days' final totals,
//                                oldest first, one "date,kwh" line each.
//                                A day is appended here either gracefully
//                                (a genuine local-midnight rollover while
//                                running, CarChargerTask.cpp's own
//                                dayChanged check) or, if the device was
//                                off across that midnight, retroactively
//                                at the next boot (loadOrResumeCarChargeToday()
//                                finds /car_charge_today.csv's stored date
//                                is no longer today, and treats that
//                                stored total as the finished day it
//                                actually was).
// All four functions below do their own LittleFS file I/O with no locking
// of their own -- every one MUST be called with lockCarChargerState()
// (CarChargerTask.h) already held by the caller, the same way
// CarChargerTask.cpp's own checkCarChargerPoll() already does for the
// first three. Confirmed necessary 2026-09-28: DebugLogBuffer.cpp's first
// draft called printCarChargeHistory() AFTER unlocking, racing unprotected
// against checkCarChargerPoll()'s own writes to the same
// /car_charge_history.csv file from the background task -- fixed before
// it ever shipped, but exactly the class of bug BatteryChargeSettings.cpp's
// own mutex comment already warns about.
#ifndef CAR_CHARGE_HISTORY_H
#define CAR_CHARGE_HISTORY_H

#include <Arduino.h>

// Call once, the first time CarChargerTask.cpp's checkCarChargerPoll()
// runs with a synced clock (todayDateStr must be a real NTP-synced
// localTodayDateString() result, "YYYY-MM-DD" -- NEVER call this with the
// pre-sync boot-default date, which would wrongly look like every
// previous day rolled over at once).
//
// If /car_charge_today.csv's own stored date matches todayDateStr,
// *outKwhToday is resumed from its stored value (an unplanned reboot
// happened earlier today; today's running total survives). Otherwise
// (file missing, or its date is anything other than today) *outKwhToday
// is set to 0.0 for a fresh start, and -- only if the file DID exist with
// a different, real date -- that stored (date, kwh) pair is appended to
// the 5-day history first, since it represents a genuinely finished day
// that never got a graceful rollover before the device went down.
// Either way, /car_charge_today.csv is left holding (todayDateStr, the
// resulting *outKwhToday) by the time this returns.
void loadOrResumeCarChargeToday(const String &todayDateStr, float *outKwhToday);

// Overwrites /car_charge_today.csv with (todayDateStr, kwhToday). Cheap,
// no debounce needed -- call every time carChargingKwhToday actually
// changes; that's already at most once per ~5min poll cycle.
void saveCarChargeToday(const String &todayDateStr, float kwhToday);

// Call once, at a genuine local-midnight rollover detected while running
// (CarChargerTask.cpp's own date != lastCarChargeDate check). Appends
// (finishedDateStr, finishedKwh) to the 5-day history (dropping the
// oldest entry past 5), then writes a fresh /car_charge_today.csv for
// (newTodayDateStr, 0.0).
void rolloverCarChargeDay(const String &newTodayDateStr, const String &finishedDateStr, float finishedKwh);

// Prints the up-to-5-day history plus today's own running total as one
// multi-line block to 'out' (oldest first, today last) -- e.g.:
//   Car charge kWh, last 5 days:
//     2026-09-23: 8.1
//     2026-09-24: 12.4
//     2026-09-27: 11.2
//     2026-09-28 (today, so far): 3.7
// A day with no entry at all (device off that whole day, so no
// /car_charge_today.csv was ever banked for it -- see
// loadOrResumeCarChargeToday()) is simply absent from the list, not shown
// as a misleading 0. Used by DebugLogBuffer.cpp's debugLogBufferDumpAll(),
// per explicit instruction, to show this at the top of the Log Tail
// screen -- MUST be called with lockCarChargerState() already held (this
// function reads /car_charge_history.csv, which CarChargerTask.cpp's own
// background-task writes are also protected by that same lock).
void printCarChargeHistory(Print &out, const String &todayDateStr, float kwhToday);

#endif
// ================= end of CarChargeHistory.h  file
