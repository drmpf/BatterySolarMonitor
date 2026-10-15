// Dwg_StatusApp.cpp  file =================
// Hand-written subclass of the generated Dwg_Status -- see
// pfodAI-guide.md section 6, "The shape of the logic - model, view,
// controller". Wires idx_datetime to the real NTP-derived local
// date/time (ntpSupport.h), idx_battery to the ACTUAL live gateway
// battery reading (latestBatterySocPercent, GatewayInstances.h -- fed
// directly by GatewayPollTask.cpp's background poll; deliberately NOT
// read from batteryChargeData/PlotData.h, which also holds historical
// and BatteryProjection.cpp-projected points not appropriate for "what is
// the battery doing right now"), and idx_tempToday/idx_rainToday/
// idx_tempTomorrow/idx_rainTomorrow to the real Open-Meteo daily forecast
// (todayWeatherForecast/tomorrowWeatherForecast, WeatherForecastInstances.h
// -- fed by SolarForecastTask.h's background poll, the same one that
// fetches the hourly solar forecast, see that file; blank until the first
// fetch succeeds), idx_usage to the real last-5-days average/max
// daily consumption (avgConsumptionProfile.avgDailyKwh/maxDailyKwh,
// AvgConsumptionInstances.h -- fed by AvgConsumptionTask.h's background
// fetch batch, the same one that builds consumptionData's forecast; blank
// until at least one day succeeds), and idx_solarToday/idx_solarTomorrow
// to solarData's/tomorrowSolarData's own totalKwh() (PlotData.h,
// PlotDataInstances.h) shown as a "(+/-X.XkWh)" excess/shortfall against
// avgDailyKwh -- solar blends ACTUAL readings so far with PREDICTED
// values for the rest of the day for today (same PlotData instance the
// Solar & Consumption chart itself reads, so it updates live as real data
// arrives), purely forecast for tomorrow (tomorrowSolarData); the
// comparison baseline is always the flat 5-day average (avgDailyKwh for
// today, tomorrowAvgConsumptionData.totalKwh() -- which amounts to the
// same average -- for tomorrow), deliberately NEVER today's own
// actual-so-far consumption (fixed 2026-09-22 -- see
// formatSolarVsConsumption()'s own comment for why blending that in
// there was a real, confirmed-in-practice bug); blank until
// solarForecastReady()/haveUsage (today)/avgConsumptionForecastReady()
// (tomorrow), and idx_hw/idx_car
// to HotWaterTask.cpp's/CarChargerTask.cpp's own background polls
// (HotWaterInstances.h/CarChargerInstances.h) -- "--" until the first poll
// succeeds. carChargingKwhToday is an ESP32-side ESTIMATE (fixed assumed
// wattage x time spent actually charging, matching
// dailyPrediction/plot_today_solar.py's own car_charging_kwh_today() --
// there is no real energy reading from the charger); the HW kWh figure is
// the controller's own reported running daily total, read directly, not
// accumulated by this code.
// Re-generating from the pfodWeb Designer overwrites Dwg_Status.h/.cpp
// and leaves this file alone.
//
// No init() override needed here: this is an insertDwg'd child of
// Dwg_Home -- an inserted dwg's own dwgRefresh_ms is discarded on the
// wire and does nothing (pfodAI-guide.md section 6, "Refresh is the
// trap"), and Dwg_Home's own dwgRefresh_ms (the one that would otherwise
// drive this) is 0 too, as of the 2026-10-08 Designer regeneration. This
// drawing still auto-refreshes every ~30s regardless, via a separate
// mechanism -- see Dwg_HomeApp.cpp's own comment (sendMainMenuUpdate()'s
// `|+` lines, tied to the main menu's own refresh_ms).
#include "Dwg_Status.h"
#include "ntpSupport.h"
#include "GatewayInstances.h"
#include "GatewayPollTask.h"
#include "BatteryConstants.h"
#include "WeatherForecastInstances.h"
#include "SolarForecastTask.h"
#include "AvgConsumptionInstances.h"
#include "AvgConsumptionTask.h"
#include "AvgConsumptionRefresh.h"
#include "PlotDataInstances.h"
#include "SolarForecastRefresh.h"
#include "CarChargerInstances.h"
#include "CarChargerTask.h"
#include "HotWaterInstances.h"
#include "HotWaterTask.h"
#include "LoadStatusFormat.h"

class MyStatus : public Dwg_Status {
  protected:
    virtual void sendIndexedItems();
};

