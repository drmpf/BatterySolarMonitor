// LoadStatusFormat.h  file  =================
// Formats the car charger / hot water controller's current state as the
// same short human-readable text the Status screen shows (idx_car/idx_hw,
// Dwg_StatusApp.cpp) -- pulled out into its own file so LoadControlTask.cpp
// can log exactly what the Status screen is displaying right after each
// control decision, without duplicating (and risking drifting from) the
// display text.
//
// Attribution model (per explicit correction -- manualOn/chargerOn is just
// the raw ON/OFF the hardware monitor reports, NOT evidence a human did
// anything, since LoadControlTask.cpp's own toggle functions set that same
// bit when THEY turn a load on):
//   1. autoStarted=true  -> label "Auto". For HW, driven purely by powered
//      (heating or not) -- the raw manualOn latch is not even consulted; a
//      transient off-reading while autoStarted is still true self-corrects
//      within one control cycle (hwCheckStop() already detects and clears
//      it), so the display must not invent a "manual turned it off" story
//      for that. For the car, driven by ac240vActive when charging, but
//      the raw chargerOn reading IS consulted (added 2026-09-11, per
//      explicit correction) to pick "Auto on" vs "Auto off" when
//      ac240vActive is false -- see formatCarState()'s own table below for
//      why this one case reports the raw switch as-is rather than a
//      predicted outcome.
//   2. autoStarted=false AND the raw switch reads ON -> label "Manual" --
//      the one condition that actually indicates something other than our
//      own control engaged it.
//   3. autoStarted=false AND the raw switch reads OFF -> still "Auto",
//      since nothing indicates a human touched anything -- but HW
//      specifically can still be powered=true here (the unit's own
//      built-in solar auto-diverter channel, independent of the manual
//      latch) which is real and must be shown, not hidden ("Auto off,
//      heating"). The car charger has no equivalent second power path, so
//      chargerOn=false with ac240vActive=true there is a genuine FAULT
//      (sensor mis-report or wiring/relay problem), not a normal state.
//
// The two full state tables below are the exact source of truth
// formatCarState()/formatHwState() implement -- keep them in sync with the
// .cpp if either ever changes.
#ifndef LOAD_STATUS_FORMAT_H
#define LOAD_STATUS_FORMAT_H

#include "CarChargerFetch.h" // CarChargerState
#include "HotWaterFetch.h"   // HotWaterState
#include <Arduino.h>          // String

// "Car: --" until reachable=false (no successful poll yet). kWh suffix is
// ALWAYS shown, including a genuine 0.0 (changed 2026-09-15, per explicit
// instruction, from the original design -- matching
// dailyPrediction/plot_today_solar.py's own car_legend, which omitted a
// 0-kWh day entirely -- specifically so the figure itself confirms the
// display is actually reading kwhToday, not indistinguishable from a
// blank/not-yet-implemented field). V2H (checked first, unconditionally)
// is a hardware safety state, orthogonal to ownership -- deliberately uses
// the wire-field name V2H, not Python's "V2L" display label (explicit
// instruction).
//
// Full state table (v2hActive/inV2hRecovery checked first, unconditionally
// -- a hardware safety block, orthogonal to ownership):
//
//   v2h | inV2hRecov | autoStarted | chargerOn (raw) | ac240vActive | inNoAcCooldown | Text
//   ----|------------|-------------|-----------------|--------------|----------------|----------------------------------------
//   T   | -          | -           | -               | T            | -              | "Car: V2H" + kwhToday suffix ("kWh Charged", always shown)
//   T   | -          | -           | -               | F            | -              | "Car: V2H tripped off <v2hLoadKw>kW load"
//   F   | T          | -           | -               | (F)          | -              | "Car: V2H tripped off <v2hLoadKw>kW load"
//   F   | F          | T           | (ignored)       | T            | -              | "Car: Auto charging"
//   F   | F          | T           | T               | F            | -              | "Car: Auto on -- Fully charged or unplugged"
//   F   | F          | T           | F               | F            | -              | "Car: Auto off -- Fully charged or unplugged"
//   F   | F          | F           | T               | T            | -              | "Car: Manual charging"
//   F   | F          | F           | T               | F            | -              | "Car: Manual on -- Fully charged or unplugged"
//   F   | F          | F           | F               | T            | -              | "Car: <r>FAULT</r> 240VAC active while OFF"
//   F   | F          | F           | F               | F            | T              | "Car: Auto off -- Fully charged or unplugged"
//   F   | F          | F           | F               | F            | F              | "Car: off"
//
// The FAULT row is a genuine, previously-unhandled combination -- no path
// exists for 240VAC to be present while the ESP32-controlled relay
// (chargerOn) is open, unlike HW's legitimate "Auto off, heating" (below),
// so it is flagged as an error rather than folded into the normal ladder.
//
// "Auto on"/"Auto off"/"Manual on" with no 240VAC (added 2026-09-11, per
// explicit instruction) are genuinely ambiguous states the ESP32 cannot
// resolve on its own -- no current is actually flowing, but there's no
// separate "car still plugged in" sensor (see CarChargerFetch.h's
// CarChargerState -- only chargerOn/ac240vActive/v2hActive exist), so any
// of these can equally mean the car finished charging and its OWN onboard
// charger stopped drawing, or it was unplugged -- hence naming both
// possibilities rather than guessing one.
//
// autoStarted's "on"/"off" here is simply the raw chargerOn reading,
// exactly like the non-autoStarted rows already do -- not a prediction of
// what the next control cycle is about to do to it.
//
// inNoAcCooldown (LoadControlTask.cpp's carNoAcCooldownTimer, published via
// CarChargerInstances.h's carInNoAcCooldown -- added 2026-09-11, per
// explicit correction) covers what happens AFTER carCheckStop()'s phase 3a
// actually stops the car for lack of 240VAC: that same code clears
// autoStarted immediately (auto-control no longer "owns" a load it isn't
// actively managing), so by the very next status read -- including the
// SAME cycle's own status log line, since the publish happens before it --
// autoStarted is already false and chargerOn/ac240vActive are both false
// too, which would otherwise be indistinguishable from "genuinely never
// charged today at all" (plain "Car: off", no explanation). The 15-min
// cooldown timer that same code starts is still the most recent (and only)
// explanation available, so "Car: Auto off -- Fully charged or unplugged"
// is shown for the WHOLE cooldown window, not just the instant of the stop
// -- falling back to plain "Car: off" only once the cooldown itself
// expires with the charger still untouched.
//
// kwhToday suffix (" X.XkWh", ALWAYS shown, including 0.0 -- see the note
// at the top of this file) appends to every row above EXCEPT the tripped
// "V2H" row, BEFORE the "Fully charged or unplugged" text on the rows that
// carry it.
//
// The plain "V2H" row (ac240vActive=true) shows kwhToday too, but spelled
// out as "X.XkWh Charged" rather than the bare "X.XkWh" every other row
// uses (added 2026-09-15, per explicit instruction) -- unlike those rows,
// there's no "charging"/"heating" word already sitting next to the figure
// here to give it context, and the value itself is frozen at whatever it
// reached before V2H engaged (CarChargerTask.cpp's own "charging" condition
// excludes v2hActive), so it needs to read unambiguously as "today's total
// so far", not "currently charging at this rate". The tripped row shows
// v2hLoadKw instead (see v2hRelevantLoadKw() just below for exactly what
// this is), per explicit instruction: "V2H tripped off" (ac240vActive=
// false, per explicit correction -- the V2H safety trip has taken the
// charger's AC output offline, not just "not connected") shows v2hLoadKw
// specifically because that's the figure the recovery decision
// (LoadControlTask.cpp's V2H_RETRY_MAX_LOAD_KW check) actually depends on,
// and isn't otherwise obvious while tripped -- kwhToday is not also shown
// on this row.
//
// inV2hRecovery (LoadControlTask.cpp's carV2hRecoveryPending, published via
// CarChargerInstances.h's carInV2hRecovery -- added 2026-09-11, per
// explicit instruction) covers the same "keep showing what's really going
// on, not a misleadingly plain state" need as inNoAcCooldown above, for a
// different situation: carCheckStart()'s V2H trip-recovery sequence
// toggles V2H OFF the moment it detects a trip (v2hActive && !ac240vActive)
// so a LATER cycle can try turning it back on once the load drops enough
// -- meaning v2hActive itself reads false throughout that whole recovery
// attempt. Without inV2hRecovery, this would fall through to a plain
// "off"-style row that hides the recovery in progress; with it,
// formatCarState() keeps showing "V2H tripped off" the entire time.
String formatCarState(bool reachable, const CarChargerState &s, float kwhToday, bool autoStarted,
                       bool inNoAcCooldown, bool inV2hRecovery, float v2hLoadKw);

