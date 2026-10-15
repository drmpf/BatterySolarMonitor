// ChartAxis.h  file  =================
// Value -> screen-y conversions for the hand-drawn chart axes in this
// design. Shared by the Dwg_<Name>App.cpp files that send Solar/
// Consumption/Battery line segments, so the axis calibration is read off
// the grid JSON once, in one place, rather than copied into every file.
#ifndef CHART_AXIS_H
#define CHART_AXIS_H

// How often the client should re-request a plot drawing that's fed by
// live/refetched data (set as dwgRefresh_ms in a subclass's init() --
// pfodAI-guide.md section 6.2: nothing is ever pushed, the client only
// sees fresh sendIndexedItems() output if it periodically re-asks).
// 5 minutes -- comfortably more often than the underlying data actually
// changes (the solar forecast refetches every 30 min, SolarForecastTask.cpp)
// without polling needlessly often.
static const unsigned long CHART_REFRESH_MS = 5UL * 60UL * 1000UL;

// Screen-space y for a kW value on the Solar & Consumption chart's y-axis
// (dwgs/PowerChartGrid.pfodDwg_json, shared by the "Solar & Consumption"
// chart and "Tomorrow's Solar and Avg Consumption", which reuses the same
// grid): gridlines at y=110/89.75/69.5/49.25/29 are 0/2/4/6/8 kW, evenly
// spaced -- 10.125 screen units per kW.
inline float screenYForKw(float kw) {
  const float Y_AT_ZERO_KW = 110.0;
  const float UNITS_PER_KW = 10.125;
  return Y_AT_ZERO_KW - kw * UNITS_PER_KW;
}

// Screen-space y for a battery-charge percentage on the Battery Charge (%)
// chart's y-axis (dwgs/BatteryChartGrid.pfodDwg_json): labelled gridlines
// at y=14/32/50/68/86 are 100/80/60/40/20%, evenly spaced -- 0.9 screen
// units per percentage point. (The chart's own drawn baseline sits at
// y=95, not the y=104 this scale would put 0% at -- the grid simply isn't
// drawn all the way down to 0%, matching the original curve's own lowest
// value of 10% at y=95.)
inline float screenYForPercent(float percent) {
  const float Y_AT_ZERO_PERCENT = 104.0;
  const float UNITS_PER_PERCENT = 0.9;
  return Y_AT_ZERO_PERCENT - percent * UNITS_PER_PERCENT;
}

#endif
// ================= end of ChartAxis.h  file
