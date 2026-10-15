// BatteryProjection.cpp  file  =================
#include "BatteryProjection.h"
#include "PlotDataInstances.h"
#include "LocalTimeMinutes.h"
#include "BatteryConstants.h"
#include "AvgConsumptionRefresh.h"
#include "SolarForecastRefresh.h"
#include "DischargeToGridState.h" // dtgPolledEnabled()/dtgScheduleStartMinutes()/dtgScheduleEndMinutes()/dtgScheduleEndSoc()
#include "ChargeFromGridState.h" // cfgPolledEnabled()/cfgScheduleStartMinutes()/cfgScheduleEndMinutes()/cfgScheduleTargetSoc()
#include "ntpSupport.h"
#include <Arduino.h>
#include <pfodDebugPtr.h>
#include <pfodDelay.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// BATT_USABLE_KWH/BATT_BACKUP_SOC come from BatteryConstants.h (shared
// with Dwg_StatusApp.cpp). BATT_CHARGE_EFF/MAX_CHARGE_KW are only used
// here, so stay local -- match dailyPrediction/plot_today_solar.py's own
// values. Capacity is fixed here, not the gateway's live capacity
// estimate -- that needs an extra endpoint, /ivp/livedata/status, not
// fetched anywhere yet -- "simple v1", the same choice already made for
// the solar estimate.
static const float BATT_CHARGE_EFF = 0.93; // fraction of solar energy that enters the battery
static const float MAX_CHARGE_KW = 7.6;    // maximum battery charge rate (kW) at full BMS rate

// Per explicit instruction: while Enphase's own discharge-to-grid setting
// is on (DischargeToGridState.h's dtgPolledEnabled() -- the ACTUAL polled
// Enphase state, not the button's own allow/disallow latch, so this still
// shows the drop even in the "we tried to disable it but Enphase hasn't
// confirmed yet" case DischargeToGridTask.cpp's own runStatusPoll() is
// mid-correcting -- see that file's own comment on the interaction), the
// projection ignores the normal solar/consumption net-energy-balance for
// the DTG schedule window and instead forces a straight-line drop from
// whatever % it projected AT the window's start down to the schedule's
// OWN configured end SOC% (DischargeToGridState.h's dtgScheduleEndSoc() --
// Enphase's "limit" field, the same % the schedule's own edit page on
// enlighten.enphaseenergy.com shows/lets you set, not a hardcoded value
// here) BY the window's end, matching what a full discharge-to-grid
// export actually does to the battery regardless of solar/consumption
// that same window.
//
// Added 2026-10-13, per explicit instruction ("on the hot day the
// forecast SOC should show the expected charge from grid in a similar
// way to the discharge from grid when that is active"): the mirror-image
// CFG (charge-from-grid) override -- while Enphase's own chargeFromGrid
// setting is on (ChargeFromGridState.h's cfgPolledEnabled(), same "actual
// polled state" reasoning as dtgPolledEnabled() above), forces a
// straight-line RISE from whatever % was projected at the cfg window's
// start UP TO the cfg schedule's own configured target SOC%
// (ChargeFromGridState.h's cfgScheduleTargetSoc() -- Enphase's "limit"
// field for the cfg schedule, charged UP to rather than discharged DOWN
// to) BY the window's end. Both overrides share the same per-slot loop
// below (DTG checked first; the two windows are at different times of
// day in practice -- cfg mid-day, dtg evening -- so they're not expected
// to overlap, but DTG wins if they ever did).

static const uint8_t PREDICTED_COLOR = 0; // BLACK -- matches Dwg_BatteryPlot1App.cpp's convention

static const unsigned long PROJECTION_INTERVAL_MS = 1UL * 60UL * 1000UL; // 1 minute

// pfodDelay (pfodParser/src/pfodDelay.h), not plain millis() subtraction --
// see GatewayPollTask.cpp's identical comment on its own pollTimer, and for
// the isRunning()/justFinished() usage idiom
// (https://www.forward.com.au/pfod/ArduinoProgramming/TimingDelaysInArduino.html)
// followed here. Only the real PROJECTION_INTERVAL_MS throttle (see the
// "Only now, having actually committed..." comment further down) is a
// lasting arm -- start(0) below just re-fires justFinished() immediately on
// the next call, so a call that bails out early (soc<=0.0, forecast not
// ready yet) retries on the very next loop() tick instead of being stuck
// waiting out a throttle window it never really started.
static pfodDelay projectionTimer;

