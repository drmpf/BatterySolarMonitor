// LoadControlTask.cpp  file  =================
// Ports dailyPrediction/plot_today_solar.py's car-charger/hot-water
// auto-control (control_loads() and the 4 phases it orchestrates --
// hw_check_stop(), car_check_start(), hw_check_start(), car_check_stop(),
// in that exact order) to the ESP32, 1:1 in decision logic. See
// dailyPrediction/docs/{car_charger_control,hot_water_control,
// solar_load_control}.md and the plan in memory
// (project_car_hw_load_control_plan.md) for the full background research
// this is built from.
//
// Two deliberate simplifications from the Python original (both explained
// fully in LoadControlState.h): only the ownership ("AutoStarted") flags
// are LittleFS-persisted (the plan's already-resolved design question --
// Python calls these hta_started_charging/hta_started_hw, "hta" being its
// own abbreviation for "started by the HTA automation dashboard"; renamed
// to AutoStarted here since there's no separate HTA process on the ESP32,
// this task IS the automation); Python's no_ac_until 15min no-240VAC
// cooldown is a RAM-only pfodDelay timer here instead of an NTP-synced
// epoch timestamp -- carNoAcCooldownTimer below, queried via
// isRunning() (a non-mutating "is this window still open" check -- see
// pfodParserAI-guide.md), not justFinished() (which would consume the
// one-shot edge on the first query and never report "still active"
// again). Python's other transient window, started_at's 10s startup
// grace, was NOT ported -- removed 2026-09-10 after confirming it was
// unreachable dead code here: carCheckStart()/carCheckStop() run in the
// SAME checkLoadControlPoll() invocation, and carCheckStart() sets
// ctx.done=true on the cycle it starts the car, so carCheckStop() always
// skips the just-started cycle outright (see its own "car just started
// this cycle" branch) regardless of any grace timer. The next cycle that
// could reach the 240VAC check is at minimum EARLY_RECHECK_MS (30s) later
// -- already well past any 10s grace AND past CarChargerTask.cpp's own
// 20s post-toggle recheck (requestCarChargerRecheck()), which is what
// actually provides real settling-time protection by then. Python needed
// its own grace because car_check_stop() could run moments after
// car_check_start() on a frequent HTA-triggered invocation; that
// same-cycle case doesn't exist here.
//
// Also not reproduced: Python's CAR_CHECK_SOON flag file (asks a separate
// HTA front-end process to re-run ~1min after starting the car, instead
// of waiting the normal 5min, so the 240VAC check fires sooner). There is
// no separate front-end process here -- this task IS the whole control
// loop -- so the direct equivalent is just re-arming this task's own
// pollTimer for EARLY_RECHECK_MS (30s) instead of POLL_INTERVAL_MS right
// after a car start (see the bottom of checkLoadControlPoll()).
//
// ── ENABLE_LOAD_CONTROL -- READ THIS BEFORE FLIPPING IT ────────────────────
// Defined in BatterySolarMonitor.ino (extern'd via LoadControlTask.h), not
// as a local const here -- a physical-hardware safety switch belongs up
// front with the sketch's other top-level settings (the WiFi config LED
// pin, ENABLE_LOAD_CONTROL itself), not buried in one .cpp file. Defaults
// to false there.
// dailyPrediction/plot_today_solar.py is STILL the live controller for
// both devices as of this port landing -- it was given its own
// ENABLE_LOAD_CONTROL switch specifically so it could be handed off.
// Running two independent controllers against the same physical car
// charger/hot water element at once would have them fighting each other:
// each polls on its own schedule, reads a possibly-different momentary
// SOC/state, and can each believe IT owns a load the other just changed.
// Flip the .ino's flag to true ONLY once plot_today_solar.py's own
// ENABLE_LOAD_CONTROL has been set to False, so exactly one controller is
// ever live at a time.
#include "LoadControlTask.h"
#include "LoadControlState.h"
#include "BatteryChargeSettings.h"
#include "HotDayState.h" // effectiveHwSocOnPercent()/effectiveHwSocOffPercent()/effectiveEvSocOnPercent()/effectiveEvSocOffPercent()
#include "CarChargerFetch.h"
#include "CarChargerInstances.h"
#include "CarChargerTask.h"
#include "HotWaterFetch.h"
#include "HotWaterInstances.h"
#include "HotWaterTask.h"
#include "GatewayInstances.h"
#include "GatewayPollTask.h"
#include "LoadStatusFormat.h"
#include "LocalTimeMinutes.h"
#include "ntpSupport.h"
#include <Arduino.h>
#include <pfodDebugPtr.h>
#include <pfodDelay.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// Master safety switch -- defined in BatterySolarMonitor.ino (deliberately
// not a local const here: a physical-hardware safety decision belongs up
// front with the sketch's other top-level settings where anyone opening
// it sees it, not buried in this one .cpp file). Declared extern here rather than
// in LoadControlTask.h because this is the only file that reads it --
// InternetPollTask.cpp (the only other includer of LoadControlTask.h)
// only ever calls checkLoadControlPoll() and has no reason to know this
// variable exists, so it stays out of that header's public interface. See
// the .ino's own comment on this variable, and this file's top-of-file
// comment, before ever changing it.
extern bool ENABLE_LOAD_CONTROL;

// Per-load turn-on/turn-off SOC% thresholds (added 2026-09-21, per explicit
// instruction, replacing the single shared SOC_CONTROL_START/SOC_CONTROL_STOP
// pair this used to be -- both loads used the same 35%/30% figures,
// confirmed 2026-09-08 against dailyPrediction/plot_today_solar.py's LIVE
// script, not the stale 92%/85% figures in the docs). Now independently
// settable per load (HW vs EV/car), LittleFS-persisted, and live-editable
// via the pfodWeb "HW/EV - Battery Charge%" sub-menu's two sliders -- see
// BatteryChargeSettings.h. Each load's turn-off threshold is always exactly
// 5% below its own turn-on threshold (BatteryChargeSettings.h's own
// OFF_BELOW_ON), not independently stored.

