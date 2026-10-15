// DischargeToGridTask.h  file  =================
// Runs on InternetPollTask.cpp's shared background task. Three jobs:
//   - a one-shot "force Enphase discharge-to-grid off" action, due
//     immediately the first time this is ever ticked (per explicit
//     instruction: "on restart always set enphase to disable ... then
//     update later as logic says") and re-armed on demand by
//     requestDischargeToGridDisableNow() (pfodMainMenuApp.cpp's button
//     handler, when the user switches the button to its disabled state).
//     NOT retried on its own -- fired once, assumed to have worked (per
//     explicit instruction), and immediately followed by
//     setDtgPolledEnabledAssumed(false) (DischargeToGridState.h) so the
//     button/BatteryProjection reflect it right away. The recurring
//     status poll just below is what actually verifies it and resends if
//     needed.
//   - a recurring ~5min status poll (DischargeToGridFetch.cpp's
//     fetchDischargeToGridStatus()) that keeps DischargeToGridState.h's
//     dtgPolledEnabled()/dtgScheduleStartMinutes()/dtgScheduleEndMinutes() current for
//     the main menu button's own text, AND is the one-shot disable
//     action's own retry mechanism: if Enphase still reports discharge on
//     while the button wants it off, the mandatory boot disable hasn't
//     been confirmed off yet, OR the auto-discharge decision below hasn't
//     positively decided to turn discharge ON (either because it hasn't
//     decided yet, or because it decided OFF), this resends the disable --
//     again just assumed to have worked (same immediate
//     setDtgPolledEnabledAssumed(false) follow-up), so it self-corrects
//     every ~5min for as long as the mismatch persists
//     (DischargeToGridTask.cpp's runStatusPoll()). The third condition
//     (auto-logic hasn't decided ON) closes a real gap found in practice:
//     once the boot disable is confirmed AND the button allows discharge,
//     nothing previously kept re-asserting "off" if the auto-logic hadn't
//     positively decided ON -- Enphase's own cloud-side schedule
//     re-engaging on its own went completely uncorrected, with no
//     "enabling discharge" decision ever logged to explain it. Skipped
//     once the schedule window has actually started today, matching
//     checkDischargeToGridLogic()'s own "ignore changes during and after
//     the discharge schedule starts" rule. The "hasn't been confirmed
//     yet" (boot) half matters separately because the button's own latch
//     (DischargeToGridState.h) is persisted and, as of 2026-10-13, no
//     longer force-reset at boot -- if it happens to be in its ordinary
//     "enabled" state (the common case) going into a reboot, checking
//     only the latch would give the mandatory boot disable no resend
//     coverage at all if its first attempt failed to land. Note
//     bootDisableUnconfirmed itself is cleared ONLY by this real poll
//     actually observing `enabled == false` -- never by the optimistic
//     setDtgPolledEnabledAssumed() calls, which affect only the DISPLAY,
//     not this safety gate (see checkDischargeToGridLogic()'s own "boot
//     safety gate" bullet below, which deliberately keeps requiring real
//     confirmation here, not an assumption).
//   - the auto-discharge DECISION itself (checkDischargeToGridLogic()
//     below) -- per explicit instruction, moved onto this same background
//     task (rather than requesting it from elsewhere) so it can call
//     setDischargeToGridEnabled() directly, same as the two jobs above,
//     with no cross-task request/apply handoff needed. Its own INPUT data
//     (tomorrow's forecast solar / average consumption totals,
//     PlotDataInstances.h) is read straight off tomorrowSolarData/
//     tomorrowAvgConsumptionData, same instances Dwg_StatusApp.cpp's own
//     status line and BatteryProjection.cpp already read -- but, unlike
//     those two (both main-thread-only callers), this read is genuinely
//     cross-thread, so it holds PlotDataInstances.h's own
//     lockTomorrowForecastData() across the readiness checks AND both
//     totalKwh() calls as one atomic snapshot -- see that lock's own
//     header comment for the full reasoning.
//
// This file itself only ever calls setDischargeToGridEnabled(false)
// directly (the boot/button-disable path) EXCEPT for
// checkDischargeToGridLogic()'s own decision, the only place
// setDischargeToGridEnabled(true) ever gets called.
#ifndef DISCHARGE_TO_GRID_TASK_H
#define DISCHARGE_TO_GRID_TASK_H

// Call repeatedly from InternetPollTask.cpp's shared background task.
void checkDischargeToGridPoll();

