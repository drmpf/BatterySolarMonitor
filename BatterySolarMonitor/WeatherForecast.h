// WeatherForecast.h  file  =================
// Hand-written model class (see pfodAI-guide.md section 6, "The shape of
// the logic - model, view, controller"). Holds one day's forecast max/min
// temperature and a single rainfall estimate, as retrieved from
// Open-Meteo's daily forecast (SolarForecastFetch.cpp's own fetch already
// pulls hourly GTI from the same host/request; the daily=
// temperature_2m_max,temperature_2m_min,precipitation_sum parameters ride
// along on that one request -- see that file). Until 2026-09-11 this data
// came from a separate fetch to the BOM weather API, which returned a
// rainfall min/max RANGE (a probabilistic quantile spread) rather than a
// single figure; Open-Meteo's precipitation_sum is a single deterministic
// model figure instead, so there's no range to show any more.
//
// Deliberately separate from SolarForecast (SolarForecast.h): this is one
// value per day, not one per hour, even though both now come from the same
// fetch/host.
#ifndef WEATHER_FORECAST_H
#define WEATHER_FORECAST_H

// Holds one day's forecast temp max/min (deg C) and rainfall (mm). One
// instance per day -- see WeatherForecastInstances.h for today's and
// tomorrow's.
//
// tempMinC/rainMm each have their OWN received flag, separate from
// 'received' (which only means temp_max -- and so the day's entry itself --
// was found): kept from the BOM era, where temp_min could be genuinely
// absent for TODAY once today's overnight low had already passed while
// temp_max (still ahead in the day) stayed present. Open-Meteo's daily
// temperature_2m_max/temperature_2m_min are normally both present or both
// absent together, but the separate flags cost nothing to keep and stay
// correct either way.
class WeatherForecast {
  public:
    float tempMaxC = 0.0;
    float tempMinC = 0.0;
    float rainMm = 0.0;

    // True once temp_max (and so this day's forecast entry itself) has
    // been received -- distinguishes "no forecast yet" from a genuine
    // 0.0 reading, same convention as HourlyGti::received in
    // SolarForecast.h.
    bool received = false;
    bool tempMinReceived = false;
    bool rainReceived = false;

    // Discards any received values, ready to be refilled by a fresh fetch.
    void clear() {
      tempMaxC = 0.0;
      tempMinC = 0.0;
      rainMm = 0.0;
      received = false;
      tempMinReceived = false;
      rainReceived = false;
    }
};

#endif
// ================= end of WeatherForecast.h  file
