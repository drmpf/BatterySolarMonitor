// DebugLogBuffer.cpp  file  =================
#include "DebugLogBuffer.h"
#include <ESP_PicoW_pfodWebServer.h> // getRawDataWriter(), resetCurrentRawDataCursor()
#include <pfodCircularLineBuffer.h> // preWebServerBuffer -- see notifyWebServerStarted()
#include "CarChargeHistory.h" // printCarChargeHistory() -- see debugLogBufferDumpAll()
#include "CarChargerInstances.h" // carChargingKwhToday
#include "CarChargerTask.h" // lockCarChargerState()/unlockCarChargerState()
#include "LocalTimeMinutes.h" // localTodayDateString()

SemaphoreHandle_t debugLogLineMutex() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}

// Captures every debug byte written before start_pfodWebServer() has run
// and allocated the real httpRawData relay -- see this file's own top
// comment ("PRE-WEBSERVER-STARTUP CAPTURE") for the full story. Sized for
// the modest amount of text setup() actually prints before
// start_pfodWebServer() runs (a handful of config-loader lines) -- NOT
// the full 16K Log Tail itself retains once the real relay is live.
// Static global, like the old (pre-2026-09-26) debugLogBuffer this file
// used to keep -- allocates once, at program start, and is never freed;
// left parked afterwards rather than reclaimed, same tradeoff that design
// already made, and negligible against this device's own free heap
// (155KB+ confirmed free after a full setup() in a real device log). Its
// permanence is now load-bearing, not just a cheap tradeoff: since
// debugLogBufferDumpAll() re-reads it fresh on every Log Tail open (see
// this file's own top comment), nothing may ever write into it again
// once notifyWebServerStarted() latches, or clear/free it -- doing so
// would silently blank the startup log for the rest of this boot.
static const size_t PRE_WEBSERVER_BUFFER_SIZE = 4096;
static pfodCircularLineBuffer preWebServerBuffer(PRE_WEBSERVER_BUFFER_SIZE);
static bool webServerStarted = false;

void debugLogBufferWrite(uint8_t c) {
  if (webServerStarted) {
    getRawDataWriter().write(c);
  } else {
    preWebServerBuffer.write(c);
  }
}

void notifyWebServerStarted() {
  // Idempotent, but no longer needs to guard against running its own
  // one-time flush twice -- see below, there's no flush left to repeat.
  webServerStarted = true;
}

void debugLogBufferDumpAll(Print &out, const __FlashStringHelper *openScreenMsg) {
  out.print(openScreenMsg);          // {=…~C}: pfodWeb clears its Raw Data screen at this boundary

  // Per explicit instruction (2026-09-28): show the last 5 days' car
  // charging kWh (plus today's running total) at the very top of the Log
  // Tail screen, before the rolling debug log itself -- see
  // CarChargeHistory.h's own module comment for why this needed adding
  // (carChargingKwhToday has no persistence/cloud backing of its own, so
  // an unplanned reboot used to silently lose it with no way to tell
  // afterwards). printCarChargeHistory() itself reads
  // /car_charge_history.csv from LittleFS, so the WHOLE call must stay
  // inside the lock, not just the carChargingKwhToday read -- otherwise
  // this (main-thread, on a button press) races unprotected against
  // CarChargerTask.cpp's own background-task writes to that same file,
  // exactly the class of bug BatteryChargeSettings.cpp's own mutex
  // comment already warns about ("a genuine cross-task race on these
  // plain statics and the LittleFS file they're backed by").
  char todayBuf[11];
  localTodayDateString(todayBuf, sizeof(todayBuf));
  lockCarChargerState();
  printCarChargeHistory(out, String(todayBuf), carChargingKwhToday);
  unlockCarChargerState();

  // Re-prints the captured startup log (preWebServerBuffer) EVERY time Log
  // Tail is opened, not just once -- per explicit instruction: this
  // device reboots once a day at midnight (DailyReboot.h), so by the time
  // anyone actually checks Log Tail (e.g. the following morning), many
  // hours of ordinary logging have almost certainly pushed the original
  // boot sequence out of the main 16K rolling relay (httpRawData) through
  // ordinary eviction -- confirmed in practice: a client whose Log Tail
  // cursor reset well after that much had accumulated never saw the boot
  // lines at all, only whatever was still in the 16K window at that
  // moment. Printing it explicitly here, every time, guarantees it's
  // always visible regardless of how long ago boot actually was or how
  // much has logged since.
  //
  // Safe to re-read repeatedly: preWebServerBuffer is never written to
  // again once notifyWebServerStarted() latches webServerStarted (see
  // debugLogBufferWrite() above), so its content never changes and is
  // never evicted -- resetForRead() just rewinds the READ position,
  // leaving the underlying bytes (and the buffer's own head/tail)
  // untouched, so every call here reproduces the exact same text.
  // Printed directly/uncapped, same as printCarChargeHistory() just
  // above -- this is the modest "a handful of config-loader lines"
  // amount described where preWebServerBuffer is declared, not the full
  // 16K relay (which DOES need MAX_RAW_DATA_PER_RESPONSE's own pacing,
  // see ESP_PicoW_pfodWebServer.cpp), so no fragmentation risk here.
  preWebServerBuffer.resetForRead();
  while (preWebServerBuffer.available()) {
    out.print((char)preWebServerBuffer.read());
  }

  // Restarts THIS client's own read cursor -- see this function's own doc
  // comment (DebugLogBuffer.h) for why that's enough, and why it can never
  // affect any other client's progress.
  resetCurrentRawDataCursor();
}
// ================= end of DebugLogBuffer.cpp  file
