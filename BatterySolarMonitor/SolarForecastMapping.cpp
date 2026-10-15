// SolarForecastMapping.cpp  file  =================
#include "SolarForecastMapping.h"

void mapSolarForecastToPlotData(const SolarForecast &forecast, PlotData &plot, uint8_t color, int skipBeforeMinutes) {
  for (int t = PlotData::START_MINUTES; t <= PlotData::END_MINUTES; t += PlotData::STEP_MINUTES) {
    if (t < skipBeforeMinutes) {
      continue; // leave an already-elapsed (actual) point alone
    }
    int hour = t / 60;
    int minuteOfHour = t % 60;
    int nextHour = hour + 1;

    float kwAtHour = forecast.estimatedKw(hour);
    float kwAtNextHour = (nextHour < SolarForecast::HOURS_PER_DAY)
                              ? forecast.estimatedKw(nextHour)
                              : kwAtHour;

    float fraction = minuteOfHour / 60.0;
    float kw = kwAtHour + (kwAtNextHour - kwAtHour) * fraction;

    plot.setPointAtTime(t, kw, color);
  }
}
// ================= end of SolarForecastMapping.cpp  file
