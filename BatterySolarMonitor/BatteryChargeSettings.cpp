// BatteryChargeSettings.cpp  file  =================
#include "BatteryChargeSettings.h"
#include <ESP_PicoW_LittleFSsupport.h> // initializeFS()
#include <LittleFS.h>
#include <pfodDebugPtr.h>
#include <pfodDelay.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const char *SETTINGS_PATH = "/battery_charge_settings.txt";

// Debounces the LittleFS write against a slider being dragged (many
// intermediate messages in quick succession) -- restarted on every change
// from setHwSocOnPercent()/setEvSocOnPercent(), committed by
// checkBatteryChargeSettingsSave() once the settings have been still for
// this long. See BatteryChargeSettings.h's own comment. No separate
// "pending" flag needed -- justFinished() (pfodDelay.cpp) is already a
// one-shot edge (false before the first start(), true exactly once at
// expiry, false again after until the next start()), which is exactly
// "is a save due" with nothing else to track.
static const unsigned long SAVE_DEBOUNCE_MS = 1000UL;
static pfodDelay saveTimer;

// Matches the HW/EV Battery Charge% sub-menu sliders' own valid range --
// see SubMenu_HW_EV___Battery_Char.cpp's prompt text ("10% settings =>
// always on. 100% settings => always off."). Also reused for the two Hot
// Day start% sliders -- same 10-100 range, same generated .cpp file.
static const int MIN_ON_PERCENT = 10;
static const int MAX_ON_PERCENT = 100;
static const int OFF_BELOW_ON = 5;

// Matches the "Tomorrows Max" slider's own valid range
// (SubMenu_HW_EV___Battery_Char.cpp's pwm_Tomorrows_Max_Cmd: `40`20~40~20~
// -- max 40, min 20 deg C).
static const int MIN_TEMP_TRIGGER_C = 20;
static const int MAX_TEMP_TRIGGER_C = 40;

static bool loaded = false;
static int hwOnPercent = BATTERY_CHARGE_DEFAULT_HW_ON_PERCENT;
static int evOnPercent = BATTERY_CHARGE_DEFAULT_EV_ON_PERCENT;
static int maxTempTriggerC = HOT_DAY_DEFAULT_MAX_TEMP_TRIGGER_C;
static int hotDayHwOnPercent_ = HOT_DAY_DEFAULT_HW_ON_PERCENT;
static int hotDayCarOnPercent_ = HOT_DAY_DEFAULT_CAR_ON_PERCENT;

// Same reasoning/shape as LoadControlState.cpp's own mutex: read from
// LoadControlTask.cpp's background task, written from whatever task
// services the sub-menu slider's HTTP request -- a genuine cross-task
// race on these plain statics and the LittleFS file they're backed by.
// Created on first use, not at global-construction time.
static SemaphoreHandle_t settingsMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (settingsMutex == NULL) {
    settingsMutex = xSemaphoreCreateMutex();
  }
  return settingsMutex;
}

static int clampOnPercent(int percent) {
  if (percent < MIN_ON_PERCENT) {
    return MIN_ON_PERCENT;
  }
  if (percent > MAX_ON_PERCENT) {
    return MAX_ON_PERCENT;
  }
  return percent;
}

static int clampTempTrigger(int tempC) {
  if (tempC < MIN_TEMP_TRIGGER_C) {
    return MIN_TEMP_TRIGGER_C;
  }
  if (tempC > MAX_TEMP_TRIGGER_C) {
    return MAX_TEMP_TRIGGER_C;
  }
  return tempC;
}

static void save() {
  if (!initializeFS()) {
    if (debugPtr) {
      debugPtr->println(F("BatteryChargeSettings: LittleFS mount failed -- could not save"));
    }
    return;
  }
  File f = LittleFS.open(SETTINGS_PATH, "w");
  if (!f) {
    if (debugPtr) {
      debugPtr->println(F("BatteryChargeSettings: could not open settings file for writing"));
    }
    return;
  }
  f.print("hw_on=");
  f.println(hwOnPercent);
  f.print("ev_on=");
  f.println(evOnPercent);
  f.print("max_temp_trigger=");
  f.println(maxTempTriggerC);
  f.print("hot_day_hw_on=");
  f.println(hotDayHwOnPercent_);
  f.print("hot_day_car_on=");
  f.println(hotDayCarOnPercent_);
  f.close();
}

