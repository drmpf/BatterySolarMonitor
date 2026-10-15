// LoadStatusFormat.cpp  file  =================
// See LoadStatusFormat.h for the full state tables both functions below
// implement -- keep this file and those tables in sync if either changes.
#include "LoadStatusFormat.h"

// "Auto on"/"Auto off"/"Manual on" with no 240VAC flowing are genuinely
// ambiguous -- see the header's full comment on these rows: no sensor
// distinguishes "car finished charging" from "car was unplugged", so both
// are named rather than guessing one.
static const char *AMBIGUOUS_SUFFIX = " -- Fully charged or unplugged";

// Assumed HW element draw (kW) when HotWaterState::powered is true -- see
// v2hRelevantLoadKw()'s own header comment for why this gets subtracted
// out. No live power reading exists for HW's element (same situation as
// the car charger -- see CarChargerFetch.cpp), so this is a fixed assumed
// value, same convention as CarChargerTask.cpp's own CAR_CHARGE_W. Added
// 2026-09-11, per explicit instruction.
static const float HW_ELEMENT_LOAD_KW = 3.5;

float v2hRelevantLoadKw(float houseLoadKw, const HotWaterState &hw) {
  return hw.powered ? (houseLoadKw - HW_ELEMENT_LOAD_KW) : houseLoadKw;
}

String formatCarState(bool reachable, const CarChargerState &s, float kwhToday, bool autoStarted,
                       bool inNoAcCooldown, bool inV2hRecovery, float v2hLoadKw) {
  if (!reachable) {
    return "Car: --";
  }
  // Always shown, including 0.0 -- per explicit instruction, so the figure
  // itself confirms the display is actually reading kwhToday rather than
  // it being indistinguishable from a blank/not-yet-implemented field.
  String kwhSuffix = " " + String(kwhToday, 1) + "kWh";

  // inV2hRecovery (added 2026-09-11, per explicit instruction) keeps this
  // whole branch active even once s.v2hActive itself reads false --
  // LoadControlTask.cpp's carCheckStart() (V2H section) toggles V2H OFF
  // the moment it detects a trip, so the recovery attempt itself would
  // otherwise be invisible (falling through to a plain "off"-style state
  // that hides what's actually happening) for as long as it's in
  // progress. ac240vActive stays false throughout the OFF phase (that's
  // the trip precondition, untouched by turning V2H off), so the ternary
  // below still correctly lands on "tripped off" the whole time.
  if (s.v2hActive || inV2hRecovery) {
    // "V2H" alone (ac240vActive=true) -- car actively connected, genuinely
    // exporting/charging-blocked; no kW shown here, per explicit
    // instruction. "V2H tripped off" (ac240vActive=false, per explicit
    // correction -- means the V2H safety trip has taken the charger's AC
    // output offline, not just "not connected") DOES show v2hLoadKw (the
    // whole-home metered load, HW's own draw already subtracted out by
    // the caller -- see v2hRelevantLoadKw() below), since that's the
    // figure the recovery decision (LoadControlTask.cpp's
    // V2H_RETRY_MAX_LOAD_KW check) actually depends on -- worth showing
    // here specifically because it's not obviously available/relevant
    // while genuinely exporting. kwhToday is never shown either way --
    // it's frozen/irrelevant while V2H blocks charging.
    if (!s.ac240vActive) {
      return "Car: V2H tripped off " + String(v2hLoadKw, 1) + "kW load";
    }
    // kwhToday itself is frozen while V2H is active (CarChargerTask.cpp's
    // own "charging" condition excludes v2hActive), but today's total up
    // to that point is still worth showing here -- "Charged" spelled out,
    // per explicit instruction, since unlike the Auto/Manual lines above
    // there's no "charging"/"heating" word already next to the figure to
    // give it context. Always shown, including 0.0 -- same reasoning as
    // kwhSuffix above.
    return "Car: V2H " + String(kwhToday, 1) + "kWh Charged";
  }
  if (autoStarted) {
    if (s.ac240vActive) {
      return "Car: Auto charging" + kwhSuffix;
    }
    // No 240VAC -- reflect the raw relay reading (chargerOn), per explicit
    // correction: "on"/"off" here means exactly what the charger itself
    // currently reports, not an assumption about what the next control
    // cycle is about to do to it.
    return (s.chargerOn ? "Car: Auto on" : "Car: Auto off") + kwhSuffix + AMBIGUOUS_SUFFIX;
  }
  if (s.chargerOn) {
    return s.ac240vActive ? ("Car: Manual charging" + kwhSuffix)
                           : ("Car: Manual on" + kwhSuffix + AMBIGUOUS_SUFFIX);
  }
  if (s.ac240vActive) {
    // FAULT, not a normal state: no path exists for 240VAC to be present
    // while the ESP32-controlled relay (chargerOn) is open -- unlike HW,
    // the car charger has no second power path. Sensor mis-report or a
    // wiring/relay problem.
    return "Car: <r>FAULT</r> 240VAC active while OFF" + kwhSuffix;
  }
  if (inNoAcCooldown) {
    // autoStarted is already false here -- carCheckStop()'s no-240VAC-stop
    // branch (LoadControlTask.cpp phase 3a) clears it the SAME cycle it
    // stops the charger, since auto-control no longer "owns" a load it
    // isn't actively managing -- but the 15-min cooldown timer it starts
    // at that same moment is still the most recent (and only) explanation
    // for why the charger is off, so keep showing "Auto off" for the
    // whole cooldown window, not just the one instant of the stop itself.
    // See CarChargerInstances.h's carInNoAcCooldown for the full reasoning.
    return "Car: Auto off" + kwhSuffix + AMBIGUOUS_SUFFIX;
  }
  return "Car: off" + kwhSuffix;
}

String formatHwState(bool reachable, const HotWaterState &s, bool autoStarted) {
  if (!reachable) {
    return "HW: --";
  }
  // Always shown, including 0.0 -- see formatCarState()'s identical change,
  // same reasoning, per explicit instruction.
  String kwhSuffix = " " + String(s.kwhToday, 1) + "kWh";
  String fullyHeatedSuffix = s.fullyHeated ? " fully heated" : "";

  String base;
  switch (s.hwSource) {
    case HW_SOURCE_OFFPEAK:
      base = "HW: Off Peak";
      break;
    case HW_SOURCE_OFF:
      base = "HW: off";
      break;
    case HW_SOURCE_NO_WIFI:
      base = "HW: no WiFi";
      break;
    default: // SOLAR or UNKNOWN -- manualOn is the raw latch, not evidence
             // of who is driving it; see the attribution model in the
             // header.
      if (autoStarted) {
        base = s.powered ? "HW: Auto heating" : "HW: Auto on, not heating";
      } else if (s.manualOn) {
        base = s.powered ? "HW: Manual heating" : "HW: Manual on, not heating";
      } else {
        // Not us, latch off -- but the unit's own separate solar
        // auto-diverter channel can still be powering the element without
        // ever touching the manual latch. A real, valid combination.
        base = s.powered ? "HW: Auto off, heating" : "HW: off";
      }
      break;
  }
  return base + kwhSuffix + fullyHeatedSuffix;
}
// ================= end of LoadStatusFormat.cpp  file