// Deliberately LONGER than CarChargerTask.cpp's/HotWaterTask.cpp's own 5min
// POLL_INTERVAL_MS (added 2026-09-19, per explicit instruction) -- this
// file's own timer barely matters in the common case, since
// CarChargerTask.cpp's "Sync with LoadControlTask.cpp" (see
// esp32_control_structure.md sec 3) re-arms it for ~0ms after every
// successful car fetch, so this timer only actually fires as a FALLBACK
// (charger unreachable, or before the first-ever sync). If this fallback
// interval were the SAME 5min as CarChargerTask.cpp's own cadence, it
// could race a fetch due at almost the same moment and run against data
// about to be superseded moments later -- exactly the kind of staleness
// this whole sync mechanism was added to close. Being strictly longer
// guarantees that by the time this fallback ever DOES fire,
// CarChargerTask.cpp has already had a full cycle to complete and
// publish, so even the fallback path sees CarChargerTask.cpp's freshest
// available data, not a stale reading racing an in-flight fetch. Was
// exactly 5min (matching dailyPrediction/plot_today_solar.py's own
// control_loads() cadence) before this margin was added.
static const unsigned long POLL_INTERVAL_MS = 5UL * 60UL * 1000UL + 30UL * 1000UL; // 5.5min
static const unsigned long EARLY_RECHECK_MS = 30UL * 1000UL;         // was 60s (matching CAR_CHECK_SOON's ~1min); shortened per explicit instruction
static const unsigned long CAR_NO_AC_COOLDOWN_MS = 15UL * 60UL * 1000UL; // matches no_ac_until's 15min pause

// Max V2H-relevant load (kW, see v2hRelevantLoadKw()) below which a V2H
// recovery attempt (see carCheckStart()'s own V2H section) will retry
// turning V2H back on -- added 2026-09-11, per explicit instruction.
static const float V2H_RETRY_MAX_LOAD_KW = 3.0;

// How soon to re-run THIS file's own poll cycle after a V2H recovery
// toggle-on, instead of waiting out the normal POLL_INTERVAL_MS: if the
// recovery attempt just re-trips V2H (v2hActive && !ac240vActive again),
// that needs to be noticed and a fresh off-toggle sent promptly, not left
// sitting mis-reported as "recovered" for up to 5 more minutes.
// Deliberately LONGER than CarChargerTask.cpp's own V2H_RECHECK_DELAY_MS
// (30s) -- that timer governs when the REAL post-toggle /api/state gets
// fetched; this one must fire strictly after that fetch has actually
// landed, or this file would just see its own stale optimistic guess
// (v2hActive=true, ac240vActive=true) instead of the real result.
static const unsigned long V2H_RECOVERY_RECHECK_MS = 35UL * 1000UL;

static pfodDelay pollTimer;
static pfodDelay carNoAcCooldownTimer;  // isRunning() while still inside the 15min no-240VAC pause

// True from the moment a V2H trip is detected (v2hActive && !ac240vActive)
// and V2H is toggled off, until a later cycle successfully toggles it back
// on (or gives up -- it never does, it just keeps retrying every cycle
// while the load stays too high). LittleFS-persisted (LoadControlState.h's
// carV2hRecoveryPending()/setCarV2hRecoveryPending() -- changed from the
// original RAM-only design 2026-09-11, per explicit instruction, so an
// unscheduled reboot mid-recovery doesn't forget V2H was just switched
// off) -- read/written directly via those accessors below, no local
// duplicate kept here. Published into CarChargerInstances.h's
// carInV2hRecovery (see the "Publish the AutoStarted ownership flags"
// block below) so the display can keep showing "V2H tripped off"
// throughout, even though v2hActive itself reads false while pending.

// onPercent>=99 means "disabled" (matches the HW/EV Battery Charge% slider's
// own prompt text, SubMenu_HW_EV___Battery_Char.cpp: "100% settings => always
// off") -- never start, and turn off if it was already running, regardless
// of socPct/offPercent. Needs onPercent (not just offPercent) so a load left
// running from before the threshold was raised to 99/100 doesn't keep
// coasting on the normal offPercent<=socPct check until SOC happens to drop
// far enough -- the setting must take effect immediately.
static bool startConditionsMet(float socPct, int onPercent) {
  if (onPercent >= 99) {
    return false;
  }
  return socPct >= onPercent;
}
static bool keepConditionsMet(float socPct, int offPercent, int onPercent) {
  if (onPercent >= 99) {
    return false;
  }
  return socPct >= offPercent;
}

static String formatMinutesHHMM(int mins) {
  // buf sized for the compiler's own worst-case width check (each %02d
  // could print 3 chars for a negative value, e.g. "-59") even though
  // mins is always 0-1439 in practice (a time-of-day value) -- silences
  // -Wformat-truncation rather than fighting it.
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d", (mins / 60) % 24, mins % 60);
  return String(buf);
}

// True ONLY when the charger's OWN v2h_auto schedule can be POSITIVELY
// CONFIRMED to want V2H on right now -- i.e. V2H recovery (carCheckStart()'s
// own retry branch, below) is attempted ONLY when this returns true; any
// doubt at all (auto disabled, an unset/degenerate schedule, no synced
// clock, or simply outside the window) means "can't confirm, don't
// recover", per explicit instruction (2026-09-15 -- reversed from this
// function's first version, which defaulted to "allowed unless confirmed
// off"). EV_Charger_V2H_switch/switchSettings.cpp's own handleAuto()
// fully switches V2H on/off at v2h_time_on/v2h_time_off regardless of
// anything this sketch does -- retrying a recovery toggle-on without
// being sure the schedule currently agrees just fights it. This was the
// actual bug observed: a recovery left pending from an earlier real trip
// would see the schedule's own Auto Off (V2H genuinely off, house load
// typically low in the evening) and "recover" V2H straight back on
// again, undoing the schedule.
//
// Re-evaluated fresh every poll from THIS cycle's freshly-fetched
// car.v2hOnTimeMinutes/v2hOffTimeMinutes (never cached from whenever the
// recovery started) -- so per explicit instruction, either crossing the
// off-time boundary while a recovery is still pending, or editing the
// schedule's on/off times so the current time no longer confirms "on",
// cancels the recovery on the very next poll.
//
// 2 minutes of slack at each edge, per explicit instruction: an off time
// of 7:02 (or later) is needed before confirmation is withdrawn (7:00/7:01
// still confirm on), and an on time of 14:00 doesn't confirm on until
// 14:02 -- keeps this check clear of handleAuto()'s own per-minute tick,
// which only switches AT the configured minute, not before it.
static const int V2H_SCHEDULE_SLACK_MIN = 2;

