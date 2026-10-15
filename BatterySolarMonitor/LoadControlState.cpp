// LoadControlState.cpp  file  =================
#include "LoadControlState.h"
#include <ESP_PicoW_LittleFSsupport.h> // initializeFS()
#include <LittleFS.h>
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static const char *STATE_PATH = "/load_control_state.txt";

static bool loaded = false;
static bool carStarted = false;
static bool hwStarted = false;
static bool carV2hRecovering = false;

// Added 2026-09-15: this module was only ever called from
// LoadControlTask.cpp's single background task until pfodMainMenuApp.cpp's
// "Clear Recovery Flag" button handler started calling
// setCarV2hRecoveryPending() directly from whatever task services pfodWeb
// HTTP requests -- a second, genuinely concurrent caller of these plain
// (unprotected) statics and the LittleFS file they're backed by. Created
// on first use, not at global-construction time -- see
// CarChargerTask.cpp's identical mutex()/lazy-init comment for why; every
// public function below takes this (bounded to 1s, not portMAX_DELAY --
// same reasoning and haveLock idiom as TimestampStream.h's own line-mutex)
// for its whole body, loadIfNeeded()'s and save()'s shared LittleFS file
// I/O included, so the two tasks can no longer interleave a read/modify/
// write of any of the three flags or race each other's save() on the same
// underlying file.
static SemaphoreHandle_t stateMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (stateMutex == NULL) {
    stateMutex = xSemaphoreCreateMutex();
  }
  return stateMutex;
}

static void loadIfNeeded() {
  if (loaded) {
    return;
  }
  // Set first -- a failed mount/open below still leaves all flags at
  // their false default, and this must not retry the LittleFS mount on
  // every single carAutoStarted()/hwAutoStarted()/carV2hRecoveryPending()
  // call.
  loaded = true;

  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  if (!initializeFS()) {
    if (debugPtr) {
      debugPtr->println(F("LoadControlState: LittleFS mount failed -- all flags default to false"));
    }
    return;
  }

  File f = LittleFS.open(STATE_PATH, "r");
  if (!f) {
    if (debugPtr) {
      debugPtr->println(F("LoadControlState: no saved state file yet -- all flags default to false"));
    }
    return; // first boot -- not an error
  }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line == "car=1") {
      carStarted = true;
    } else if (line == "hw=1") {
      hwStarted = true;
    } else if (line == "v2h=1") {
      carV2hRecovering = true;
    }
  }
  f.close();

  if (debugPtr) {
    debugPtr->print(F("LoadControlState: loaded carAutoStarted="));
    debugPtr->print(carStarted ? F("true") : F("false"));
    debugPtr->print(F(" hwAutoStarted="));
    debugPtr->print(hwStarted ? F("true") : F("false"));
    debugPtr->print(F(" carV2hRecoveryPending="));
    debugPtr->println(carV2hRecovering ? F("true") : F("false"));
  }
}

static void save() {
  if (!initializeFS()) {
    if (debugPtr) {
      debugPtr->println(F("LoadControlState: LittleFS mount failed -- could not save"));
    }
    return;
  }
  File f = LittleFS.open(STATE_PATH, "w");
  if (!f) {
    if (debugPtr) {
      debugPtr->println(F("LoadControlState: could not open state file for writing"));
    }
    return;
  }
  f.print(carStarted ? F("car=1\n") : F("car=0\n"));
  f.print(hwStarted ? F("hw=1\n") : F("hw=0\n"));
  f.print(carV2hRecovering ? F("v2h=1\n") : F("v2h=0\n"));
  f.close();
}

bool carAutoStarted() {
  // Bounded wait, not portMAX_DELAY -- see TimestampStream.h's identical
  // haveLock pattern/reasoning: a safety net against any still-undiscovered
  // holder that doesn't give the mutex back, now that it's taken from two
  // tasks instead of one. Proceeds unlocked (best-effort, same exposure as
  // before this mutex existed) on a timeout rather than blocking forever.
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  bool result = carStarted;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

bool hwAutoStarted() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  bool result = hwStarted;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

bool carV2hRecoveryPending() {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  bool result = carV2hRecovering;
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  return result;
}

void setCarAutoStarted(bool started) {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  if (carStarted == started) {
    if (haveLock) {
      xSemaphoreGive(mutex());
    }
    return; // no real transition -- skip the LittleFS write
  }
  carStarted = started;
  save();
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("LoadControlState: carAutoStarted -> "));
    debugPtr->println(started ? F("true") : F("false"));
  }
}

void setHwAutoStarted(bool started) {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  if (hwStarted == started) {
    if (haveLock) {
      xSemaphoreGive(mutex());
    }
    return;
  }
  hwStarted = started;
  save();
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("LoadControlState: hwAutoStarted -> "));
    debugPtr->println(started ? F("true") : F("false"));
  }
}

void setCarV2hRecoveryPending(bool pending) {
  bool haveLock = (xSemaphoreTake(mutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
  loadIfNeeded();
  if (carV2hRecovering == pending) {
    if (haveLock) {
      xSemaphoreGive(mutex());
    }
    return;
  }
  carV2hRecovering = pending;
  save();
  if (haveLock) {
    xSemaphoreGive(mutex());
  }
  if (debugPtr) {
    debugPtr->print(F("LoadControlState: carV2hRecoveryPending -> "));
    debugPtr->println(pending ? F("true") : F("false"));
  }
}
// ================= end of LoadControlState.cpp  file
