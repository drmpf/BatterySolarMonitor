// CarChargerFetch.h  file  =================
// Talks to the separate car-charger ESP32 controller (plain HTTP, fixed
// LAN IP, no DNS -- matches CHARGER_IP in dailyPrediction/plot_today_solar.py)
// to read its state and toggle it. See dailyPrediction/docs/
// car_charger_control.md for the full protocol and the start/stop decision
// logic this feeds.
//
// Stage 1 of the load-control feature: this file only provides the raw
// I/O (fetchCarChargerState()/toggleCarCharger()). CarChargerTask.h drives
// periodic polling for now, just to confirm connectivity; the actual
// start/stop decision logic (a later stage) is what will call
// toggleCarCharger() for real.
#ifndef CAR_CHARGER_FETCH_H
#define CAR_CHARGER_FETCH_H

// One reading of the car charger's reported state (GET /api/state).
struct CarChargerState {
  bool chargerOn = false;    // charger_state == "on"
  bool ac240vActive = false; // ac240v_status == "active"
  bool v2hActive = false;    // v2h_state == "on" -- V2L/V2H block (charging disabled)

  // The charger's OWN V2H on/off schedule (added 2026-09-15) -- see
  // EV_Charger_V2H_switch/switchSettings.cpp's handleAuto(), which
  // switches V2H fully on/off at these times regardless of anything this
  // sketch does. LoadControlTask.cpp's V2H recovery logic reads these to
  // avoid fighting a deliberate scheduled-off (see its own comment).
  bool v2hAutoActive = false;   // v2h_auto_active == "active"
  int v2hOnTimeMinutes = 0;     // v2h_time_on ("HH:mm"), parsed to minutes since midnight
  int v2hOffTimeMinutes = 0;    // v2h_time_off, parsed the same way
};

// Fetches GET /api/state and fills 'state' on success. Returns true on
// success; false on any failure ('state' left untouched). Fixed local IP,
// no DNS lookup involved -- unlike SolarForecastFetch.h, safe to call from
// any thread, including a background task, with no separate resolveXHost()
// main-thread step needed.
bool fetchCarChargerState(CarChargerState &state);

// POSTs to /chargerPower, which TOGGLES whatever the charger's current
// on/off state currently is (not an explicit "set to X") -- same as
// dailyPrediction/plot_today_solar.py's own _car_toggle(). Callers must
// check the current state (fetchCarChargerState()) before calling this to
// know which way it will flip. Returns true if the POST succeeded.
bool toggleCarCharger();

// POSTs to /v2hPower (added 2026-09-11, confirmed against the charger
// controller's own source: server.on("/v2hPower", HTTP_POST,
// handle_v2hPower)), which TOGGLES the charger's current V2H mode -- same
// "toggle, not set" semantics and empty-body POST as toggleCarCharger()
// above. Callers must check the current v2hActive (fetchCarChargerState())
// before calling this to know which way it will flip. Returns true if the
// POST succeeded. See LoadControlTask.cpp's carCheckStart() (V2H section)
// for the trip-recovery logic this drives.
bool toggleV2h();

#endif
// ================= end of CarChargerFetch.h  file