static bool v2hScheduleConfirmsOn(const CarChargerState &car) {
  if (!car.v2hAutoActive) {
    return false; // schedule not in use -- nothing to confirm against
  }
  // Raw (un-slacked) window width, on-time to off-time, wrapping past
  // midnight if needed -- 0 for equal on/off times. Per explicit
  // instruction: equal times, or times within V2H_SCHEDULE_SLACK_MIN of
  // EACH edge (<= 2*slack apart overall) must NOT fall through to the
  // shifted-window math below -- shrinking a near-zero-width window
  // inward from BOTH ends flips onMin past offMin, which the wraparound
  // branch below would misread as a window covering nearly the whole
  // 24h, exactly backwards from the user's actual (tiny/no) window.
  // Treat anything this narrow as "nothing to confirm" instead.
  int rawGapMin = (car.v2hOffTimeMinutes - car.v2hOnTimeMinutes + 1440) % 1440;
  if (rawGapMin <= 2 * V2H_SCHEDULE_SLACK_MIN) {
    return false;
  }
  if (missedSNTPupdate()) {
    return false; // no real clock yet -- can't confirm "now" is in the window
  }
  int onMin = (car.v2hOnTimeMinutes + V2H_SCHEDULE_SLACK_MIN) % 1440;
  int offMin = (car.v2hOffTimeMinutes + 1440 - V2H_SCHEDULE_SLACK_MIN) % 1440;
  int mins = localTimeMinutesSinceMidnight();
  return (onMin < offMin) ? (mins >= onMin && mins < offMin)
                          : (mins >= onMin || mins < offMin);
}

// Margin (added 2026-09-19, per explicit instruction) purely to allow for
// clock mismatch between this ESP32 and the charger's own clock -- used
// ONLY by nearV2hWindow() below, to gate whether carCheckStart() may START
// a brand NEW charge session. Same numeric value as V2H_SCHEDULE_SLACK_MIN
// above by coincidence, not by relation -- kept as its own named constant
// since the two serve opposite purposes (see nearV2hWindow()'s own
// comment) and must stay free to diverge if either ever needs retuning.
static const int CAR_START_V2H_MARGIN_MIN = 2;

// True if "now" falls within the charger's OWN v2h_auto on/off window,
// WIDENED by CAR_START_V2H_MARGIN_MIN at EACH edge -- added 2026-09-19,
// per explicit report/instruction. Confirmed via device log
// (streaming_2026-09-19T04-05-33-646Z.txt): carCheckStart()'s "3. Start"
// decision ran against a car.v2hActive reading up to a full poll-interval
// stale (CarChargerTask.cpp's own poll cadence was never phase-aligned
// with this file's -- see requestLoadControlPollNow()'s own comment,
// LoadControlTask.h, for the general sync fix landing alongside this
// check) -- v2hActive can still read false right up until the charger's
// OWN handleAuto() actually flips it, which can be seconds to minutes
// after v2h_time_on. Starting a NEW charge session in that gap puts a real
// physical relay in direct conflict with the charger's own V2H safety
// circuit about to (or having just) taken over the same AC output.
//
// Deliberately the OPPOSITE shape from v2hScheduleConfirmsOn() above,
// which is for V2H RECOVERY and narrows its confirm-on window INWARD by
// slack specifically to stay clear of handleAuto()'s own per-minute tick
// (a false NEGATIVE there just means "don't recover yet, try again next
// cycle" -- cheap). Here the risk runs the other way -- missing the
// window would let a start happen right at the boundary -- so this widens
// OUTWARD instead, blocking a bit early and releasing a bit late.
//
// Fail-safe DIRECTION also deliberately differs: with no synced clock
// (missedSNTPupdate()), this returns false (don't block) rather than
// true -- an un-synced clock right after a reboot must not permanently
// prevent charging whenever SOC is otherwise high enough; that would be a
// worse regression than the narrow timing race this guards against.
static bool nearV2hWindow(const CarChargerState &car) {
  if (!car.v2hAutoActive) {
    return false; // schedule not in use -- nothing to block against
  }
  if (missedSNTPupdate()) {
    return false; // can't confirm "now" -- see comment above
  }
  int onMin = (car.v2hOnTimeMinutes - CAR_START_V2H_MARGIN_MIN + 1440) % 1440;
  int offMin = (car.v2hOffTimeMinutes + CAR_START_V2H_MARGIN_MIN) % 1440;
  int mins = localTimeMinutesSinceMidnight();
  return (onMin < offMin) ? (mins >= onMin && mins < offMin)
                          : (mins >= onMin || mins < offMin);
}

static void logLine(const String &line) {
  if (debugPtr) {
    debugPtr->println(line);
  }
}

// Every actual toggle in this file goes through one of these two, not the
// raw toggleCarCharger()/toggleHotWater() directly -- the single point
// ENABLE_LOAD_CONTROL is enforced. A suppressed toggle reports the same
// "failed" outcome to its caller as a genuine network failure would --
// matching plot_today_solar.py's own choice (there, _car_toggle()/
// _hw_send_toggle() raise either way and every call site's existing
// except-branch already handles "toggle didn't happen" uniformly).
//
// Each also OPTIMISTICALLY updates CarChargerInstances.h/HotWaterInstances.h
// with the state the toggle was sent to PRODUCE ('intendedOn'), the
// moment the toggle command itself is confirmed sent -- deliberately NOT
// by re-fetching and reading the device back immediately. The physical
// devices take a few seconds (hot water) up to ~8s (car charger) to
// actually complete the transition, so an immediate re-fetch would very
// likely catch the device still mid-transition and read back the STALE
// PRE-toggle value -- publishing that with false confidence is worse than
// leaving the old (still correctly stale-labelled) cached value alone.
// Without SOME immediate update, though, the Status screen
// (Dwg_StatusApp.cpp's idx_car/idx_hw) keeps showing the pre-toggle state
// until CarChargerTask.cpp's/HotWaterTask.cpp's own INDEPENDENT 5-min
// poll happens to run again (confirmed: device log showed "HW start: ...
// STARTED" immediately followed by the Status screen still reading
// "HW: off"). Each call site already knows which way it's driving the
// device, so writing that assumed value directly is correct without
// waiting out the transition or re-reading anything.
//
// The assumption covers BOTH fields, not just the primary switch --
// turning on assumes ac240vActive/powered come on too (per explicit
// instruction), and turning off assumes they go off too. Leaving the
// secondary field stale on a stop would have been actively wrong, not
// just optimistic: chargerOn=false with a stale ac240vActive=true trips
// formatCarState()'s own FAULT branch (LoadStatusFormat.h/.cpp -- no
// normal path has 240VAC present while the relay is open), so this also
// closes a real spurious-FAULT-after-stop bug, not only the requested
// on-branch behaviour.
//
// Each also calls requestCarChargerRecheck()/requestHotWaterRecheck()
// (CarChargerTask.h/HotWaterTask.h) so the REAL state gets confirmed
// promptly too, not just assumed indefinitely: those force that device's
// own independent poll to fire again in ~20s (comfortably past both
// devices' worst-case transition time) instead of waiting out the rest of
// the normal 5-min interval, so the optimistic guess above only ever
// stands uncorrected for a short window.
static bool toggleCarChargerIfEnabled(bool intendedOn) {
  if (!ENABLE_LOAD_CONTROL) {
    logLine("  Car charger: ENABLE_LOAD_CONTROL is False -- toggle suppressed");
    return false;
  }
  bool ok = toggleCarCharger();
  if (ok) {
    lockCarChargerState();
    latestCarChargerState.chargerOn = intendedOn;
    latestCarChargerState.ac240vActive = intendedOn;
    unlockCarChargerState();
    requestCarChargerRecheck();
  }
  return ok;
}

