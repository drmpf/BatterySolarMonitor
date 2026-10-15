// SolarForecast.h  file  =================
// Hand-written model class (see pfodAI-guide.md section 6, "The shape of
// the logic - model, view, controller"). Holds one day's worth of hourly
// GTI (global tilted irradiance), as retrieved directly from the Open-Meteo
// forecast API -- see dailyPrediction/plot_today_solar.py's
// fetch_today_forecast() and dailyPrediction/docs/data_sources.md section 1
// (hourly.global_tilted_irradiance, W/m^2).
//
// Deliberately separate from PlotData (PlotData.h): PlotData is the pfod
// chart's own fixed 15-minute display grid (5am-10pm); this is the
// external source's own resolution (one value per hour, 0-23) before
// anything maps it onto a chart. Contains no pfod types, same as PlotData.
//
// estimatedKw() below applies dailyPrediction/solar_shading_model.py's
// per-sun-angle shade_factor correction (ShadeFactorTable.h, loaded from
// LittleFS at boot) once this forecast's calendar day is known (setDate())
// -- see that method's own comment for why the day matters here.
#ifndef SOLAR_FORECAST_H
#define SOLAR_FORECAST_H

#include <Arduino.h>
#include "SunPosition.h"
#include "ShadeFactorTable.h"
#include "MorningFloorTable.h"
#include "SiteSettings.h" // siteSystemKw()

// One hour's forecast GTI (+ cloud cover, needed by MorningFloorTable.h's
// clear-sky floor correction -- see that file's own header comment).
// received is false until this hour's value has actually arrived -- the
// external fetch can return fewer than 24 hours (partial response, hours
// already in the past for "today", etc), so absence has to be
// distinguishable from a genuine 0.0 reading.
struct HourlyGti {
  float gtiWm2 = 0.0;
  // 0 (full sun -- clear sky) rather than dailyPrediction's own 50%
  // "neutral" null-fallback: cloud_cover only ever feeds
  // MorningFloorTable.h's clear-sky floor as a DERATING factor (1.0 at
  // 0%, down to 0.4 at 100%), so if it's genuinely missing, assume clear
  // (derate by nothing) rather than guessing a mid-range cloud level and
  // silently shrinking the floor for no real reason.
  float cloudCoverPct = 0.0;
  bool received = false;
};

// Holds one day's hourly GTI forecast. One instance per day -- see
// SolarForecastInstances.h for today's and tomorrow's.
class SolarForecast {
  public:
    static const int HOURS_PER_DAY = 24;

    HourlyGti hours[HOURS_PER_DAY];

    // Which calendar day this forecast is for -- 0 means "not set yet".
    // estimatedKw() needs this to compute each hour's sun position (solar
    // declination depends on day-of-year; the AEST/AEDT offset depends on
    // month) for ShadeFactorTable.h's shade-factor lookup. Set via
    // setDate() below, once per fetch (SolarForecastFetch.cpp knows
    // whether it's filling today's or tomorrow's instance and can read
    // the real NTP-synced date -- this class itself stays date-agnostic
    // otherwise, same as before).
    int dayOfYear = 0; // 1-366, 0 = unknown
    int month = 0;     // 1-12, 0 = unknown

    // Discards all received values, ready to be refilled by a fresh fetch.
    void clear() {
      for (int h = 0; h < HOURS_PER_DAY; h++) {
        hours[h].gtiWm2 = 0.0;
        hours[h].cloudCoverPct = 0.0;
        hours[h].received = false;
      }
      dayOfYear = 0;
      month = 0;
    }

    // Records which calendar day this forecast belongs to -- see
    // dayOfYear/month's own comment above for why estimatedKw() needs it.
    // dayOfYearIn -- 1-366 (LocalTimeMinutes.h's localMonthAndDayOfYear())
    // monthIn     -- 1-12
    void setDate(int dayOfYearIn, int monthIn) {
      dayOfYear = dayOfYearIn;
      month = monthIn;
    }