// Parses "key=NN" lines into *out, where NN must fall within [minValue,
// maxValue]; returns false (leaving *out untouched) otherwise -- the
// caller treats that as "the whole file is invalid", not a per-field
// patch. Shared by the two HW/EV on% fields, the two Hot Day on% fields
// (all four use MIN_ON_PERCENT/MAX_ON_PERCENT), and the max-temp trigger
// (MIN_TEMP_TRIGGER_C/MAX_TEMP_TRIGGER_C) -- generalised from this file's
// original hardcoded-range version so all five share one parser.
static bool parseRangedIntLine(const String &line, const char *key, int minValue, int maxValue, int *out) {
  String prefix = String(key) + "=";
  if (!line.startsWith(prefix)) {
    return false;
  }
  int value = line.substring(prefix.length()).toInt();
  if (value < minValue || value > maxValue) {
    return false;
  }
  *out = value;
  return true;
}

static void loadIfNeeded() {
  if (loaded) {
    return;
  }
  loaded = true; // set first -- a failed mount/parse below must not retry every call

  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  bool haveHwOn = false;
  bool haveEvOn = false;
  int loadedHwOn = BATTERY_CHARGE_DEFAULT_HW_ON_PERCENT;
  int loadedEvOn = BATTERY_CHARGE_DEFAULT_EV_ON_PERCENT;

  // Hot Day fields -- validated as their OWN all-or-nothing group,
  // SEPARATE from hw_on/ev_on above: added 2026-10-13, after hw_on/ev_on
  // already existed on real devices' saved files. Grouping all five
  // together into one all-or-nothing check would mean every existing
  // device's already-valid hw_on/ev_on (saved before these three fields
  // existed) gets silently reset to BATTERY_CHARGE_DEFAULT_HW_ON_PERCENT/
  // EV_ON_PERCENT too, purely because the newly-added fields are (quite
  // normally) absent from that older file -- exactly the kind of
  // "overwrite a good existing value because of an unrelated missing
  // one" this project's CLAUDE.md explicitly warns against. Keeping them
  // as a separate group means an upgrade from the old 2-field file just
  // adds the three new ones at their own defaults, without touching the
  // user's already-configured hw_on/ev_on at all.
  bool haveMaxTemp = false;
  bool haveHotDayHw = false;
  bool haveHotDayCar = false;
  int loadedMaxTemp = HOT_DAY_DEFAULT_MAX_TEMP_TRIGGER_C;
  int loadedHotDayHw = HOT_DAY_DEFAULT_HW_ON_PERCENT;
  int loadedHotDayCar = HOT_DAY_DEFAULT_CAR_ON_PERCENT;

  if (initializeFS()) {
    File f = LittleFS.open(SETTINGS_PATH, "r");
    if (f) {
      while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (parseRangedIntLine(line, "hw_on", MIN_ON_PERCENT, MAX_ON_PERCENT, &loadedHwOn)) {
          haveHwOn = true;
        } else if (parseRangedIntLine(line, "ev_on", MIN_ON_PERCENT, MAX_ON_PERCENT, &loadedEvOn)) {
          haveEvOn = true;
        } else if (parseRangedIntLine(line, "max_temp_trigger", MIN_TEMP_TRIGGER_C, MAX_TEMP_TRIGGER_C, &loadedMaxTemp)) {
          haveMaxTemp = true;
        } else if (parseRangedIntLine(line, "hot_day_hw_on", MIN_ON_PERCENT, MAX_ON_PERCENT, &loadedHotDayHw)) {
          haveHotDayHw = true;
        } else if (parseRangedIntLine(line, "hot_day_car_on", MIN_ON_PERCENT, MAX_ON_PERCENT, &loadedHotDayCar)) {
          haveHotDayCar = true;
        }
      }
      f.close();
    }
  } else if (debugPtr) {
    debugPtr->print(F("BatteryChargeSettings: LittleFS mount failed -- defaulting to hwOnPercent="));
    debugPtr->print(BATTERY_CHARGE_DEFAULT_HW_ON_PERCENT);
    debugPtr->print(F("% evOnPercent="));
    debugPtr->print(BATTERY_CHARGE_DEFAULT_EV_ON_PERCENT);
    debugPtr->println(F("% and will retry save"));
  }

  bool needsSave = false;

  if (haveHwOn && haveEvOn) {
    hwOnPercent = loadedHwOn;
    evOnPercent = loadedEvOn;
    if (debugPtr) {
      debugPtr->print(F("BatteryChargeSettings: loaded hwOnPercent="));
      debugPtr->print(hwOnPercent);
      debugPtr->print(F("% evOnPercent="));
      debugPtr->print(evOnPercent);
      debugPtr->println(F("%"));
    }
  } else {
    // Missing file, unreadable file, or a partially/fully invalid one --
    // treated as one "invalid" case per explicit instruction, not patched
    // field-by-field: both thresholds default to their own
    // BATTERY_CHARGE_DEFAULT_HW_ON_PERCENT/BATTERY_CHARGE_DEFAULT_EV_ON_PERCENT
    // and the file is rewritten fresh with those defaults.
    hwOnPercent = BATTERY_CHARGE_DEFAULT_HW_ON_PERCENT;
    evOnPercent = BATTERY_CHARGE_DEFAULT_EV_ON_PERCENT;
    needsSave = true;
    if (debugPtr) {
      debugPtr->print(F("BatteryChargeSettings: hw_on/ev_on missing or invalid -- defaulting to hwOnPercent="));
      debugPtr->print(hwOnPercent);
      debugPtr->print(F("% evOnPercent="));
      debugPtr->print(evOnPercent);
      debugPtr->println(F("% and re-creating it"));
    }
  }

  if (haveMaxTemp && haveHotDayHw && haveHotDayCar) {
    maxTempTriggerC = loadedMaxTemp;
    hotDayHwOnPercent_ = loadedHotDayHw;
    hotDayCarOnPercent_ = loadedHotDayCar;
    if (debugPtr) {
      debugPtr->print(F("BatteryChargeSettings: loaded maxTempTriggerC="));
      debugPtr->print(maxTempTriggerC);
      debugPtr->print(F("C hotDayHwOnPercent="));
      debugPtr->print(hotDayHwOnPercent_);
      debugPtr->print(F("% hotDayCarOnPercent="));
      debugPtr->print(hotDayCarOnPercent_);
      debugPtr->println(F("%"));
    }
  } else {
    // Same all-or-nothing rule as hw_on/ev_on above, but kept as its OWN
    // group -- see this block's own top comment for why.
    maxTempTriggerC = HOT_DAY_DEFAULT_MAX_TEMP_TRIGGER_C;
    hotDayHwOnPercent_ = HOT_DAY_DEFAULT_HW_ON_PERCENT;
    hotDayCarOnPercent_ = HOT_DAY_DEFAULT_CAR_ON_PERCENT;
    needsSave = true;
    if (debugPtr) {
      debugPtr->print(F("BatteryChargeSettings: Hot Day settings missing or invalid -- defaulting to maxTempTriggerC="));
      debugPtr->print(maxTempTriggerC);
      debugPtr->print(F("C hotDayHwOnPercent="));
      debugPtr->print(hotDayHwOnPercent_);
      debugPtr->print(F("% hotDayCarOnPercent="));
      debugPtr->print(hotDayCarOnPercent_);
      debugPtr->println(F("% and re-creating it"));
    }
  }

  if (needsSave) {
    save();
  }
}