// Same shape as toggleCarChargerIfEnabled() above, but for V2H mode
// (POST /v2hPower, added 2026-09-11) rather than the main charge relay --
// see carCheckStart()'s own V2H section for the full recovery sequence
// this is used in. Turning V2H OFF (intendedOn=false) leaves
// ac240vActive untouched (it's already false -- that's the trip
// precondition). Turning V2H back ON (intendedOn=true) optimistically
// sets ac240vActive=true too, "assuming it works and that AC comes on"
// per explicit instruction -- matching toggleCarChargerIfEnabled()'s own
// established optimistic-update convention; the caller separately
// requests an accelerated recheck (requestCarChargerV2hRecheck(), ~30s --
// longer than this function's own recheck since V2H itself takes ~16s to
// take effect) to confirm the REAL state shortly after. Deliberately does
// NOT call requestCarChargerRecheck()/requestCarChargerV2hRecheck() itself
// -- the OFF step doesn't need one (the next recovery check is a full
// 5-min cycle away regardless, per explicit instruction), and the ON
// step's caller requests its own with the longer V2H-specific delay.
static bool toggleV2hIfEnabled(bool intendedOn) {
  if (!ENABLE_LOAD_CONTROL) {
    logLine("  Car V2H: ENABLE_LOAD_CONTROL is False -- toggle suppressed");
    return false;
  }
  bool ok = toggleV2h();
  if (ok) {
    lockCarChargerState();
    latestCarChargerState.v2hActive = intendedOn;
    if (intendedOn) {
      latestCarChargerState.ac240vActive = true;
    }
    unlockCarChargerState();
  }
  return ok;
}

static bool toggleHotWaterIfEnabled(bool intendedOn) {
  if (!ENABLE_LOAD_CONTROL) {
    logLine("  Hot water: ENABLE_LOAD_CONTROL is False -- toggle suppressed");
    return false;
  }
  bool ok = toggleHotWater();
  if (ok) {
    lockHotWaterState();
    latestHotWaterState.manualOn = intendedOn;
    latestHotWaterState.powered = intendedOn;
    unlockHotWaterState();
    requestHotWaterRecheck();
  }
  return ok;
}

// ── Phase 1: HW stop ────────────────────────────────────────────────────────
// Ported from hw_check_stop(). Returns true if a stop toggle was sent (or
// attempted -- matches Python's own hw_stopped semantics, which is set
// from the toggle's try/except outcome) this cycle -- gates hw_check_start()'s
// "skip if HW stopped this cycle" rule and carCheckStop()'s "defer stop
// one cycle" rule below.
static bool hwCheckStop(bool haveBattery, float socPct, bool hwReachable, const HotWaterState &hw) {
  if (!haveBattery) {
    logLine("  HW auto-control: no battery data");
    return false;
  }
  if (!hwReachable) {
    logLine("  HW auto-control: no HW status");
    return false;
  }

  int hwOnPercent = effectiveHwSocOnPercent();
  int hwOffPercent = effectiveHwSocOffPercent();
  bool autoStarted = hwAutoStarted();
  bool startCond = startConditionsMet(socPct, hwOnPercent);
  bool keepHw = keepConditionsMet(socPct, hwOffPercent, hwOnPercent);
  bool autoHwOn = hw.powered && !autoStarted && !hw.manualOn;

  String inputs = String("  HW stop:  auto=") + (autoStarted ? "T" : "F")
      + "  manual=" + (hw.manualOn ? "T" : "F")
      + (autoHwOn ? "  AutoHW=on" : "")
      + "  start=" + (startCond ? "YES" : "no")
      + "  keep=" + (keepHw ? "YES" : "no");

  // We started it but manual_on is now False -- turned off externally
  // (or the controller's own 1hr auto-off fired) -- clear the flag.
  if (autoStarted && !hw.manualOn) {
    setHwAutoStarted(false);
    logLine(inputs + "  -> flag cleared (manual_on went False -- turned off externally)");
    return false;
  }

  // User-manual (not started by us) -- leave alone.
  if (hw.manualOn && !autoStarted) {
    logLine(inputs + "  -> no action (user manual -- not started by us)");
    return false;
  }

  // Running and we started it -- check keep conditions.
  if (autoStarted && hw.manualOn) {
    if (!keepHw) {
      bool ok = toggleHotWaterIfEnabled(false); // turning it off
      setHwAutoStarted(false);
      logLine(inputs + "  -> " + (ok ? "STOPPED" : "failed to stop")
          + " (SOC<" + String(hwOffPercent) + "% (" + String((int)(socPct + 0.5)) + "%))");
      return ok;
    }
    logLine(inputs + "  -> keeping on (SOC=" + String((int)(socPct + 0.5)) + "%>=" + String(hwOffPercent) + "%)");
    return false;
  }

  // HW not running -- nothing to stop.
  logLine(inputs + "  -> no action (not running)");
  return false;
}

