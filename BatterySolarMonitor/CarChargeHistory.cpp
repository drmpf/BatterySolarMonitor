// CarChargeHistory.cpp  file  =================
#include "CarChargeHistory.h"
#include <ESP_PicoW_LittleFSsupport.h> // initializeFS()
#include <LittleFS.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const char *TODAY_PATH     = "/car_charge_today.csv";
static const char *HISTORY_PATH   = "/car_charge_history.csv";
static const int MAX_HISTORY_DAYS = 5;

static Print *dbg() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  return debugPtr;
}

// Reads TODAY_PATH's "date=.../kwh=..." lines. Returns false
// (outDate/outKwh untouched) if the file is missing or either line is
// absent/malformed -- caller then treats it the same as "no prior file".
static bool readTodayFile(String *outDate, float *outKwh) {
  if (!initializeFS()) {
    return false;
  }
  File f = LittleFS.open(TODAY_PATH, "r");
  if (!f) {
    return false;
  }
  bool haveDate = false, haveKwh = false;
  String date;
  float kwh = 0.0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.startsWith("date=")) {
      date = line.substring(5);
      haveDate = date.length() > 0;
    } else if (line.startsWith("kwh=")) {
      kwh = line.substring(4).toFloat();
      haveKwh = true;
    }
  }
  f.close();
  if (!haveDate || !haveKwh) {
    return false;
  }
  *outDate = date;
  *outKwh = kwh;
  return true;
}

static void writeTodayFile(const String &date, float kwh) {
  if (!initializeFS()) {
    if (dbg()) {
      debugPtr->println(F("CarChargeHistory: LittleFS mount failed -- could not save today's kwh"));
    }
    return;
  }
  File f = LittleFS.open(TODAY_PATH, "w");
  if (!f) {
    if (dbg()) {
      debugPtr->println(F("CarChargeHistory: could not open today file for writing"));
    }
    return;
  }
  f.print("date=");
  f.println(date);
  f.print("kwh=");
  f.println(kwh, 2);
  f.close();
}

// Reads HISTORY_PATH (oldest first, "date,kwh" per line) into
// outDates[]/outKwh[], both sized maxEntries. Returns the count actually
// found (0..maxEntries). A file somehow holding more than maxEntries
// lines (e.g. hand-edited) is recovered sensibly by keeping only the
// MOST RECENT maxEntries lines seen, oldest dropped first -- same trim
// rule appendHistoryEntry() itself uses going forward.
static int readHistoryFile(String outDates[], float outKwh[], int maxEntries) {
  if (!initializeFS()) {
    return 0;
  }
  File f = LittleFS.open(HISTORY_PATH, "r");
  if (!f) {
    return 0;
  }
  int count = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    int comma = line.indexOf(',');
    if (comma < 0) {
      continue; // malformed line -- skip
    }
    String date = line.substring(0, comma);
    float kwh = line.substring(comma + 1).toFloat();
    if (count < maxEntries) {
      outDates[count] = date;
      outKwh[count] = kwh;
      count++;
    } else {
      for (int i = 1; i < maxEntries; i++) {
        outDates[i - 1] = outDates[i];
        outKwh[i - 1] = outKwh[i];
      }
      outDates[maxEntries - 1] = date;
      outKwh[maxEntries - 1] = kwh;
    }
  }
  f.close();
  return count;
}

static void writeHistoryFile(const String dates[], const float kwhs[], int count) {
  if (!initializeFS()) {
    if (dbg()) {
      debugPtr->println(F("CarChargeHistory: LittleFS mount failed -- could not save history"));
    }
    return;
  }
  File f = LittleFS.open(HISTORY_PATH, "w");
  if (!f) {
    if (dbg()) {
      debugPtr->println(F("CarChargeHistory: could not open history file for writing"));
    }
    return;
  }
  for (int i = 0; i < count; i++) {
    f.print(dates[i]);
    f.print(",");
    f.println(kwhs[i], 2);
  }
  f.close();
}

// Appends (date, kwh) to the history file, dropping the oldest entry if
// already at MAX_HISTORY_DAYS.
static void appendHistoryEntry(const String &date, float kwh) {
  String dates[MAX_HISTORY_DAYS];
  float kwhs[MAX_HISTORY_DAYS];
  int count = readHistoryFile(dates, kwhs, MAX_HISTORY_DAYS);
  if (count == MAX_HISTORY_DAYS) {
    for (int i = 1; i < MAX_HISTORY_DAYS; i++) {
      dates[i - 1] = dates[i];
      kwhs[i - 1] = kwhs[i];
    }
    dates[MAX_HISTORY_DAYS - 1] = date;
    kwhs[MAX_HISTORY_DAYS - 1] = kwh;
  } else {
    dates[count] = date;
    kwhs[count] = kwh;
    count++;
  }
  writeHistoryFile(dates, kwhs, count);
}

void loadOrResumeCarChargeToday(const String &todayDateStr, float *outKwhToday) {
  dbg();
  String storedDate;
  float storedKwh = 0.0;
  if (readTodayFile(&storedDate, &storedKwh)) {
    if (storedDate == todayDateStr) {
      *outKwhToday = storedKwh;
      if (debugPtr) {
        debugPtr->print(F("CarChargeHistory: resumed today's kwh="));
        debugPtr->print(storedKwh, 2);
        debugPtr->println(F(" from before an unplanned reboot"));
      }
      return; // today file already correct -- nothing to append/rewrite
    }
    // Stored file is from a finished day that never rolled over gracefully
    // (device was off across that midnight) -- bank it before starting fresh.
    appendHistoryEntry(storedDate, storedKwh);
    if (debugPtr) {
      debugPtr->print(F("CarChargeHistory: recovered finished day "));
      debugPtr->print(storedDate);
      debugPtr->print(F("="));
      debugPtr->print(storedKwh, 2);
      debugPtr->println(F("kwh from before a reboot that crossed midnight"));
    }
  }
  *outKwhToday = 0.0;
  writeTodayFile(todayDateStr, 0.0);
}

void saveCarChargeToday(const String &todayDateStr, float kwhToday) {
  writeTodayFile(todayDateStr, kwhToday);
}

void rolloverCarChargeDay(const String &newTodayDateStr, const String &finishedDateStr, float finishedKwh) {
  appendHistoryEntry(finishedDateStr, finishedKwh);
  writeTodayFile(newTodayDateStr, 0.0);
  if (dbg()) {
    debugPtr->print(F("CarChargeHistory: day rolled over -- "));
    debugPtr->print(finishedDateStr);
    debugPtr->print(F("="));
    debugPtr->print(finishedKwh, 2);
    debugPtr->println(F("kwh banked"));
  }
}

void printCarChargeHistory(Print &out, const String &todayDateStr, float kwhToday) {
  String dates[MAX_HISTORY_DAYS];
  float kwhs[MAX_HISTORY_DAYS];
  int count = readHistoryFile(dates, kwhs, MAX_HISTORY_DAYS);
  out.println(F("Car charge kWh, last 5 days:"));
  for (int i = 0; i < count; i++) {
    out.print(F("  "));
    out.print(dates[i]);
    out.print(F(": "));
    out.println(kwhs[i], 2);
  }
  out.print(F("  "));
  out.print(todayDateStr);
  out.print(F(" (today, so far): "));
  out.println(kwhToday, 2);
}
// ================= end of CarChargeHistory.cpp  file