// The whole-home metered load (GatewayInstances.h's latestConsumeKw) MINUS
// HW's own assumed draw (HW_ELEMENT_LOAD_KW) when hw.powered is true --
// added 2026-09-11, per explicit correction: HW is on a completely
// separate circuit from the car charger/V2H, so its own consumption
// shouldn't count against V2H's own circuit capacity. This is the figure
// both formatCarState()'s two V2H rows display AND LoadControlTask.cpp's
// own V2H_RETRY_MAX_LOAD_KW recovery-decision threshold compares against
// -- the same function is called from both places (LoadControlTask.cpp's
// checkLoadControlPoll(), Dwg_StatusApp.cpp's own independent read) so the
// displayed figure and the decision it drives always agree. There's no
// live power reading for HW's element either (same situation as the car
// charger's own CAR_CHARGE_W estimate -- see CarChargerTask.cpp),
// so HW_ELEMENT_LOAD_KW is a fixed assumed value, same convention.
float v2hRelevantLoadKw(float houseLoadKw, const HotWaterState &hw);

// Same not-yet-available convention as formatCarState() above. hwSource
// OFFPEAK/OFF/NO_WIFI (from the controller's own c8 report) still
// short-circuit to their own fixed text regardless of autoStarted/
// manualOn/powered -- those are the controller's own separate
// mains-supply/connectivity states, not something our ownership tracking
// has any bearing on.
//
// Full state table for the SOLAR/UNKNOWN hwSource branch (OFFPEAK/OFF/
// NO_WIFI short-circuit before this, as above):
//
//   autoStarted | manualOn (raw latch) | powered (heating) | Text
//   ------------|------------------------|---------------------|----------------------------
//   T           | (ignored)              | T                    | "HW: Auto heating"
//   T           | (ignored)              | F                    | "HW: Auto on, not heating"
//   F           | T                      | T                    | "HW: Manual heating"
//   F           | T                      | F                    | "HW: Manual on, not heating"
//   F           | F                      | T                    | "HW: Auto off, heating"
//   F           | F                      | F                    | "HW: off"
//
// "HW: Auto off, heating" is real and reachable -- the unit's own built-in
// solar auto-diverter channel can power the element with the manual latch
// never engaged at all. kwhToday suffix (" X.XkWh", ALWAYS shown, including
// 0.0 -- see the note at the top of this file) and "fully heated" suffix
// (omitted when false, matching Python's own fully_heated_str) both append
// to EVERY row above, including the OFFPEAK/OFF/NO_WIFI short-circuits.
String formatHwState(bool reachable, const HotWaterState &s, bool autoStarted);

#endif
// ================= end of LoadStatusFormat.h  file