// ── Phase 3: HW start ───────────────────────────────────────────────────────
// Ported from hw_check_start(). hwAnyStopped=true if hwCheckStop() stopped
// HW this same cycle -- HW is not re-started on the same poll in that case.
static void hwCheckStart(bool haveBattery, float socPct, bool hwReachable, const HotWaterState &hw, bool hwAnyStopped) {
  if (!haveBattery || !hwReachable) {
    return; // matches Python: no log line at all in this case
  }
  if (hwAnyStopped) {
    logLine("  HW start:  -> skipped (HW stopped this cycle -- re-evaluate next poll)");
    return;
  }

  int hwOnPercent = effectiveHwSocOnPercent();
  bool autoStarted = hwAutoStarted();
  bool startCond = startConditionsMet(socPct, hwOnPercent);

  String inputs = String("  HW start:  auto=") + (autoStarted ? "T" : "F")
      + "  manual=" + (hw.manualOn ? "T" : "F")
      + "  start=" + (startCond ? "YES" : "no");

  if (autoStarted) {
    logLine(inputs + "  -> no action (already running -- auto=T, handled by HW stop check)");
    return;
  }
  if (hw.manualOn) {
    logLine(inputs + "  -> no action (user manual -- not started by us)");
    return;
  }

  if (startCond) {
    bool ok = toggleHotWaterIfEnabled(true); // turning it on
    if (ok) {
      setHwAutoStarted(true);
    }
    logLine(inputs + "  -> " + (ok ? "STARTED" : "failed to start")
        + " (SOC=" + String((int)(socPct + 0.5)) + "%>=" + String(hwOnPercent) + "%)");
    return;
  }

  logLine(inputs + "  -> no start (SOC=" + String((int)(socPct + 0.5)) + "%<" + String(hwOnPercent) + "%)");
}

// car_state, mirroring the Python car_state string/False/None union --
// used here purely for carCheckStop()'s own "why was I skipped" logging,
// not published anywhere (the Status screen's idx_car still reads raw
// CarChargerInstances state directly, unaffected by this file).
enum CarState { CAR_STATE_NONE, CAR_STATE_AUTO, CAR_STATE_EXT, CAR_STATE_COOLDOWN, CAR_STATE_V2H, CAR_STATE_V2H_WINDOW, CAR_STATE_OFF, CAR_STATE_UNREACHABLE };

// Ctx shared between carCheckStart() and carCheckStop() -- mirrors
// car_check_start()'s own ctx dict.
struct CarCtx {
  bool done = false;
  CarState carState = CAR_STATE_NONE;
  bool carStarted = false;
  // True only when THIS cycle sent a successful V2H recovery toggle-on --
  // checkLoadControlPoll() uses this to re-arm its own poll sooner
  // (V2H_RECOVERY_RECHECK_MS) so a possible re-trip gets caught promptly.
  // See carCheckStart()'s own V2H section.
  bool v2hRecoveryAttempted = false;
  float socPct = 0.0;
  // EV/car's own turn-on/turn-off SOC% thresholds this cycle
  // (HotDayState.h's effectiveEvSocOnPercent()/effectiveEvSocOffPercent())
  // -- captured here so carCheckStop()'s log lines use the same values
  // carCheckStart() actually decided against, not a re-read that could
  // in principle differ if a slider moved mid-cycle.
  int onPercent = 0;
  int offPercent = 0;
  bool autoStarted = false;
  bool keepCharging = false;
  bool chargerOn = false;
  bool ac240Active = false;
};

