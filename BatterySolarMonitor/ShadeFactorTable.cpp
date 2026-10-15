// ShadeFactorTable.cpp  file  =================
#include "ShadeFactorTable.h"
#include <ESP_PicoW_LittleFSsupport.h> // initializeFS()
#include <LittleFS.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const char *SHADE_GRID_PATH = "/shade_grid.csv";

// Matches solar_shading_model.py's own AZ_BIN/EL_BIN constants.
static const int AZ_BIN_SIZE = 10;
static const int EL_BIN_SIZE = 5;

// solar_shading_model.py's own fallback for a sun position bin with no
// training evidence at all (apply_shading(): shade_factor.fillna(0.85)).
static const float DEFAULT_SHADE_FACTOR = 0.85;

struct ShadeEntry {
  int16_t azBin;
  int16_t elBin;
  float shadeFactor;
};

// 216 rows as of the 2026-09-08 export (solar_shading_model.py's own
// "Wrote N (az_bin, el_bin) rows" log) -- sized with headroom for future
// recalibrations adding a few more bins without needing a code change.
static const int MAX_ENTRIES = 320;
static ShadeEntry entries[MAX_ENTRIES];
static int entryCount = 0;

bool loadShadeFactorTable() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  if (!initializeFS()) {
    if (debugPtr) {
      debugPtr->println(F("ShadeFactorTable: LittleFS mount failed"));
    }
    return false;
  }

  File f = LittleFS.open(SHADE_GRID_PATH, "r");
  if (!f) {
    if (debugPtr) {
      debugPtr->print(F("ShadeFactorTable: could not open "));
      debugPtr->println(SHADE_GRID_PATH);
    }
    return false;
  }

  f.readStringUntil('\n'); // header row (az_bin,el_bin,shade_factor)
  entryCount = 0;
  while (f.available() && entryCount < MAX_ENTRIES) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma + 1);
    if (firstComma < 0 || secondComma < 0) {
      continue; // malformed line -- skip
    }
    entries[entryCount].azBin = (int16_t)line.substring(0, firstComma).toInt();
    entries[entryCount].elBin = (int16_t)line.substring(firstComma + 1, secondComma).toInt();
    entries[entryCount].shadeFactor = line.substring(secondComma + 1).toFloat();
    entryCount++;
  }
  f.close();

  if (debugPtr) {
    debugPtr->print(F("ShadeFactorTable: loaded "));
    debugPtr->print(entryCount);
    debugPtr->print(F(" entries from "));
    debugPtr->println(SHADE_GRID_PATH);
  }
  return entryCount > 0;
}

float shadeFactorForSunPosition(float azDeg, float elDeg) {
  // Same floor-to-bin rule as solar_shading_model.py's own
  // (sun_azimuth // AZ_BIN * AZ_BIN) -- plain integer division matches
  // Python's floor // for non-negative operands, which azDeg/elDeg always
  // are for any sun position worth looking up (see this function's own
  // header comment).
  int azBin = ((int)azDeg / AZ_BIN_SIZE) * AZ_BIN_SIZE;
  int elBin = ((int)elDeg / EL_BIN_SIZE) * EL_BIN_SIZE;
  for (int i = 0; i < entryCount; i++) {
    if (entries[i].azBin == azBin && entries[i].elBin == elBin) {
      return entries[i].shadeFactor;
    }
  }
  return DEFAULT_SHADE_FACTOR;
}
// ================= end of ShadeFactorTable.cpp  file
