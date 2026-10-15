// BatteryProjection.h  file  =================
// Call every loop(). Periodically projects battery % forward from "now"
// to end-of-day (10pm), using solarData's forecast values and
// consumptionData's average/placeholder values for each future 15-min
// slot -- the same net-energy-balance approach as
// dailyPrediction/plot_today_solar.py's project_soc_afternoon()/
// project_soc_continuation(): net_kw = solar_kw - consume_kw charges the
// battery when positive (capped at the charger's max rate, tapered near
// full) and discharges it when negative, converted to a %-of-capacity
// step each 15 minutes. Writes the result into batteryChargeData
// (PlotDataInstances.h), predicted-coloured (BLACK, matching
// Dwg_BatteryPlot1App.cpp's convention).
//
// consumptionData's future slots are today's dummy/placeholder daily-load
// shape (no real consumption forecast exists yet) -- this is the same
// role plot_today_solar.py's own avg_profile parameter plays there (an
// average daily shape, not a live forecast), so using it here is
// consistent with that script's own approach, not a downgrade from it.
#ifndef BATTERY_PROJECTION_H
#define BATTERY_PROJECTION_H

void checkBatteryProjection();

// Call from DischargeToGridTask.cpp's own background-task tick, right
// after any setDtgPolledEnabledAssumed() call (DischargeToGridState.h) --
// forces checkBatteryProjection()'s own ~1min throttle to fire on its
// very next call instead of leaving the SOC plot's DTG override showing
// a stale guess for up to another ~1min after an optimistic
// enable/disable change. Safe to call from a different task than
// checkBatteryProjection() itself runs on -- mutex-protected.
void requestBatteryProjectionRecompute();

#endif
// ================= end of BatteryProjection.h  file