// ── Phase 2: car start ──────────────────────────────────────────────────────
// Ported from car_check_start().
static CarCtx carCheckStart(bool haveBattery, float socPct, bool carReachable, const CarChargerState &car, float v2hLoadKw) {
  CarCtx ctx;
  ctx.socPct = socPct;

  if (!haveBattery) {
    ctx.done = true;
    ctx.carState = CAR_STATE_UNREACHABLE;
    logLine("  Car charger: no battery data");
    return ctx;
  }

  int onPercent = effectiveEvSocOnPercent();
  int offPercent = effectiveEvSocOffPercent();
  ctx.onPercent = onPercent;
  ctx.offPercent = offPercent;
  bool autoStarted = carAutoStarted();
  ctx.autoStarted = autoStarted;
  bool conditionsMet = startConditionsMet(socPct, onPercent);
  bool keepCharging = keepConditionsMet(socPct, offPercent, onPercent);
  ctx.keepCharging = keepCharging;

  String inputs = String("  Car start:  SOC=") + String((int)(socPct + 0.5)) + "%";

  if (!carReachable) {
    ctx.done = true;
    ctx.carState = CAR_STATE_UNREACHABLE;
    logLine(inputs + "  -> ESP32 error: unreachable");
    return ctx;
  }
  ctx.chargerOn = car.chargerOn;
  ctx.ac240Active = car.ac240vActive;

  // 1. Cooldown check -- carNoAcCooldownTimer is armed 15min in
  // carCheckStop()'s no-240VAC-stop branch below.
  unsigned long cooldownRemainingMs = carNoAcCooldownTimer.remaining();
  if (cooldownRemainingMs > 0) {
    ctx.done = true;
    ctx.carState = CAR_STATE_COOLDOWN;
    logLine(inputs + "  -> cooldown: " + String(cooldownRemainingMs / 60000UL) + "min remaining (no 240VAC)");
    return ctx;
  }

  const char *chargerStateStr = car.chargerOn ? "on" : "off";

  // 2. V2H active, or recovering from a trip -- charging blocked either
  // way. (Python calls this "V2L" in its user-facing text despite reading
  // the same v2h_state field -- see Dwg_StatusApp.cpp's own comment on
  // that naming inconsistency; this file's internal log uses V2H
  // throughout, matching the wire field.)
  //
  // V2H trip recovery (added 2026-09-11, per explicit instruction):
  // v2h_state can stay "on" while ac240v_status goes "inactive" -- the
  // charger's own V2H safety circuit has tripped its AC output offline,
  // distinct from the car simply being unplugged (that's ac240vActive=
  // false with v2hActive=false too, handled entirely separately by
  // carCheckStop()'s no-AC-stop branch below). Recovery sequence:
  //   1. Trip detected (v2hActive && !ac240vActive) -> toggle V2H OFF
  //      (POST /v2hPower) so a LATER cycle can attempt to recover it;
  //      carV2hRecoveryPending marks this. Nothing else happens this
  //      cycle.
  //   2. NEXT normal 5-min cycle (no early recheck requested for this
  //      step -- 5 min is comfortably longer than the toggle's own
  //      settling time): if carV2hRecoveryPending && !v2hActive (confirms
  //      the OFF toggle actually took -- the car now just looks "off"),
  //      check v2hLoadKw (the whole-home metered load, MINUS HW's own
  //      assumed draw when it's powered -- see v2hRelevantLoadKw(),
  //      LoadStatusFormat.h/.cpp; HW is on a completely separate circuit
  //      from the car charger/V2H, per explicit correction, so its own
  //      consumption shouldn't count against V2H's own circuit capacity)
  //      -- if it's dropped below V2H_RETRY_MAX_LOAD_KW, toggle V2H back
  //      ON and assume it works (optimistic, matching
  //      toggleCarChargerIfEnabled()'s own established convention --
  //      including assuming ac240vActive comes back on too, per explicit
  //      instruction), then request an accelerated recheck in ~30s
  //      (longer than the normal post-toggle RECHECK_DELAY_MS (20s) in
  //      CarChargerTask.cpp, since V2H itself takes ~16s to actually take
  //      effect, per explicit instruction) so the real post-recovery
  //      state gets confirmed promptly -- and re-arms THIS file's own
  //      poll cycle sooner too (ctx.v2hRecoveryAttempted ->
  //      V2H_RECOVERY_RECHECK_MS at the bottom of checkLoadControlPoll()),
  //      so if the recovery attempt just re-trips V2H, that gets noticed
  //      and a fresh off-toggle sent promptly rather than sitting
  //      mis-reported as "recovered" for up to 5 more minutes -- per
  //      explicit instruction. If the load is still too high, stays
  //      pending and retries the same check again next cycle -- no
  //      toggle sent, nothing to undo.
  //   3. At EVERY retry of step 2, the load check is only even reached if
  //      v2hScheduleConfirmsOn() can POSITIVELY CONFIRM the charger's OWN
  //      v2h_auto schedule currently wants V2H on (added 2026-09-15, per
  //      explicit instruction -- any doubt at all, not just a confirmed
  //      off, blocks recovery). If not confirmed, carV2hRecoveryPending is
  //      cleared instead, standing down and leaving V2H to the schedule
  //      (which turns it back on itself at its own on-time). See that
  //      function's own comment for why this is needed and the 2-minute
  //      edge slack.
  // The display (LoadStatusFormat.h/.cpp) is told about carV2hRecoveryPending
  // via the published carInV2hRecovery (CarChargerInstances.h) so it keeps
  // showing "V2H tripped off" throughout the whole recovery attempt, even
  // though v2hActive itself reads false in the meantime.
  if (car.v2hActive && !car.ac240vActive) {
    // Written BEFORE the toggle is sent, per explicit instruction -- so a
    // crash/reboot landing between the two still leaves LittleFS reflecting
    // what this code was ABOUT to do (see LoadControlState.h's own comment).
    setCarV2hRecoveryPending(true);
    bool ok = toggleV2hIfEnabled(false);
    ctx.done = true;
    ctx.carState = CAR_STATE_V2H;
    logLine(inputs + "  charger=" + chargerStateStr + "  v2h=on  240vac=N  -> "
        + (ok ? "V2H tripped -- toggled off, will retry recovery next cycle"
               : "V2H tripped -- failed to toggle off"));
    return ctx;
  }
  if (car.v2hActive) {
    ctx.done = true;
    ctx.carState = CAR_STATE_V2H;
    logLine(inputs + "  -> V2H active, 240vac=Y -> exporting normally (charging blocked)");
    return ctx;
  }
  if (carV2hRecoveryPending()) {
    ctx.done = true;
    ctx.carState = CAR_STATE_V2H;
    if (!v2hScheduleConfirmsOn(car)) {
      // Same "write before acting" reasoning as the toggle branches below
      // -- clearing this now, rather than leaving it pending, means a
      // crash/reboot right after doesn't resume trying to fight the
      // schedule either. See v2hScheduleConfirmsOn()'s own comment.
      setCarV2hRecoveryPending(false);
      logLine(inputs + "  v2h=off (recovering)  v2hAuto=" + (car.v2hAutoActive ? "on " : "off ") + formatMinutesHHMM(car.v2hOnTimeMinutes) + "-" + formatMinutesHHMM(car.v2hOffTimeMinutes)
          + "  -> cannot confirm schedule wants V2H on now -- standing down, recovery cancelled");
    } else if (v2hLoadKw < V2H_RETRY_MAX_LOAD_KW) {
      // Same "write before toggle" reasoning as the off-toggle above --
      // reverted below if the toggle itself fails to even send, so
      // LittleFS never claims recovery is done when it isn't.
      setCarV2hRecoveryPending(false);
      bool ok = toggleV2hIfEnabled(true);
      String result;
      if (ok) {
        ctx.v2hRecoveryAttempted = true;
        requestCarChargerV2hRecheck();
        result = "toggled V2H on, assuming it works -- rechecking in 30s";
      } else {
        setCarV2hRecoveryPending(true); // toggle didn't send -- still pending
        result = "failed to toggle V2H on -- still pending, will retry next cycle";
      }
      logLine(inputs + "  v2h=off (recovering)  load=" + String(v2hLoadKw, 1) + "kW<"
          + String((int)V2H_RETRY_MAX_LOAD_KW) + "kW  -> " + result);
    } else {
      logLine(inputs + "  v2h=off (recovering)  load=" + String(v2hLoadKw, 1) + "kW>="
          + String((int)V2H_RETRY_MAX_LOAD_KW) + "kW  -> waiting (retry next cycle)");
    }
    return ctx;
  }

  // 3. Start
  if (conditionsMet && !car.chargerOn) {
    // 3a. Blocked near the charger's OWN v2h_auto on/off window (added
    // 2026-09-19) -- see nearV2hWindow()'s own comment for the full
    // reasoning. Checked only here, exactly when a start would otherwise
    // actually happen -- an ALREADY-running charge session going into this
    // window is carCheckStop()'s concern, not this guard's, and a low-SOC
    // "wouldn't have started anyway" cycle shouldn't log a misleading
    // "blocked by V2H window" reason.
    if (nearV2hWindow(car)) {
      ctx.done = true;
      ctx.carState = CAR_STATE_V2H_WINDOW;
      logLine(inputs + "  charger=" + chargerStateStr + "  v2hAuto=" + (car.v2hAutoActive ? "on " : "off ")
          + formatMinutesHHMM(car.v2hOnTimeMinutes) + "-" + formatMinutesHHMM(car.v2hOffTimeMinutes)
          + "  -> within +/-" + String(CAR_START_V2H_MARGIN_MIN) + "min of V2H window -- not starting");
      return ctx;
    }
    bool ok = toggleCarChargerIfEnabled(true); // turning it on
    ctx.done = true;
    if (ok) {
      setCarAutoStarted(true);
      ctx.carStarted = true;
      ctx.autoStarted = true;
      ctx.carState = CAR_STATE_AUTO;
      logLine(inputs + "  charger=" + chargerStateStr + "  -> STARTED (SOC="
          + String((int)(socPct + 0.5)) + "%>=" + String(onPercent) + "%)");
    } else {
      ctx.carState = autoStarted ? CAR_STATE_AUTO : (car.chargerOn ? CAR_STATE_EXT : CAR_STATE_OFF);
      logLine(inputs + "  charger=" + chargerStateStr + "  -> failed to start");
    }
    return ctx;
  }

  // 4. Conditions not met or charger already on -- log only, ctx.done
  // stays false so carCheckStop() runs its own keep/stop evaluation.
  String decision;
  if (car.chargerOn && autoStarted) {
    decision = "charger running (auto=T) -- keep/stop evaluated by car stop check";
  } else if (car.chargerOn) {
    decision = "charger already on (not started by us -- ext)";
  } else {
    decision = String("no start (SOC=") + String((int)(socPct + 0.5)) + "%<" + String(onPercent) + "%)";
  }
  logLine(inputs + "  charger=" + chargerStateStr + "  -> " + decision);
  return ctx;
}

