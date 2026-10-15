// EnlightenHistoryRefresh.cpp  file  =================
#include "EnlightenHistoryRefresh.h"
#include "EnlightenHistoryFetch.h"
#include "EnlightenHistoryTask.h"
#include "EnlightenHistoryInstances.h"
#include "PlotDataInstances.h"
#include "LocalTimeMinutes.h"
#include "ntpSupport.h"
#include <Arduino.h>
#include <pfodDebugPtr.h>
#include <pfodDelay.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// Give InternetPollTask.cpp's shared background task room to get through
// its first Gateway/Solar/Weather poll (each ~2-4s of HTTPS+JSON) before
// this one's first request -- mostly historical now that every internet
// poll is already serialized onto that one task (so there's no longer a
// concurrent-connection risk to avoid), but there's no benefit to
// requesting Enlighten history before NTP has even synced anyway (see the
// missedSNTPupdate() check in checkEnlightenHistory() below), so this
// stays as a simple floor -- attemptTimer's own initial delay
// (initializeEnlightenHistoryRefresh(), called once from setup()), not a
// separate millis() comparison.
static const unsigned long MIN_UPTIME_BEFORE_HISTORY_MS = 20000;

// How often to refresh, once a fetch has actually succeeded. Per
// explicit instruction, now 5min -- shorter than Enphase's own 15-minute
// history slot resolution, specifically to pick up a newly-available
// slot sooner: at the old 15min cadence, a refresh landing just before
// Enphase's cloud side actually advanced to a new slot meant waiting
// almost a full 15 more minutes for the NEXT refresh to pick it up.
// Polling every 5min instead caps that worst-case pickup delay at ~5min.
// Most individual refreshes still just re-confirm a slot that hasn't
// advanced yet (a harmless, idempotent re-merge) -- that's expected and
// fine; the point is the worst case, not the typical case. Each
// successful refresh's mergeHistoryInto() below overwrites
// GatewayPollTask.cpp's own more-frequent-but-INSTANTANEOUS 5-min local
// readings with Enphase's true 15-min AVERAGE for the same slots
// (smoother -- see the conversation this was added from), and, since
// this fetch goes to Enphase's cloud rather than the local gateway, also
// transparently backfills any stretch where the local gateway connection
// itself was down.
static const unsigned long REFRESH_INTERVAL_MS = 5UL * 60UL * 1000UL;

// How long to wait before starting a fresh attempt after one FAILS.
// Short, since each attempt now remembers and skips its own failed IPs
// (EnlightenHistoryFetch.h's isKnownBadIP()/markIPBad()) -- retrying
// sooner mainly gives the DNS round-robin/router cache another chance to
// hand back a not-yet-tried IP, not a full backoff wait.
static const unsigned long RETRY_DELAY_MS = 30UL * 1000UL; // 30 seconds

// pfodDelay (pfodParser/src/pfodDelay.h), not a plain millis()/nextAttemptAtMs
// comparison -- see GatewayPollTask.cpp's identical comment on its own
// pollTimer for why, and for the justFinished() usage idiom
// (https://www.forward.com.au/pfod/ArduinoProgramming/TimingDelaysInArduino.html)
// followed here.
//
// Doubles as the sole "is a fetch attempt currently in flight" state --
// there is no separate attemptInProgress flag (removed 2026-09-22, per
// explicit instruction): !isRunning() means in flight, isRunning() means
// waiting (either for the uptime floor on the very first attempt, or a
// retry/refresh delay on every later one). This works because the two
// states are kept perfectly in sync at every transition: dispatching a
// new attempt explicitly stop()'s this timer (checkEnlightenHistory()),
// and finishEnlightenHistoryAttempt() always restarts it (one way or
// another) the moment that attempt resolves -- so it is NEVER left
// running during an in-flight attempt, nor stopped while merely waiting.
// Started once, in initializeEnlightenHistoryRefresh() (called from
// setup()), with MIN_UPTIME_BEFORE_HISTORY_MS as its very first delay --
// deliberately NOT left unstarted until some later "ready" latch, so
// isRunning() reads true (matching "waiting", not "in flight") from the
// very first tick onward, rather than misreading a never-started timer
// the same as a deliberately-stopped one.
static pfodDelay attemptTimer;

// Copies every point history holds (index 0 .. numPoints-1) into 'real',
// overwriting whatever was there (history is authoritative for the
// elapsed-day range it covers -- dummy-fill placeholder or stale forecast
// alike). Never shrinks 'real's numPoints, so any later/further point a
// live refresh (GatewayRefresh.cpp/SolarForecastRefresh.cpp) already
// wrote is left alone.
//
// Never copies past "now"'s own resolved slot, regardless of what
// history.numPoints claims -- confirmed as a real, not hypothetical, risk
// (plots.jpg showed the Battery Charge line's ACTUAL (orange) colour
// extending to ~95% well past the true current ~60% reading, staying
// wrong until BatteryProjection.cpp's own next 5-min cycle happened to
// repaint those same slots back to black -- a race this recurring 15-min
// merge could re-lose at any time). No writer of an "actual" colour
// (GatewayRefresh.cpp writes only AT "now"; this is the only other one)
// may ever place actual-coloured data beyond the true elapsed point.
static void mergeHistoryInto(const PlotData &history, PlotData &real, int nowMinutes) {
  int nowIndex = (PlotData::nearestSlotMinutes(nowMinutes) - PlotData::START_MINUTES) / PlotData::STEP_MINUTES;
  int limit = history.numPoints;
  if (limit > nowIndex + 1) {
    limit = nowIndex + 1;
  }
  for (int i = 0; i < limit; i++) {
    real.points[i] = history.points[i];
  }
  if (limit > real.numPoints) {
    real.numPoints = limit;
  }
}