int hwSocOnPercent() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  int result = hwOnPercent;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

int hwSocOffPercent() {
  return hwSocOnPercent() - OFF_BELOW_ON;
}

int evSocOnPercent() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  int result = evOnPercent;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

int evSocOffPercent() {
  return evSocOnPercent() - OFF_BELOW_ON;
}

void setHwSocOnPercent(int percent) {
  percent = clampOnPercent(percent);
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  if (hwOnPercent == percent) {
    if (haveLock) {
      xSemaphoreGive(mutex());
    }
    return; // no real change -- nothing to debounce/save
  }
  hwOnPercent = percent;
  saveTimer.start(SAVE_DEBOUNCE_MS); // (re)start the debounce -- see checkBatteryChargeSettingsSave()
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("BatteryChargeSettings: hwOnPercent -> "));
    debugPtr->print(percent);
    debugPtr->println(F(" (save pending)"));
  }
}

void setEvSocOnPercent(int percent) {
  percent = clampOnPercent(percent);
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  if (evOnPercent == percent) {
    if (haveLock) {
      xSemaphoreGive(mutex());
    }
    return;
  }
  evOnPercent = percent;
  saveTimer.start(SAVE_DEBOUNCE_MS);
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("BatteryChargeSettings: evOnPercent -> "));
    debugPtr->print(percent);
    debugPtr->println(F(" (save pending)"));
  }
}

