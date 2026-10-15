// MorningFloorTable.cpp  file  =================
#include "MorningFloorTable.h"
#include <ESP_PicoW_LittleFSsupport.h> // initializeFS()
#include <LittleFS.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const char *MORNING_FLOOR_PATH = "/morning_floor.csv";

struct FloorEntry {
  int8_t month;
  int8_t hour;
  float floorKw;
};

// 79 rows as of the 2026-09-08 export (build_morning_floor()'s own
// "Wrote N (month, hour) entries" log) -- 12 months x up to ~13 daylight
// hours tops out at 156, so this has headroom for future recalibrations
// without needing a code change.
static const int MAX_ENTRIES = 200;
static FloorEntry entries[MAX_ENTRIES];
static int entryCount = 0;

bool loadMorningFloorTable() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  if (!initializeFS()) {
    if (debugPtr) {
      debugPtr->println(F("MorningFloorTable: LittleFS mount failed"));
    }
    return false;
  }

  File f = LittleFS.open(MORNING_FLOOR_PATH, "r");
  if (!f) {
    if (debugPtr) {
      debugPtr->print(F("MorningFloorTable: could not open "));
      debugPtr->println(MORNING_FLOOR_PATH);
    }
    return false;
  }

  f.readStringUntil('\n'); // header row (month,hour,floor_kw_clear,n_clear,n_total)
  entryCount = 0;
  while (f.available() && entryCount < MAX_ENTRIES) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    // Only the first 3 fields (month,hour,floor_kw_clear) are needed --
    // n_clear/n_total are Python-side calibration metadata, ignored here.
    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma + 1);
    if (firstComma < 0 || secondComma < 0) {
      continue; // malformed line -- skip
    }
    int thirdComma = line.indexOf(',', secondComma + 1);
    String floorField = (thirdComma < 0) ? line.substring(secondComma + 1)
                                          : line.substring(secondComma + 1, thirdComma);
    entries[entryCount].month = (int8_t)line.substring(0, firstComma).toInt();
    entries[entryCount].hour = (int8_t)line.substring(firstComma + 1, secondComma).toInt();
    entries[entryCount].floorKw = floorField.toFloat();
    entryCount++;
  }
  f.close();

  if (debugPtr) {
    debugPtr->print(F("MorningFloorTable: loaded "));
    debugPtr->print(entryCount);
    debugPtr->print(F(" entries from "));
    debugPtr->println(MORNING_FLOOR_PATH);
  }
  return entryCount > 0;
}

float morningFloorKw(int month, int hour) {
  for (int i = 0; i < entryCount; i++) {
    if (entries[i].month == month && entries[i].hour == hour) {
      return entries[i].floorKw;
    }
  }
  return 0.0;
}
// ================= end of MorningFloorTable.cpp  file