// Which gate (if any) last blocked a projection attempt -- lets the two
// "skipping" messages below print only on a CHANGE of reason (edge
// triggered), not every single call. Needed because checkBatteryProjection()
// now retries on every loop() call while blocked (see projectionTimer's own
// comment above -- that's what makes the FIRST real projection happen
// promptly once the data is ready), which would otherwise spam the same
// "no ... forecast painted yet" line hundreds of times a second.
enum BlockedReason { BLOCKED_NONE, BLOCKED_NO_CONSUMPTION_FORECAST, BLOCKED_NO_SOLAR_FORECAST };
static BlockedReason lastReportedReason = BLOCKED_NONE;

// Set (from InternetPollTask.cpp's shared background task, via
// requestBatteryProjectionRecompute() below) whenever DischargeToGridTask.cpp
// optimistically changes dtgPolledEnabled() (setDtgPolledEnabledAssumed(),
// DischargeToGridState.h) -- without this, the SOC plot's own DTG
// override above would keep showing whatever it last computed for up to
// another ~1min (PROJECTION_INTERVAL_MS) after an optimistic change,
// same staleness this project's own button-text fix (setDtgPolledEnabledAssumed()
// itself) already closed for the menu. Mutex-protected -- genuinely
// cross-thread, same two-caller situation as elsewhere in this project
// (e.g. LoadControlState.cpp's carV2hRecoveryPending). Cleared the
// moment checkBatteryProjection() picks it up.
static bool forceRecomputeNow = false;

static SemaphoreHandle_t forceRecomputeMutex = NULL;

static SemaphoreHandle_t forceRecomputeMutexHandle() {
  if (forceRecomputeMutex == NULL) {
    forceRecomputeMutex = xSemaphoreCreateMutex();
  }
  return forceRecomputeMutex;
}

void requestBatteryProjectionRecompute() {
  xSemaphoreTake(forceRecomputeMutexHandle(), portMAX_DELAY);
  forceRecomputeNow = true;
  xSemaphoreGive(forceRecomputeMutexHandle());
}

// The BMS allows full charge rate below 98% SOC; above that it tapers to
// roughly half -- matches plot_today_solar.py's _charge_rate_factor().
static float chargeRateFactor(float soc) {
  return (soc >= 98.0) ? 0.5 : 1.0;
}