// ── Phase 4: car keep/stop ──────────────────────────────────────────────────
// Ported from car_check_stop(). hwJustStopped=true if hwCheckStop()
// stopped HW this same cycle (defers the car's own stop check one cycle,
// so two loads never stop in the same poll).
static void carCheckStop(bool hwJustStopped, CarCtx &ctx) {
  if (ctx.done) {
    const char *skipReason;
    if (ctx.carStarted) {
      skipReason = "car just started this cycle";
    } else if (ctx.carState == CAR_STATE_COOLDOWN) {
      skipReason = "in cooldown";
    } else if (ctx.carState == CAR_STATE_V2H) {
      skipReason = "V2H active";
    } else if (ctx.carState == CAR_STATE_V2H_WINDOW) {
      skipReason = "near V2H window";
    } else {
      skipReason = "handled by car start check";
    }
    logLine(String("  Car stop:  -> skipped (") + skipReason + ")");
    return;
  }

  String inputs = String("  Car stop:  keep=") + (ctx.keepCharging ? "YES" : "no");

  // 3. Running checks (only when we started it and it's on).
  if (ctx.autoStarted && ctx.chargerOn) {
    // 3a. 240VAC check. No startup grace here -- carCheckStart() sets
    // ctx.done=true on the cycle it starts the car, so this function
    // never reaches this point on that same cycle (see the "car just
    // started this cycle" skip above); the next cycle that CAN reach here
    // is at minimum EARLY_RECHECK_MS (30s) later, well past
    // CarChargerTask.cpp's own 20s post-toggle recheck that refreshes the
    // real ac240Active value -- see this file's top-of-file comment.
    if (!ctx.ac240Active) {
      // Car not plugged in or charge complete -- turn off and pause 15min.
      // setCarAutoStarted(false) here means the display can no longer tell
      // "just auto-stopped for lack of 240VAC" apart from "genuinely never
      // charged today" from autoStarted/chargerOn/ac240vActive alone -- so
      // carNoAcCooldownTimer's own isRunning() window (published as
      // CarChargerInstances.h's carInNoAcCooldown, checked/updated at the
      // end of checkLoadControlPoll()) is what keeps formatCarState()
      // showing "Car: Auto off -- Fully charged or unplugged" for the
      // WHOLE 15-min cooldown below, not just this one cycle -- see
      // LoadStatusFormat.h's own table/comment for the full reasoning.
      bool ok = toggleCarChargerIfEnabled(false); // turning it off
      setCarAutoStarted(false);
      carNoAcCooldownTimer.start(CAR_NO_AC_COOLDOWN_MS);
      logLine(inputs + "  charger=on  auto=T  240vac=N  keep=" + (ctx.keepCharging ? "YES" : "no")
          + "  -> " + (ok ? "STOPPED" : "failed to stop")
          + " (no 240VAC -- car unplugged or charge complete, pausing 15min)");
      return;
    }

    String keepStr = String("keep=") + (ctx.keepCharging ? "YES" : "no");

    // 3b. If HW stopped this cycle, skip the car stop check entirely.
    if (hwJustStopped) {
      logLine(inputs + "  charger=on  auto=T  240vac=Y  hw_stopped=T  -> keep (HW stopped this cycle -- skipping car stop check)");
      return;
    }

    if (!ctx.keepCharging) {
      bool ok = toggleCarChargerIfEnabled(false); // turning it off
      setCarAutoStarted(false);
      String decision = ok
          ? (String("STOPPED (SOC<") + String(ctx.offPercent) + "% (" + String((int)(ctx.socPct + 0.5)) + "%))")
          : "failed to stop";
      logLine(inputs + "  charger=on  auto=T  240vac=Y  " + keepStr + "  -> " + decision);
      return;
    }

    // Keep conditions met -- still charging.
    logLine(inputs + "  charger=on  auto=T  240vac=Y  " + keepStr + "  -> charging (SOC="
        + String((int)(ctx.socPct + 0.5)) + "%>=" + String(ctx.offPercent) + "%)");
    return;
  }

  // 4. We started it but charger is now off externally.
  if (ctx.autoStarted && !ctx.chargerOn) {
    setCarAutoStarted(false);
    logLine(inputs + "  charger=off  auto=T  -> flag cleared (charger turned off externally)");
    return;
  }

  // 5. Charger on externally (auto=F, charger=on).
  if (ctx.chargerOn) {
    logLine(inputs + "  charger=on  auto=F  -> no action (charger on externally -- not started by us)");
    return;
  }

  // 6. Charger off, not started by us -- conditions not met.
  logLine(inputs + "  charger=off  auto=F  -> no action (not started by us)");
}