// Arms the one-shot disable action to run at its very next opportunity
// (the next background-task tick, not the full ~5min status-poll
// interval) -- call from onAutoBatteryChargeDisPressed() right after the
// button switches to its disabled state.
void requestDischargeToGridDisableNow();

// The auto-discharge decision -- call repeatedly from
// InternetPollTask.cpp's shared background task, same as
// checkDischargeToGridPoll() above (right after it, in practice, since
// this reads that poll's own dtgScheduleStartMinutes()). Per explicit
// instruction:
//   - runs at all only while the master "Auto Battery Charge/Discharge"
//     switch's own latch (DischargeToGridState.h's autoDischargeEnabled())
//     is enabled -- disabled, this returns immediately every call and
//     never acts itself (this file's own boot/button-disable path owns
//     turning it off).
//   - stops changing anything once "now" reaches the DTG schedule's own
//     start time, for the rest of that day -- a plain time comparison
//     re-evaluated fresh every call, not a sticky "frozen" flag, so it
//     self-corrects regardless of any button toggling in the meantime and
//     naturally resets at the next local midnight (and, in the ordinary
//     case, the next boot -- DailyReboot.h).
//   - also holds off requesting "on" until the mandatory boot disable has
//     actually been confirmed off (bootDisableUnconfirmed).
//   - Hot Day gate (added 2026-10-13): skips the normal decision entirely,
//     keeping discharge off, whenever HotDayState.h's readHotDayForecast()
//     reports tomorrow's forecast max temp >= BatteryChargeSettings.h's
//     tomorrowsMaxTempTriggerC() -- re-tested fresh on every tick,
//     independent of HotDayTask.cpp's own once-a-day 2:02pm gate.
//   - Cold Day gate (added 2026-10-14): same "keep discharge off" shape as
//     the Hot Day gate, but for the opposite extreme -- skips the normal
//     decision whenever tomorrow's forecast max temp
//     (WeatherForecastInstances.h's tomorrowWeatherForecast.tempMaxC) is
//     <= this file's own fixed COLD_DAY_MAX_TEMP_C (18 deg C).
//   - re-checks every 5 minutes, per explicit instruction (updated
//     2026-10-14): requires BOTH tomorrow's forecast excess (solar minus
//     average consumption over however many of the last 5 days have
//     actually been fetched so far -- tomorrowSolarData.totalKwh() vs
//     tomorrowAvgConsumptionData.totalKwh(), PlotDataInstances.h, read
//     directly, gated on avgConsumptionForecastReady()/solarForecastReady()
//     the same way BatteryProjection.cpp gates its own read of the same
//     two instances) to be at least this file's own fixed
//     MIN_TOMORROW_EXCESS_KWH (5kWh) AND today's car-charging energy so
//     far (CarChargerInstances.h's carChargingKwhToday, read under
//     lockCarChargerState()) to be at least this file's own fixed
//     MIN_CAR_CHARGE_TODAY_KWH (10kWh) -- enables Enphase's setting only
//     if both hold; otherwise disables it.
//   - only calls setDischargeToGridEnabled() when the decision actually
//     CHANGES from the last one applied -- never just because 5 minutes
//     passed and the same answer came out again. Not retried if that call
//     fails; the next 5min re-check only tries again if the computed
//     decision differs from what was last applied.
//   - immediately follows that call with setDtgPolledEnabledAssumed()
//     (DischargeToGridState.h) so the button text/BatteryProjection's DTG
//     override update right away, assumed to have succeeded, rather than
//     sitting stale for up to ~5min until the next real status poll --
//     see that function's own comment for why this is safe even when the
//     assumption turns out wrong.
void checkDischargeToGridLogic();

// Read access to this file's own fixed Cold Day/excess/car-charge
// decision thresholds (DischargeToGridTask.cpp's own
// COLD_DAY_MAX_TEMP_C/MIN_TOMORROW_EXCESS_KWH/MIN_CAR_CHARGE_TODAY_KWH),
// added 2026-10-14 so pfodMainMenuApp.cpp's own status label can report
// SPECIFICALLY which gate is currently withholding discharge (hot day,
// cold day, insufficient excess, insufficient car charging) using the
// exact same comparisons checkDischargeToGridLogic() itself uses above --
// rather than a second, separately-maintained copy of the same three
// magic numbers that could silently drift out of sync with this file's
// own actual decision if either copy were ever changed alone.
float coldDayMaxTempC();
float minTomorrowExcessKwh();
float minCarChargeTodayKwh();

#endif
// ================= end of DischargeToGridTask.h  file