void checkBatteryProjection() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  // Pick up any pending optimistic-change request BEFORE the normal
  // throttle pair below, forcing projectionTimer due right now on this
  // call -- see requestBatteryProjectionRecompute()'s own comment.
  // Doesn't itself call/skip justFinished(), so this doesn't disturb the
  // "one rule that matters" ordering the pair below still follows
  // unconditionally.
  xSemaphoreTake(forceRecomputeMutexHandle(), portMAX_DELAY);
  bool forced = forceRecomputeNow;
  forceRecomputeNow = false;
  xSemaphoreGive(forceRecomputeMutexHandle());
  if (forced) {
    projectionTimer.start(0);
  }

  // justFinished() must be checked unconditionally, at the outermost
  // level, before anything else can skip it (pfodParserAI-guide.md sec 4,
  // "the one rule that matters") -- so the missedSNTPupdate() gate below
  // comes AFTER this pair, not before it. Harmless either way (this
  // file's own start(0)/justFinished() idiom already means a blocked
  // pass just re-arms and re-fires next call regardless of which gate
  // blocked it -- see the "Only now, having actually committed..."
  // comment below), but checking it here regardless, rather than relying
  // on that, is what actually guarantees it.
  if (!projectionTimer.isRunning()) {
    projectionTimer.start(0);
  }
  if (!projectionTimer.justFinished()) {
    return;
  }

  if (missedSNTPupdate()) {
    return; // need a real "now" before projecting forward from it
  }

  int nowMinutes = localTimeMinutesSinceMidnight();
  float soc = batteryChargeData.valueAtTime(nowMinutes);
  if (soc <= 0.0) {
    return; // no starting point yet (before 5am/after 10pm, or nothing set)
  }

  // consumptionData's future slots are only ever real forecast data once
  // AvgConsumptionRefresh.cpp has actually PAINTED them from a genuine
  // multi-day average -- until then they're still whatever the boot
  // dummy-fill left (Dwg_ConsumptionPlot1App.cpp, a rough demo curve, not
  // measured or forecast). Projecting SOC forward from that would produce
  // a plausible-looking but fabricated result, so skip entirely rather
  // than plot a fake forecast -- matches consumptionData's own "no
  // forecast plotted" behaviour when no day succeeds
  // (AvgConsumptionRefresh.cpp).
  //
  // Deliberately checks avgConsumptionForecastReady(), NOT
  // avgConsumptionProfile.hasData() directly -- hasData() can already be
  // true mid-batch (as soon as the first of the up-to-5 days succeeds, on
  // the background task) well before consumptionData has actually been
  // painted, which let this run against consumptionData's still-unpainted
  // (dummy-fill leftover) values -- see AvgConsumptionRefresh.h.
  if (!avgConsumptionForecastReady()) {
    if (debugPtr && lastReportedReason != BLOCKED_NO_CONSUMPTION_FORECAST) {
      debugPtr->println(F("BatteryProjection: no average consumption forecast painted yet -- skipping SOC projection"));
    }
    lastReportedReason = BLOCKED_NO_CONSUMPTION_FORECAST;
    return;
  }

  // Same reasoning as the consumption check just above, for solarData's
  // future slots instead -- without this, solarData could still be
  // Dwg_SolarPlot1App.cpp's dummy-fill demo curve (correctly coloured
  // predicted now, but not real solar data) for however long it takes
  // SolarForecastTask.h's first fetch to land.
  if (!solarForecastReady()) {
    if (debugPtr && lastReportedReason != BLOCKED_NO_SOLAR_FORECAST) {
      debugPtr->println(F("BatteryProjection: no solar forecast painted yet -- skipping SOC projection"));
    }
    lastReportedReason = BLOCKED_NO_SOLAR_FORECAST;
    return;
  }

  lastReportedReason = BLOCKED_NONE;

  // Only now, having actually committed to projecting, start the
  // PROJECTION_INTERVAL_MS throttle -- previously this was set right after
  // the missedSNTPupdate() check, so an early boot-time call that got this
  // far but then bailed out on soc<=0.0 or a not-yet-ready forecast gate
  // still consumed the throttle window, needlessly delaying the FIRST
  // real projection by up to 5 more minutes once the data actually became
  // ready (confirmed in deviceLog.txt: both forecast gates were ready by
  // 14:18:15, but the projection didn't run until 14:22:42).
  projectionTimer.start(PROJECTION_INTERVAL_MS);

  const float fractionOfHour = PlotData::STEP_MINUTES / 60.0;

  // "Now" itself already has a value (soc, above, from
  // batteryChargeData.valueAtTime(nowMinutes)) -- the projection fills
  // every slot strictly after it. PlotData::nearestSlotMinutes() gives
  // the exact same slot valueAtTime() just read 'soc' from -- a
  // previous floor-based reimplementation here didn't always match (see
  // that method's own comment) and silently overwrote the just-read
  // actual/current reading with a projected one roughly half the time.
  int firstProjectedMinute = PlotData::nearestSlotMinutes(nowMinutes) + PlotData::STEP_MINUTES;

  // DTG (discharge-to-grid) override window -- see the comment above these
  // constants. Snapped to this line's own 15-min grid (PlotData::
  // nearestSlotMinutes()) so it lines up exactly with the slots the loop
  // below steps through. dtgWindowValid stays false (no override anywhere
  // in the loop, normal projection throughout) unless there's a real,
  // currently-enabled, non-degenerate window AND a real configured end
  // SOC% to work with -- never guesses either from a partial/unpolled
  // result.
  bool dtgWindowValid = false;
  int dtgStartSlot = -1;
  int dtgEndSlot = -1;
  float dtgEndSoc = 0.0;
  float socAtDtgStart = 0.0;
  bool haveSocAtDtgStart = false;

  if (dtgPolledEnabled()) {
    int rawStart = dtgScheduleStartMinutes();
    int rawEnd = dtgScheduleEndMinutes();
    float rawEndSoc = dtgScheduleEndSoc();
    if (rawStart >= 0 && rawEnd >= 0 && rawEndSoc >= 0.0) {
      dtgStartSlot = PlotData::nearestSlotMinutes(rawStart);
      dtgEndSlot = PlotData::nearestSlotMinutes(rawEnd);
      dtgEndSoc = rawEndSoc;
      dtgWindowValid = (dtgEndSlot > dtgStartSlot);
    }
  }

  if (dtgWindowValid && dtgStartSlot < firstProjectedMinute) {
    // The window already started before our first projected slot (DTG is
    // active right now) -- the drop's starting % is whatever this line
    // already holds there (an actual reading if dtgStartSlot is in the
    // past, matching batteryChargeData's own "actual so far" convention).
    // A still-unset slot reads back as -1.0/0.0 (PlotData.h's own "no
    // data" sentinels, see its valueAtTime() comment) -- treated as "no
    // real starting point", so the whole override is skipped rather than
    // interpolating from a fabricated 0%.
    socAtDtgStart = batteryChargeData.valueAtTime(dtgStartSlot);
    if (socAtDtgStart <= 0.0) {
      dtgWindowValid = false;
    } else {
      haveSocAtDtgStart = true;
    }
  }

  // dtgEndSlot < firstProjectedMinute means the window is already fully
  // in the past relative to this projection -- dtgWindowValid stays true
  // (nothing wrong with the window itself) but no loop iteration below
  // will ever match it, so this is excluded here purely to keep the log
  // line honest about whether the override will actually do anything.
  if (debugPtr && dtgWindowValid && dtgEndSlot >= firstProjectedMinute) {
    debugPtr->print(F("BatteryProjection: DTG window active ("));
    debugPtr->print(dtgStartSlot);
    debugPtr->print(F("-"));
    debugPtr->print(dtgEndSlot);
    debugPtr->print(F(" min-of-day) -- overriding projection to drop to "));
    debugPtr->print(dtgEndSoc, 1);
    debugPtr->println(F("% by its end"));
  }

  // CFG (charge-from-grid) override window -- mirror-image of the DTG
  // block just above (rise instead of drop, cfgScheduleTargetSoc() instead
  // of dtgScheduleEndSoc()); see the comment above these two blocks'
  // shared constants for the full reasoning.
  bool cfgWindowValid = false;
  int cfgStartSlot = -1;
  int cfgEndSlot = -1;
  float cfgTargetSoc = 0.0;
  float socAtCfgStart = 0.0;
  bool haveSocAtCfgStart = false;

  if (cfgPolledEnabled()) {
    int rawStart = cfgScheduleStartMinutes();
    int rawEnd = cfgScheduleEndMinutes();
    float rawTargetSoc = cfgScheduleTargetSoc();
    if (rawStart >= 0 && rawEnd >= 0 && rawTargetSoc >= 0.0) {
      cfgStartSlot = PlotData::nearestSlotMinutes(rawStart);
      cfgEndSlot = PlotData::nearestSlotMinutes(rawEnd);
      cfgTargetSoc = rawTargetSoc;
      cfgWindowValid = (cfgEndSlot > cfgStartSlot);
    }
  }

  if (cfgWindowValid && cfgStartSlot < firstProjectedMinute) {
    // Same "start from whatever this line already holds" reasoning as the
    // DTG block above.
    socAtCfgStart = batteryChargeData.valueAtTime(cfgStartSlot);
    if (socAtCfgStart <= 0.0) {
      cfgWindowValid = false;
    } else {
      haveSocAtCfgStart = true;
    }
  }

  if (debugPtr && cfgWindowValid && cfgEndSlot >= firstProjectedMinute) {
    debugPtr->print(F("BatteryProjection: CFG window active ("));
    debugPtr->print(cfgStartSlot);
    debugPtr->print(F("-"));
    debugPtr->print(cfgEndSlot);
    debugPtr->print(F(" min-of-day) -- overriding projection to rise to "));
    debugPtr->print(cfgTargetSoc, 1);
    debugPtr->println(F("% by its end"));
  }

  for (int t = firstProjectedMinute; t <= PlotData::END_MINUTES; t += PlotData::STEP_MINUTES) {
    if (dtgWindowValid && haveSocAtDtgStart && t > dtgStartSlot && t <= dtgEndSlot) {
      // Inside the DTG window, past its start -- straight-line drop from
      // socAtDtgStart to dtgEndSoc, reached exactly at dtgEndSlot. Replaces
      // the normal net-energy-balance update below entirely for these
      // slots. Clamped the same as the normal path below -- dtgEndSoc is
      // boundary data (Enphase's own "limit" field), not a trusted
      // constant, so it gets the same [BATT_BACKUP_SOC, 100] safety net.
      float fraction = (float)(t - dtgStartSlot) / (float)(dtgEndSlot - dtgStartSlot);
      soc = socAtDtgStart + (dtgEndSoc - socAtDtgStart) * fraction;
      if (soc < BATT_BACKUP_SOC) {
        soc = BATT_BACKUP_SOC;
      } else if (soc > 100.0) {
        soc = 100.0;
      }
      batteryChargeData.setPointAtTime(t, soc, PREDICTED_COLOR);
      continue;
    }

    if (cfgWindowValid && haveSocAtCfgStart && t > cfgStartSlot && t <= cfgEndSlot) {
      // Inside the CFG window, past its start -- mirror-image of the DTG
      // block above: straight-line RISE from socAtCfgStart to
      // cfgTargetSoc, reached exactly at cfgEndSlot. Same clamp, same
      // reasoning (cfgTargetSoc is boundary data too).
      float fraction = (float)(t - cfgStartSlot) / (float)(cfgEndSlot - cfgStartSlot);
      soc = socAtCfgStart + (cfgTargetSoc - socAtCfgStart) * fraction;
      if (soc < BATT_BACKUP_SOC) {
        soc = BATT_BACKUP_SOC;
      } else if (soc > 100.0) {
        soc = 100.0;
      }
      batteryChargeData.setPointAtTime(t, soc, PREDICTED_COLOR);
      continue;
    }

    float solarKw = solarData.valueAtTime(t);
    float consumeKw = consumptionData.valueAtTime(t);
    float netKw = solarKw - consumeKw;
    float eff;
    if (netKw > 0) {
      float maxCharge = MAX_CHARGE_KW * chargeRateFactor(soc);
      if (netKw > maxCharge) {
        netKw = maxCharge;
      }
      eff = BATT_CHARGE_EFF;
    } else {
      eff = 1.0; // discharging -- no efficiency loss applied, matches plot_today_solar.py
    }
    soc += netKw * eff * fractionOfHour / BATT_USABLE_KWH * 100.0;
    if (soc < BATT_BACKUP_SOC) {
      soc = BATT_BACKUP_SOC;
    } else if (soc > 100.0) {
      soc = 100.0;
    }
    batteryChargeData.setPointAtTime(t, soc, PREDICTED_COLOR);

    // The DTG window starts AT or AFTER this projected slot (not before
    // it, handled above) -- capture the normally-projected % right here,
    // at dtgStartSlot itself, as the drop's own starting point for every
    // later slot in the loop. Per explicit instruction: the drop starts
    // FROM the current/forecast % at the start time, so this slot's own
    // value is deliberately left as the normal projection computed it,
    // not overridden.
    if (dtgWindowValid && !haveSocAtDtgStart && t == dtgStartSlot) {
      socAtDtgStart = soc;
      haveSocAtDtgStart = true;
    }
    // Same capture, mirror-image, for the CFG window's own start.
    if (cfgWindowValid && !haveSocAtCfgStart && t == cfgStartSlot) {
      socAtCfgStart = soc;
      haveSocAtCfgStart = true;
    }
  }

  if (debugPtr) {
    debugPtr->print(F("BatteryProjection: projected from "));
    debugPtr->print(nowMinutes / 60);
    debugPtr->print(':');
    if ((nowMinutes % 60) < 10) {
      debugPtr->print('0');
    }
    debugPtr->print(nowMinutes % 60);
    debugPtr->print(F(" soc="));
    debugPtr->print(batteryChargeData.valueAtTime(nowMinutes), 1);
    debugPtr->print(F("% to 22:00 soc="));
    debugPtr->print(soc, 1);
    debugPtr->println(F("%"));
  }
}
// ================= end of BatteryProjection.cpp  file