// Publishes the AutoStarted/cooldown/recovery ownership flags into the
// shared Instances and logs the "Status:"/"V2H sched:  end" lines --
// pulled out into its own function purely so checkLoadControlPoll()'s
// end-of-cycle code (below) reads as one call rather than a long inline
// block; it has no other caller.
static void publishAndLogLoadStatus() {
  lockCarChargerState();
  carChargerAutoStarted = carAutoStarted();
  carInNoAcCooldown = carNoAcCooldownTimer.isRunning();
  carInV2hRecovery = carV2hRecoveryPending();
  CarChargerState carNow = latestCarChargerState;
  bool carReachableNow = carChargerReachable;
  float carKwhNow = carChargingKwhToday;
  bool carAutoStartedNow = carChargerAutoStarted;
  bool carInNoAcCooldownNow = carInNoAcCooldown;
  bool carInV2hRecoveryNow = carInV2hRecovery;
  unlockCarChargerState();

  lockHotWaterState();
  hotWaterAutoStarted = hwAutoStarted();
  HotWaterState hwNow = latestHotWaterState;
  bool hwReachableNow = hotWaterReachable;
  bool hwAutoStartedNow = hotWaterAutoStarted;
  unlockHotWaterState();

  lockGatewayReading();
  float houseLoadKw = latestConsumeKw;
  unlockGatewayReading();
  float v2hLoadKwNow = v2hRelevantLoadKw(houseLoadKw, hwNow);

  logLine(String("  Status:  ") + formatCarState(carReachableNow, carNow, carKwhNow, carAutoStartedNow, carInNoAcCooldownNow, carInV2hRecoveryNow, v2hLoadKwNow)
      + "   |   " + formatHwState(hwReachableNow, hwNow, hwAutoStartedNow));

  logLine(String("  V2H sched:  end    auto=") + (carNow.v2hAutoActive ? "on" : "off")
      + "  on=" + formatMinutesHHMM(carNow.v2hOnTimeMinutes)
      + "  off=" + formatMinutesHHMM(carNow.v2hOffTimeMinutes)
      + "  recoveryPending=" + (carInV2hRecoveryNow ? "YES" : "no"));
}

void checkLoadControlPoll() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  if (!pollTimer.isRunning()) {
    pollTimer.start(0); // due immediately the first time InternetPollTask.cpp ticks this
  }
  if (!pollTimer.justFinished()) {
    return;
  }

  lockGatewayReading();
  float socPct = latestBatterySocPercent;
  float houseLoadKw = latestConsumeKw; // current whole-home load -- adjusted below via v2hRelevantLoadKw() before use
  unlockGatewayReading();
  bool haveBattery = socPct >= 0.0; // -1 sentinel -- see GatewayInstances.h

  lockCarChargerState();
  CarChargerState car = latestCarChargerState;
  bool carReachable = carChargerReachable;
  unlockCarChargerState();

  // V2H schedule + recovery-pending snapshot at the START of this cycle,
  // per explicit instruction -- added alongside the matching end-of-cycle
  // line below (search "V2H sched:  end") so a device log shows both the
  // schedule/pending state this cycle's decisions were made FROM and what
  // they left it as, without having to cross-reference the "Status:" line
  // or CarChargerFetch.cpp's own per-poll debug line.
  logLine(String("  V2H sched:  start  auto=") + (car.v2hAutoActive ? "on" : "off")
      + "  on=" + formatMinutesHHMM(car.v2hOnTimeMinutes)
      + "  off=" + formatMinutesHHMM(car.v2hOffTimeMinutes)
      + "  recoveryPending=" + (carV2hRecoveryPending() ? "YES" : "no"));

  lockHotWaterState();
  HotWaterState hw = latestHotWaterState;
  bool hwReachable = hotWaterReachable;
  unlockHotWaterState();

  // HW is on a completely separate circuit from the car charger/V2H (per
  // explicit correction) -- its own draw shouldn't count against V2H's
  // own circuit capacity, so it's subtracted out of the raw metered load
  // before the V2H recovery decision (below) ever sees it. See
  // v2hRelevantLoadKw()'s own comment (LoadStatusFormat.h/.cpp) for the
  // full reasoning and the HW_ELEMENT_LOAD_KW assumed-draw constant.
  float v2hLoadKw = v2hRelevantLoadKw(houseLoadKw, hw);

  // Poll cycle order matches control_loads() exactly: HW stop first (so a
  // HW stop this cycle can defer the car's own stop check), then car
  // start, then HW start (skipped if HW just stopped), then car
  // keep/stop.
  bool hwStopped = hwCheckStop(haveBattery, socPct, hwReachable, hw);
  CarCtx carCtx = carCheckStart(haveBattery, socPct, carReachable, car, v2hLoadKw);
  hwCheckStart(haveBattery, socPct, hwReachable, hw, hwStopped);
  carCheckStop(hwStopped, carCtx);

  // Publish the AutoStarted/cooldown/recovery ownership flags and log the
  // "Status:"/"V2H sched:  end" lines -- see publishAndLogLoadStatus()'s
  // own comment. Re-reads the shared instances rather than reusing the
  // 'car'/'hw' locals from above, since a toggle during any of the 4
  // phases just run may have OPTIMISTICALLY updated them already (see
  // toggleCarChargerIfEnabled()'s/toggleHotWaterIfEnabled()'s own comment)
  // -- this makes the log show the same "assumed new state" the Status
  // screen is now displaying, not the pre-toggle snapshot this cycle
  // started with.
  publishAndLogLoadStatus();

  // Direct equivalent of Python's CAR_CHECK_SOON: no separate front-end
  // process to signal here, so just re-arm THIS task's own timer sooner
  // instead of waiting the normal 5min, so the 240VAC grace-period check
  // (carCheckStop()'s 3a) gets evaluated again promptly. carCtx.v2hRecoveryAttempted
  // gets the same early-recheck treatment (V2H_RECOVERY_RECHECK_MS, added
  // 2026-09-11, per explicit instruction) -- see carCheckStart()'s own V2H
  // section for why: a recovery toggle-on can itself re-trip V2H, and that
  // needs to be noticed and reacted to promptly, not left sitting
  // mis-reported as "recovered" for up to 5 more minutes. In practice
  // CarChargerTask.cpp's own requestLoadControlPollNow() (called
  // unconditionally after EVERY successful fetch, added 2026-09-19 -- see
  // that file's own "Sync point" comment) now fires FIRST in the common
  // case and re-runs this whole cycle before this 35s timer even elapses --
  // V2H_RECOVERY_RECHECK_MS stays armed regardless, as a fallback in case
  // the charger is unreachable when it's due.
  unsigned long nextPollMs = POLL_INTERVAL_MS;
  if (carCtx.carStarted) {
    nextPollMs = EARLY_RECHECK_MS;
  } else if (carCtx.v2hRecoveryAttempted) {
    nextPollMs = V2H_RECOVERY_RECHECK_MS;
  }
  pollTimer.start(nextPollMs);
}

void requestLoadControlPollNow() {
  pollTimer.start(0); // due on InternetPollTask.cpp's very next tick -- same "start(0)" idiom checkLoadControlPoll() itself uses for its first-ever run
}
// ================= end of LoadControlTask.cpp  file