// Call once from setup(), before loop() ever calls checkEnlightenHistory()
// -- see BatterySolarMonitor.ino. Arms attemptTimer with the uptime floor
// as its very first delay, so checkEnlightenHistory()'s own unconditional
// justFinished() check naturally becomes true once that floor has
// elapsed, with no separate millis()/"ready yet" latch needed.
void initializeEnlightenHistoryRefresh() {
  attemptTimer.start(MIN_UPTIME_BEFORE_HISTORY_MS);
}

// Called once a background fetch attempt has just resolved -- merges the
// result if one's ready and the clock can still be trusted, otherwise
// schedules a retry. Pulled out of checkEnlightenHistory() itself purely
// to keep that function's own "in flight / not" shape readable; this is
// its only caller. Returns the delay to restart attemptTimer with,
// rather than calling start() itself at each of its own three return
// points -- the caller makes that one call, so "this always restarts the
// timer" (attemptTimer's own comment -- it's what makes isRunning()
// double as "an attempt is in flight") is visibly true from a single
// call site instead of three scattered ones.
static unsigned long finishEnlightenHistoryAttempt() {
  if (!enlightenHistoryReady()) {
    // That attempt failed -- schedule the next one.
    if (debugPtr) {
      debugPtr->print(F("EnlightenHistory: attempt failed, retrying in "));
      debugPtr->print(RETRY_DELAY_MS / 1000);
      debugPtr->println(F("s"));
    }
    return RETRY_DELAY_MS;
  }
  // enlightenHistoryReady() above already consumed its own one-shot ready
  // flag (EnlightenHistoryTask.cpp), so bailing out here doesn't leave
  // this fetch's result waiting to be merged later -- it's discarded, and
  // the retry re-fetches from Enlighten entirely. Rare (the clock would
  // have to lose sync in the moments between the fetch completing and
  // this check) and self-correcting, just not free -- accepted rather
  // than complicating the ready-flag protocol to support resuming a merge
  // later.
  if (missedSNTPupdate()) {
    return RETRY_DELAY_MS;
  }
  int nowMinutes = localTimeMinutesSinceMidnight();
  lockEnlightenHistory();
  mergeHistoryInto(enlightenSolarHistory, solarData, nowMinutes);
  mergeHistoryInto(enlightenConsumptionHistory, consumptionData, nowMinutes);
  mergeHistoryInto(enlightenBatteryHistory, batteryChargeData, nowMinutes);
  unlockEnlightenHistory();
  if (debugPtr) {
    debugPtr->println(F("EnlightenHistory: merged into solarData/consumptionData/batteryChargeData"));
  }
  return REFRESH_INTERVAL_MS; // recurring -- refresh again in 5 min
}

void checkEnlightenHistory() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  // Checked unconditionally, at the outermost level, on every single
  // call -- per pfodParserAI-guide.md sec 4, "the one rule that matters".
  // A true result here already stops the timer as a side effect -- see
  // attemptTimer's own comment for why that distinction (just-became-due
  // vs. already-stopped because an attempt is in flight) is exactly what
  // tells the two "not due" cases below apart. Every path inside this
  // block returns, so reaching the code after it already proves
  // justFinished() was true -- no need to name/re-check the result.
  if (!attemptTimer.justFinished()) {
    if (attemptTimer.isRunning()) {
      return; // waiting out the uptime floor (first attempt), or a retry/refresh delay (every later one)
    }
    // Not due, and not running -- a fetch attempt is currently in flight
    // (attemptTimer was explicitly stopped when it was dispatched, below).
    if (enlightenHistoryAttemptRunning()) {
      return; // still working
    }
    attemptTimer.start(finishEnlightenHistoryAttempt());
    return;
  }

  // A new attempt is due.  attemptTimer is stopped at this point
  //
  // missedSNTPupdate() also doubles as "has NTP ever actually succeeded"
  // -- covers BOTH the very first attempt (the uptime floor above has no
  // idea whether NTP has actually synced yet) and every later one (a
  // long WiFi outage can re-arm missedSNTPupdate() well after the very
  // first sync -- ntpSupport.cpp: no update for 70 min). Either way, the
  // fetch's date=/now cutoff needs a real clock, so just reschedule a
  // short retry rather than trusting a stale/absent one.
  if (missedSNTPupdate()) {
    attemptTimer.start(RETRY_DELAY_MS); // re-arm -- justFinished() above already consumed this firing
    return;
  }
  // resolveEnlightenHosts() does DNS -- main thread only. lwIP only ever
  // returns one address per lookup here, so this is re-run before every
  // attempt (not just the first) -- see EnlightenHistoryTask.h.
  resolveEnlightenHosts();
  requestEnlightenHistoryFetch();
}
// ================= end of EnlightenHistoryRefresh.cpp  file
