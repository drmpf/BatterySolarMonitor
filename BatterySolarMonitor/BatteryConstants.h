// BatteryConstants.h  file  =================
#ifndef BATTERY_CONSTANTS_H
#define BATTERY_CONSTANTS_H

// Shared battery constants -- matches dailyPrediction/plot_today_solar.py's
// BATT_USABLE_KWH/BATT_BACKUP_SOC for this specific home battery. Kept
// here once, rather than duplicated, so BatteryProjection.cpp (SOC
// forecast) and Dwg_StatusApp.cpp (usable-kWh-remaining status label)
// can't drift apart.
static const float BATT_USABLE_KWH = 8.35; // usable kWh at 100% SOC
static const float BATT_BACKUP_SOC = 10.0; // minimum SOC % (backup reserve, not discharged below)

// Converts SOC% to usable kWh remaining above the backup reserve --
// matches plot_today_solar.py's soc_to_usable_kwh().
static inline float socToUsableKwh(float socPercent) {
  float kwh = (socPercent - BATT_BACKUP_SOC) / (100.0 - BATT_BACKUP_SOC) * BATT_USABLE_KWH;
  return (kwh < 0.0) ? 0.0 : kwh;
}

#endif
// ================= end of BatteryConstants.h  file
