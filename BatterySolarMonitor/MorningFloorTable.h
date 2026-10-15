// MorningFloorTable.h  file  =================
// Loads dailyPrediction/solar_shading_model.py's empirical (month, hour)
// production floor (data/morning_floor.csv, built by that file's
// build_morning_floor()) from LittleFS at boot -- compensates for
// Open-Meteo's GTI being systematically too low at low sun elevations
// (see that function's own doc comment for why a multiplicative shade
// factor alone can't fix this).
#ifndef MORNING_FLOOR_TABLE_H
#define MORNING_FLOOR_TABLE_H

// Reads /morning_floor.csv from LittleFS (mounts it via initializeFS() --
// ESP_PicoW_LittleFSsupport.h -- if not already mounted) into an
// in-memory table. Call once from setup(). Returns true if the file was
// found and at least one row was parsed.
bool loadMorningFloorTable();

// Clear-sky production floor (kW) for the given calendar month (1-12) and
// hour of day (0-23). Returns 0.0 (no floor -- same as
// solar_shading_model.py's own floor_map.get(k, 0.0)) if the table hasn't
// been loaded yet or this (month, hour) was never seen in training.
float morningFloorKw(int month, int hour);

#endif
// ================= end of MorningFloorTable.h  file
