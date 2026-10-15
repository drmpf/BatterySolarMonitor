// SunPosition.cpp  file  =================
#include "SunPosition.h"
#include "SiteSettings.h" // siteLatitude()/siteLongitude()
#include <math.h>

void sunPosition(int dayOfYear, int month, float hourOfDayLocal,
                  float &elevationDegOut, float &azimuthDegOut) {
  // Site coordinates -- until 2026-09-11 this file and
  // SolarForecastFetch.cpp each hardcoded their own independent LAT/LON
  // constants (matching how BatterySolarMonitorUpdate's own Python
  // scripts each redeclare LAT/LON too); both now read the same
  // site_settings.csv-loaded values instead, so they can no longer
  // silently drift out of sync with each other.
  const float latRad = siteLatitude() * (float)M_PI / 180.0f;
  const float lon = siteLongitude();

  // AEDT (UTC+11) roughly Oct-Mar, AEST (UTC+10) Apr-Sep -- approximation
  // by month only, within a week of the actual transition (same as
  // solar_shading_model.py's own solar_position()).
  bool isAedt = (month >= 10) || (month <= 3);
  float utcOffset = isAedt ? 11.0f : 10.0f;
  float hourUtc = hourOfDayLocal - utcOffset;

  // Solar declination (radians) -- Spencer (1971).
  float gamma = 2.0f * (float)M_PI * (float)(dayOfYear - 1) / 365.0f;
  float decl = 0.006918f
             - 0.399912f * cosf(gamma) + 0.070257f * sinf(gamma)
             - 0.006758f * cosf(2 * gamma) + 0.000907f * sinf(2 * gamma)
             - 0.002697f * cosf(3 * gamma) + 0.00148f * sinf(3 * gamma);

  // Equation of time (minutes).
  float eot = 229.18f * (0.000075f
             + 0.001868f * cosf(gamma) - 0.032077f * sinf(gamma)
             - 0.014615f * cosf(2 * gamma) - 0.04089f * sinf(2 * gamma));

  // Solar time from UTC + longitude + equation of time.
  float solarHour = hourUtc + lon / 15.0f + eot / 60.0f;

  // Hour angle (radians, 0 = solar noon, positive = afternoon).
  float ha = (15.0f * (solarHour - 12.0f)) * (float)M_PI / 180.0f;

  float sinEl = sinf(latRad) * sinf(decl) + cosf(latRad) * cosf(decl) * cosf(ha);
  if (sinEl > 1.0f) {
    sinEl = 1.0f;
  } else if (sinEl < -1.0f) {
    sinEl = -1.0f;
  }
  float elevationRad = asinf(sinEl);
  float elevationDeg = elevationRad * 180.0f / (float)M_PI;

  float cosEl = cosf(elevationRad);
  if (cosEl < 0.001f) {
    cosEl = 0.001f; // avoid divide-by-zero right at the horizon
  }

  float cosAz = (sinf(decl) - sinf(latRad) * sinEl) / (cosf(latRad) * cosEl);
  if (cosAz > 1.0f) {
    cosAz = 1.0f;
  } else if (cosAz < -1.0f) {
    cosAz = -1.0f;
  }
  float azimuthDeg = acosf(cosAz) * 180.0f / (float)M_PI;

  // Afternoon: azimuth > 180 (west of north).
  if (ha > 0.0f) {
    azimuthDeg = 360.0f - azimuthDeg;
  }

  elevationDegOut = elevationDeg;
  azimuthDegOut = azimuthDeg;
}
// ================= end of SunPosition.cpp  file