// Blank unless BOTH tempMaxC and tempMinC have actually been received --
// matches dailyPrediction/plot_today_solar.py's own display logic
// (only ever shows "Today: X/Y" when both are present). Kept from the BOM
// era, where temp_min could be omitted for TODAY specifically once today's
// overnight low had already passed while temp_max (still ahead) stayed
// present -- see WeatherForecast.h -- so w.received alone (temp_max only)
// is NOT enough to show this pair; showing "32/0" for a genuinely-missing
// min would be actively wrong, not just an early blank.
static String formatTemp(const WeatherForecast &w) {
  if (!w.received || !w.tempMinReceived) {
    return "";
  }
  return String((int)(w.tempMaxC + 0.5)) + "℃/" + String((int)(w.tempMinC + 0.5)) + "℃";
}

// "---" for a genuine 0mm forecast (no rain expected) rather than the
// literal "~0.0mm". Blank unless rainMm has actually been received. The
// leading "~" flags this as Open-Meteo's single deterministic
// precipitation_sum estimate (2026-09-11), not a measured/guaranteed
// amount -- there is no min/max range any more (that was BOM's own
// probabilistic quantile spread, which Open-Meteo doesn't provide -- see
// WeatherForecast.h).
static String formatRain(const WeatherForecast &w) {
  if (!w.received || !w.rainReceived) {
    return "";
  }
  if (w.rainMm == 0.0) {
    return "---";
  }
  return "~" + String(w.rainMm, 1) + "mm";
}

// "X.XkWh" alone until 'consumptionKwh' is also available, then
// "X.XkWh (+/-Y.YkWh)" -- the excess/shortfall of 'solarKwh' over it, same
// idea as dailyPrediction/plot_today_solar.py's own predicted-vs-
// consumption comparison. String(float) already prints a leading '-' for a
// negative excess, so only the '+' for a non-negative one needs adding here.
//
// 'consumptionKwh' is a BASELINE (the flat 5-day average), not a live
// figure -- callers must NOT pass today's own actual-so-far consumption
// (e.g. consumptionData.totalKwh()) here, even for the "today" call where
// 'solarKwh' itself legitimately IS today's actual-so-far-plus-forecast.
// Confirmed as a real bug in practice (2026-09-22, plots.jpg): passing
// the live blend made today's comparison drift low by exactly however
// much today's actual usage so far undershot the average (e.g. "10.0kWh
// (-3.5kWh)" shown against a "24.4kWh avg" Usage line that implies
// -14.4kWh) -- correct only once the day is nearly over and "actual so
// far" has converged on the full day's total; badly wrong any time
// earlier in the day when the elapsed portion happens to be a low-usage
// stretch (overnight/early morning). Solar legitimately updates live
// because that side of the comparison IS meant to track today's real
// generation; consumption is the fixed yardstick it's measured against.
static String formatSolarVsConsumption(float solarKwh, bool haveConsumptionKwh, float consumptionKwh) {
  String result = String(solarKwh, 1) + "kWh";
  if (haveConsumptionKwh) {
    float excessKwh = solarKwh - consumptionKwh;
    result += excessKwh >= 0 ? " (+" : " (";
    result += String(excessKwh, 1) + "kWh)";
  }
  return result;
}

// formatCarState()/formatHwState() moved to LoadStatusFormat.h/.cpp so
// LoadControlTask.cpp can log the exact same status text after each
// control decision -- see that header for the full rules/rationale
// comments (kWh suffix always shown including 0.0, V2H-not-V2L naming,
// etc.).