int tomorrowsMaxTempTriggerC() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  int result = maxTempTriggerC;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

void setTomorrowsMaxTempTriggerC(int tempC) {
  tempC = clampTempTrigger(tempC);
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  if (maxTempTriggerC == tempC) {
    if (haveLock) {
      xSemaphoreGive(mutex());
    }
    return;
  }
  maxTempTriggerC = tempC;
  saveTimer.start(SAVE_DEBOUNCE_MS);
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("BatteryChargeSettings: maxTempTriggerC -> "));
    debugPtr->print(tempC);
    debugPtr->println(F(" (save pending)"));
  }
}

int hotDayHwOnPercent() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  int result = hotDayHwOnPercent_;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

int hotDayHwOffPercent() {
  return hotDayHwOnPercent() - OFF_BELOW_ON;
}

void setHotDayHwOnPercent(int percent) {
  percent = clampOnPercent(percent);
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  if (hotDayHwOnPercent_ == percent) {
    if (haveLock) {
      xSemaphoreGive(mutex());
    }
    return;
  }
  hotDayHwOnPercent_ = percent;
  saveTimer.start(SAVE_DEBOUNCE_MS);
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("BatteryChargeSettings: hotDayHwOnPercent -> "));
    debugPtr->print(percent);
    debugPtr->println(F(" (save pending)"));
  }
}

int hotDayCarOnPercent() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  int result = hotDayCarOnPercent_;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

int hotDayCarOffPercent() {
  return hotDayCarOnPercent() - OFF_BELOW_ON;
}

void setHotDayCarOnPercent(int percent) {
  percent = clampOnPercent(percent);
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  if (hotDayCarOnPercent_ == percent) {
    if (haveLock) {
      xSemaphoreGive(mutex());
    }
    return;
  }
  hotDayCarOnPercent_ = percent;
  saveTimer.start(SAVE_DEBOUNCE_MS);
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("BatteryChargeSettings: hotDayCarOnPercent -> "));
    debugPtr->print(percent);
    debugPtr->println(F(" (save pending)"));
  }
}

// Called every loop() from the main .ino loop() -- see this file's own
// header comment. justFinished() is called unconditionally, every call,
// never gated behind anything else -- same rule every other pfodDelay
// timer in this project follows. Its own one-shot-edge semantics already
// mean this only ever proceeds past the guard once per settle, so no
// separate "pending" flag is needed.
void checkBatteryChargeSettingsSave() {
  if (!saveTimer.justFinished()) {
    return;
  }
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  save();
  int hwNow = hwOnPercent;
  int evNow = evOnPercent;
  int maxTempNow = maxTempTriggerC;
  int hotDayHwNow = hotDayHwOnPercent_;
  int hotDayCarNow = hotDayCarOnPercent_;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("BatteryChargeSettings: committed to LittleFS -- hwOnPercent="));
    debugPtr->print(hwNow);
    debugPtr->print(F("% evOnPercent="));
    debugPtr->print(evNow);
    debugPtr->print(F("% maxTempTriggerC="));
    debugPtr->print(maxTempNow);
    debugPtr->print(F("C hotDayHwOnPercent="));
    debugPtr->print(hotDayHwNow);
    debugPtr->print(F("% hotDayCarOnPercent="));
    debugPtr->print(hotDayCarNow);
    debugPtr->println(F("%"));
  }
}
// ================= end of BatteryChargeSettings.cpp  file