    // Records the forecast GTI and cloud cover for one hour of the day.
    // Ignored if hour is outside 0-23.
    // hour          -- hour of day, 0-23
    // gtiWm2        -- global tilted irradiance for that hour, W/m^2
    // cloudCoverPct -- cloud cover for that hour, 0-100%
    void setHourGti(int hour, float gtiWm2, float cloudCoverPct) {
      if (hour >= 0 && hour < HOURS_PER_DAY) {
        hours[hour].gtiWm2 = gtiWm2;
        hours[hour].cloudCoverPct = cloudCoverPct;
        hours[hour].received = true;
      }
    }

    // Predicted solar output for one hour, kW: siteSystemKw() * GTI/1000 *
    // shadeFactor, then raised to MorningFloorTable.h's empirical
    // clear-sky floor if that's higher, clipped to [0, siteSystemKw()] --
    // same formula, order, and clip as
    // dailyPrediction/solar_shading_model.py's own apply_shading()
    // (shadeFactor can run above 1.0, up to 1.3, for sun positions where
    // OM's GTI underestimates -- e.g. reflection/albedo -- but actual
    // production can never exceed the panel's own physical capacity, so
    // the clip still applies after both corrections). shadeFactor comes
    // from ShadeFactorTable.h's (az_bin, el_bin) lookup for this hour's
    // sun position (SunPosition.h); the floor -- which compensates for
    // OM's GTI being systematically too low at low sun elevations, a
    // problem no multiplicative shade factor can fix -- only ever RAISES
    // the estimate, only while the sun is actually up, and is itself
    // scaled down by this hour's cloud cover (1.0x clear, down to 0.4x
    // overcast) so a cloudy morning doesn't get floored to a clear-sky
    // value. Both fall back to no correction (shadeFactor=1.0, no floor)
    // if dayOfYear/month haven't been set yet (see setDate()), same as
    // this method's own pre-correction v1 behaviour. Returns 0 if that
    // hour's GTI has not been received.
    // hour -- hour of day, 0-23
    float estimatedKw(int hour) const {
      if (hour < 0 || hour >= HOURS_PER_DAY || !hours[hour].received) {
        return 0.0;
      }
      float shadeFactor = 1.0;
      float elevationDeg = 0.0;
      bool haveSunPosition = false;
      if (dayOfYear > 0) {
        float azimuthDeg;
        sunPosition(dayOfYear, month, (float)hour, elevationDeg, azimuthDeg);
        shadeFactor = shadeFactorForSunPosition(azimuthDeg, elevationDeg);
        haveSunPosition = true;
      }
      float kw = siteSystemKw() * hours[hour].gtiWm2 / 1000.0 * shadeFactor;

      if (haveSunPosition && elevationDeg > 0.0) {
        float floorKw = morningFloorKw(month, hour);
        if (floorKw > 0.0) {
          float cloudFactor = 1.0 - hours[hour].cloudCoverPct / 100.0 * 0.6;
          if (cloudFactor < 0.4) {
            cloudFactor = 0.4;
          } else if (cloudFactor > 1.0) {
            cloudFactor = 1.0;
          }
          float floorEffective = floorKw * cloudFactor;
          if (floorEffective > kw) {
            kw = floorEffective;
          }
        }
      }

      if (kw < 0.0) {
        kw = 0.0;
      } else if (kw > siteSystemKw()) {
        kw = siteSystemKw();
      }
      return kw;
    }

    // Whole day's predicted total energy, kWh -- each hour's estimatedKw()
    // is already an average kW over that one-hour slot, so summing all 24
    // (kW * 1h) directly gives kWh. Missing hours (received==false)
    // contribute 0 via estimatedKw()'s own guard, same as a partial
    // response elsewhere in this class.
    float totalKwh() const {
      float total = 0.0;
      for (int h = 0; h < HOURS_PER_DAY; h++) {
        total += estimatedKw(h);
      }
      return total;
    }
};

#endif
// ================= end of SolarForecast.h  file