void MyStatus::sendIndexedItems() {
    // The ACTUAL, current live reading -- never batteryChargeData/PlotData
    // (which also holds historical/boot-backfill points and
    // BatteryProjection.cpp's own projected-forward points; neither is
    // "the actual read status"). -1 sentinel means the last poll's
    // battery-SOC fetch failed -- see GatewayInstances.h.
    lockGatewayReading();
    float batterySoc = latestBatterySocPercent;
    float houseLoadKw = latestConsumeKw; // current whole-home load -- see formatCarState()'s V2H rows
    unlockGatewayReading();

    String batteryStr;
    if (batterySoc >= 0.0) {
      batteryStr = "Battery " + String((int)(batterySoc + 0.5)) + "% " + String(socToUsableKwh(batterySoc), 2) + "kWh";
    } else {
      batteryStr = "Battery --";
    }
    dwgsPtr->label().idx(idx_battery).color(dwgsPtr->BLACK).text(batteryStr.c_str()).fontSize(12).bold().offset(5,10).left().decimals(2).send();

    // Real state -- HotWaterTask.cpp/CarChargerTask.cpp's own 5-min
    // background polls (HotWaterInstances.h/CarChargerInstances.h).
    // carChargingKwhToday is an ESP32-side ESTIMATE (the charger has no
    // real energy reading -- see CarChargerTask.cpp); kwhToday for HW is
    // the controller's own reported running total, not accumulated here.
    lockHotWaterState();
    HotWaterState hwState = latestHotWaterState;
    bool hwReachable = hotWaterReachable;
    bool hwAutoStarted = hotWaterAutoStarted;
    unlockHotWaterState();
    String hwStr = formatHwState(hwReachable, hwState, hwAutoStarted);
    dwgsPtr->label().idx(idx_hw).color(dwgsPtr->BLACK).text(hwStr.c_str()).fontSize(12).bold().offset(5,23).left().decimals(2).send();

    lockCarChargerState();
    CarChargerState carState = latestCarChargerState;
    bool carReachable = carChargerReachable;
    float carKwhToday = carChargingKwhToday;
    bool carAutoStarted = carChargerAutoStarted;
    bool carInNoAcCooldownNow = carInNoAcCooldown;
    bool carInV2hRecoveryNow = carInV2hRecovery;
    unlockCarChargerState();
    // HW is on a separate circuit from the car charger/V2H -- see
    // v2hRelevantLoadKw()'s own comment (LoadStatusFormat.h/.cpp).
    float v2hLoadKw = v2hRelevantLoadKw(houseLoadKw, hwState);
    String carStr = formatCarState(carReachable, carState, carKwhToday, carAutoStarted, carInNoAcCooldownNow, carInV2hRecoveryNow, v2hLoadKw);
    dwgsPtr->label().idx(idx_car).color(dwgsPtr->BLACK).text(carStr.c_str()).fontSize(12).bold().offset(5,35).left().decimals(2).send();
    // Real last-5-fully-elapsed-days usage, from the SAME Enlighten fetches
    // AvgConsumptionTask.cpp already does for the consumption forecast
    // (EnlightenHistoryFetch.h's fetchEnlightenConsumptionForDate() also
    // returns each day's whole-day total kWh) -- see
    // AvgConsumptionProfile.h's avgDailyKwh/maxDailyKwh. Blank ("Usage --")
    // until at least one day has succeeded, same convention as every other
    // not-yet-available field on this screen.
    lockAvgConsumptionProfile();
    float avgDailyKwh = avgConsumptionProfile.avgDailyKwh;
    float maxDailyKwh = avgConsumptionProfile.maxDailyKwh;
    bool haveUsage = avgConsumptionProfile.hasData();
    unlockAvgConsumptionProfile();

    String usageStr;
    if (haveUsage) {
      usageStr = "Usage " + String(avgDailyKwh, 1) + "kWh avg, " + String(maxDailyKwh, 1) + "kWh max";
    } else {
      usageStr = "Usage --";
    }
    dwgsPtr->label().idx(idx_usage).color(dwgsPtr->BLACK).text(usageStr.c_str()).fontSize(10).bold().offset(5,48).left().decimals(2).send();
    // Blank (empty text) until SolarForecastTask's background poll has
    // actually fetched a value -- see WeatherForecast.h's 'received' flag.
    WeatherForecast todayWeather, tomorrowWeather;
    lockSolarForecast();
    todayWeather = todayWeatherForecast;
    tomorrowWeather = tomorrowWeatherForecast;
    unlockSolarForecast();

    String todayTempStr = formatTemp(todayWeather);
    String todayRainStr = formatRain(todayWeather);
    String tomorrowTempStr = formatTemp(tomorrowWeather);
    String tomorrowRainStr = formatRain(tomorrowWeather);

    // Solar side: solarData/tomorrowSolarData's own totalKwh() (PlotData.h).
    // Today's blends ACTUAL readings for the elapsed part of the day
    // (GREEN, written by GatewayRefresh.cpp/EnlightenHistoryRefresh.cpp)
    // with PREDICTED values for the remainder (BLACK, written by
    // SolarForecastRefresh.cpp) -- "today's real-so-far + forecast-for-
    // the-rest" -- and updates live as real readings land throughout the
    // day. Tomorrow's is purely forecast (tomorrow hasn't happened).
    // Gated on solarForecastReady() (SolarForecastRefresh.cpp paints
    // solarData/tomorrowSolarData), not solarData.numPoints or similar --
    // matches the chart's own readiness gate.
    //
    // Consumption side is the comparison BASELINE, not a live figure --
    // per explicit correction (2026-09-22): it must always be the flat
    // 5-day average (avgDailyKwh, the SAME figure the "Usage" line above
    // shows), never blended with today's actual-so-far the way solar is.
    // Confirmed as a real bug in practice: consumptionData.totalKwh() (the
    // chart's own today-blended total) undershoots badly whenever today's
    // actual usage so far has been low (e.g. overnight/early morning),
    // showing something like "10.0kWh (-3.5kWh)" against a "24.4kWh avg"
    // Usage line two lines above that implies -14.4kWh -- today's ACTUAL
    // consumption-so-far belongs on the Solar & Consumption chart (it's
    // exactly what that chart shows), not folded into this "vs average"
    // comparison too. tomorrowAvgConsumptionData.totalKwh() already
    // amounts to the same average (it's painted in full from avgKw[],
    // never blended with any "actual" -- tomorrow hasn't happened), so it
    // needs no equivalent change. Gated on haveUsage (avgConsumptionProfile.hasData(),
    // above), not avgConsumptionForecastReady() -- that gate exists only to
    // protect reads of consumptionData/tomorrowAvgConsumptionData, which
    // avgDailyKwh doesn't depend on; tomorrow's own call still needs it,
    // since tomorrowAvgConsumptionData itself is what's being read there.
    bool haveSolarForecast = solarForecastReady();
    bool haveConsumptionForecast = avgConsumptionForecastReady();
    float todaySolarKwh = solarData.totalKwh();
    float tomorrowSolarKwh = tomorrowSolarData.totalKwh();
    float todayConsumptionKwh = avgDailyKwh;
    float tomorrowConsumptionKwh = tomorrowAvgConsumptionData.totalKwh();

    String solarTodayStr = haveSolarForecast ? formatSolarVsConsumption(todaySolarKwh, haveUsage, todayConsumptionKwh) : "";
    String solarTomorrowStr = haveSolarForecast ? formatSolarVsConsumption(tomorrowSolarKwh, haveConsumptionForecast, tomorrowConsumptionKwh) : "";

/**  from Dwg_Status.cpp
    dwgsPtr->label().idx(idx_solarToday).color(dwgsPtr->BLACK).text("32.2kWh (+28kWh)").fontSize(12).bold().offset(88,77).center().decimals(2).send();
    dwgsPtr->label().idx(idx_tempToday).color(dwgsPtr->BLACK).text("20℃/22℃").fontSize(12).bold().offset(173,77).center().decimals(2).send();
    dwgsPtr->label().idx(idx_rainToday).color(dwgsPtr->BLACK).text("&#126;10mm").fontSize(12).bold().offset(228,77).center().decimals(2).send();
    dwgsPtr->label().idx(idx_solarTomorrow).color(dwgsPtr->BLACK).text("32.2kWh (+28kWh)").fontSize(12).offset(88,91).center().decimals(2).send();
    dwgsPtr->label().idx(idx_tempTomorrow).color(dwgsPtr->BLACK).text("29℃/24℃").fontSize(12).offset(173,91).center().decimals(2).send();
    dwgsPtr->label().idx(idx_rainTomorrow).color(dwgsPtr->BLACK).text("&#126;19mm\n").fontSize(12).offset(228,91).center().decimals(2).send();
  */
    dwgsPtr->label().idx(idx_solarToday).color(dwgsPtr->BLACK).text(solarTodayStr.c_str()).fontSize(12).bold().offset(88,77).center().decimals(2).send();
    dwgsPtr->label().idx(idx_tempToday).color(dwgsPtr->BLACK).text(todayTempStr.c_str()).fontSize(12).bold().offset(173,77).center().decimals(2).send();
    dwgsPtr->label().idx(idx_rainToday).color(dwgsPtr->BLACK).text(todayRainStr.c_str()).fontSize(12).bold().offset(228,77).center().decimals(2).send();
    dwgsPtr->label().idx(idx_solarTomorrow).color(dwgsPtr->BLACK).text(solarTomorrowStr.c_str()).fontSize(12).offset(88,91).center().decimals(2).send();
    dwgsPtr->label().idx(idx_tempTomorrow).color(dwgsPtr->BLACK).text(tomorrowTempStr.c_str()).fontSize(12).offset(173,91).center().decimals(2).send();
    dwgsPtr->label().idx(idx_rainTomorrow).color(dwgsPtr->BLACK).text(tomorrowRainStr.c_str()).fontSize(12).offset(228,91).center().decimals(2).send();

    // ntpSupport.h only exposes the full month name -- abbreviate to 3
    // letters here rather than editing that shared file.
    String monthAbbrev = getCurrentMonth().substring(0, 3);
    String dateTimeStr = getCurrentDayOfMonth() + " " + monthAbbrev + " " + getCurrentYear() + "  " + getCurrentTime_hhmmAM_PM();
    dwgsPtr->label().idx(idx_datetime).color(dwgsPtr->BLACK).text(dateTimeStr.c_str()).fontSize(12).bold().offset(126,10).left().decimals(2).send();
}

MyStatus myStatus;
Dwg_Status& get_dwg_Status() { return myStatus; }
// ============== end of Dwg_StatusApp.cpp  file
