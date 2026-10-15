// SunPosition.h  file  =================
// Solar elevation/azimuth calculation -- Spencer (1971) equations, ported
// from dailyPrediction/solar_shading_model.py's and plot_today_solar.py's
// own solar_position() (identical in both). Feeds ShadeFactorTable.h's
// (az_bin, el_bin) lookup from SolarForecast.h's estimatedKw().
#ifndef SUN_POSITION_H
#define SUN_POSITION_H

// Computes solar elevation and azimuth for a given local time.
// dayOfYear      -- 1-366 (LocalTimeMinutes.h's localMonthAndDayOfYear())
// month          -- 1-12 -- only used to approximate the AEST/AEDT DST
//                   offset (Oct-Mar = AEDT/UTC+11, Apr-Sep = AEST/UTC+10),
//                   the same month-based approximation
//                   solar_shading_model.py's own solar_position() uses
//                   (accurate to within a week of the actual transition).
// hourOfDayLocal -- local time of day, 0.0-23.99 (hour + minute/60)
// elevationDegOut/azimuthDegOut -- receive the result, degrees
//                   (elevation: -90 to 90; azimuth: 0-360, clockwise from
//                   North -- 0=N, 90=E, 180=S, 270=W)
void sunPosition(int dayOfYear, int month, float hourOfDayLocal,
                  float &elevationDegOut, float &azimuthDegOut);

#endif
// ================= end of SunPosition.h  file
