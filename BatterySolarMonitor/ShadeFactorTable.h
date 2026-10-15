// ShadeFactorTable.h  file  =================
// Loads dailyPrediction/solar_shading_model.py's per-sun-angle shade
// factor lookup (data/shade_grid.csv, exported by that file's own
// write_shade_grid()) from LittleFS at boot -- see solar_shading_model.py
// for how the table is built (NNLS regression against Enphase actuals,
// smoothed, then corrected for two known trees plus a table of empirical
// per-bin overrides). Same (az_bin, el_bin) binning as the Python model:
// 10deg azimuth bins, 5deg elevation bins.
#ifndef SHADE_FACTOR_TABLE_H
#define SHADE_FACTOR_TABLE_H

// Reads /shade_grid.csv from LittleFS (mounts it via initializeFS() --
// ESP_PicoW_LittleFSsupport.h -- if not already mounted) into an
// in-memory table. Call once from setup(). Returns true if the file was
// found and at least one row was parsed.
bool loadShadeFactorTable();

// Shade factor for the sun position nearest (azDeg, elDeg) -- binned the
// same way as the Python model (floor to the nearest 10deg/5deg). Both
// are expected non-negative (elDeg <= 0 -- sun below the horizon -- is
// never meaningfully looked up: the caller's own GTI is already ~0 then,
// same reasoning solar_shading_model.py's own pipeline uses to only ever
// bin daylight hours in the first place). Returns 0.85 -- the same
// fallback the Python model itself uses for a sun position with no
// training evidence at all (apply_shading(): shade_factor.fillna(0.85))
// -- if the table hasn't been loaded yet or this bin was never seen.
float shadeFactorForSunPosition(float azDeg, float elDeg);

#endif
// ================= end of ShadeFactorTable.h  file
