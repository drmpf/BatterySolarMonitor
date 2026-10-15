// GatewayPollTask.cpp  file  =================
#include "GatewayPollTask.h"
#include "GatewayFetch.h"
#include "GatewayInstances.h"
#include <Arduino.h>
#include <pfodDebugPtr.h>
#include <pfodDelay.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// 5 minutes -- matches PlotData's own design intent (setPointAtTime's
// nearest-slot rounding was designed for a ~5min poll cycle against a
// 15min grid, see PlotData.h).
static const unsigned long POLL_INTERVAL_MS = 5UL * 60UL * 1000UL;

// Used whenever this poll comes back with batterySocPercent still at its
// -1 sentinel -- either the WHOLE fetch failed (e.g. /ivp/meters/readings
// itself got "connection refused"/timed out, see checkGatewayPoll() below)
// or just the battery-status endpoint did (GatewayFetch.cpp's
// fetchBatteryStatus(), a separate request within fetchGatewayPower()).
// Rather than wait out the full 5-minute cycle when that happens, retry
// sooner -- confirmed via deviceLog.txt as a real, recurring need: the
// gateway's own minimal embedded HTTPS server can refuse/stall connections
// for a couple of minutes at a stretch (e.g. right after a prior request,
// or under its own load), and letting InternetPollTask.cpp's shared
// background task move on to the OTHER polls (Solar, Enlighten, car
// charger, hot water, ...) in the meantime is exactly what already
// happens: this function returns after one attempt, so nothing here
// blocks those other polls -- InternetPollTask.cpp's 1s tick keeps
// servicing all of them every second regardless of this timer, and simply
// finds checkGatewayPoll() a no-op (pollTimer not yet finished) until this
// delay elapses, then "comes back" to retry on its own next tick.
// 15s -- per explicit instruction, alongside
// consecutiveBatteryFetchFailures() below: BuzzerControl.cpp's low-SOC
// alarm treats 20 of these consecutive failures (5 minutes of not being
// able to confirm the battery's SOC at all) as alarm-worthy in its own
// right, so this interval also sets how finely that 5 minutes is diced up.
static const unsigned long FETCH_RETRY_DELAY_MS = 15UL * 1000UL; // 15 seconds

// Created on first use, not at global-construction time -- see
// SolarForecastTask.cpp's identical comment for why.
static SemaphoreHandle_t gatewayMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (gatewayMutex == NULL) {
    gatewayMutex = xSemaphoreCreateMutex();
  }
  return gatewayMutex;
}

void lockGatewayReading() {
  xSemaphoreTake(mutex(), portMAX_DELAY);
}

void unlockGatewayReading() {
  xSemaphoreGive(mutex());
}

static volatile bool readingUpdatedFlag = false;

bool gatewayReadingUpdated() {
  bool result = false;
  lockGatewayReading();
  if (readingUpdatedFlag) {
    result = true;
    readingUpdatedFlag = false;
  }
  unlockGatewayReading();
  return result;
}

// Consecutive polls where batterySocPercent came back at its -1 "fetch
// failed" sentinel (GatewayFetch.h) -- never 0.0 itself, see that file's
// own isNull() guards -- right up to and including the most recent poll.
// batteryFailStreak (this counter, not batterySocPercent) resets to 0 the
// moment any poll gets a real (>=0.0) reading; batterySocPercent itself
// takes on whatever that reading actually was, not 0. Not one-shot/
// consumed like readingUpdatedFlag above -- see this file's own header
// comment.
static int batteryFailStreak = 0;

int consecutiveBatteryFetchFailures() {
  int result;
  lockGatewayReading();
  result = batteryFailStreak;
  unlockGatewayReading();
  return result;
}

// pfodDelay (pfodParser/src/pfodDelay.h), not a plain millis()/nextDueMs
// comparison -- a raw "millis() < nextDueMs" check is wrong once millis()
// wraps around (every ~49.7 days of uptime): nextDueMs stays a large
// pre-wrap value while millis() resets near 0, so the comparison reads
// "not due yet" for a long stretch after the wrap. pfodDelay's
// justFinished() instead compares via subtraction
// ((millis() - startTime) >= delay), which is wraparound-safe. See
// https://www.forward.com.au/pfod/ArduinoProgramming/TimingDelaysInArduino.html
// for the idiom this follows: justFinished() is checked unconditionally,
// at the outermost level, every call -- never folded into a compound
// condition with isRunning() (which would short-circuit past it and can
// hide a delay that's actually finished).
static pfodDelay pollTimer;

void checkGatewayPoll() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  if (!pollTimer.isRunning()) {
    // Due immediately the first time InternetPollTask.cpp ticks this,
    // same as this job's old "deliberately un-delayed" boot behaviour --
    // pfodDelay's own start(0) means justFinished() reads true right
    // away below. No longer needs an explicit startup stagger against
    // the other jobs -- InternetPollTask.cpp runs them one at a time on
    // a single shared task, so there's no longer a way for two of them
    // to open connections at the same moment regardless of ordering.
    pollTimer.start(0);
  }
  if (!pollTimer.justFinished()) {
    return;
  }

  float solarKw, consumeKw, batterySocPercent;
  bool ok = fetchGatewayPower(solarKw, consumeKw, batterySocPercent);
  // batterySocPercent is always a valid signal here (set to -1 right at
  // the top of fetchGatewayPower(), before any of its own early
  // returns) regardless of 'ok' -- see GatewayFetch.h. batteryFailStreak
  // tracks it independently of 'ok' (and of latestBatterySocPercent, which
  // this poll only updates below when the WHOLE fetch succeeds) -- a
  // battery-status failure counts here whether or not solar/consumption
  // also failed this same cycle.
  lockGatewayReading();
  if (batterySocPercent < 0.0) {
    batteryFailStreak++;
  } else {
    batteryFailStreak = 0;
  }
  if (ok) {
    latestSolarKw = solarKw;
    latestConsumeKw = consumeKw;
    latestBatterySocPercent = batterySocPercent; // -1 if just that part failed -- see GatewayFetch.h
    readingUpdatedFlag = true;
  }
  unlockGatewayReading();
  unsigned long delayMs = (batterySocPercent < 0.0) ? FETCH_RETRY_DELAY_MS : POLL_INTERVAL_MS;
  pollTimer.start(delayMs);
}
// ================= end of GatewayPollTask.cpp  file
