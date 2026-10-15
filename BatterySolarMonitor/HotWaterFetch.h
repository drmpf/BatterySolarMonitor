// HotWaterFetch.h  file  =================
// Talks to the separate hot-water controller (raw TCP, pfodProtocol, fixed
// LAN IP:port, no DNS -- matches HW_IP/HW_PORT in
// dailyPrediction/plot_today_solar.py) to read its manual-on/powered state
// and toggle the manual latch. See dailyPrediction/docs/hot_water_control.md
// for the full protocol and the start/stop decision logic this feeds.
//
// Stage 2 of the load-control feature (car charger was stage 1,
// CarChargerFetch.h/CarChargerTask.h): this file only provides the raw
// I/O (fetchHotWaterState()/toggleHotWater()). HotWaterTask.h drives
// periodic polling; the actual start/stop decision logic (a later stage)
// is what will call toggleHotWater() for real.
//
#ifndef HOT_WATER_FETCH_H
#define HOT_WATER_FETCH_H

// hw_source, from the controller's {<ver>:c8`0} response -- see
// dailyPrediction/docs/hot_water_control.md's response-text table.
// UNKNOWN covers both "not yet fetched" and "response matched none of the
// known strings" (same as Python's hw_source=None).
enum HotWaterSource {
  HW_SOURCE_UNKNOWN,
  HW_SOURCE_SOLAR,
  HW_SOURCE_OFFPEAK,
  HW_SOURCE_OFF,
  HW_SOURCE_NO_WIFI
};

// One reading of the hot water controller's state.
struct HotWaterState {
  bool manualOn = false;   // |C`13~9 present -- channel 13 manually latched on
  bool powered = false;    // |uh`7 present -- element actually drawing power
  float kwhToday = 0.0;    // from c4 -- the controller's OWN running daily total,
                            // not accumulated by this code (unlike the car
                            // charger, which has no such reading -- see
                            // CarChargerTask.cpp's own estimate)
  HotWaterSource hwSource = HW_SOURCE_UNKNOWN; // from c8
  bool fullyHeated = false; // from c5 -- sticky "reached full temperature today" flag
};

// Opens a TCP connection, does the version handshake, and requests channel
// state (c2 -- required for success), today's kWh (c4), the fully-heated
// flag (c5), and the heating source (c8) -- all three of c4/c5/c8 are
// best-effort, matching plot_today_solar.py's own per-step try/except: a
// failure on any one of them leaves that one field at its default rather
// than failing the whole fetch, since manualOn/powered (from c2) is the
// field the control logic actually depends on. Fills 'state' and returns
// true if at least the c2 step succeeded; false on any earlier failure
// ('state' left untouched). Fixed local IP:port, no DNS lookup involved --
// unlike SolarForecastFetch.h, safe to call from any thread, including a
// background task, with no separate
// resolveXHost() main-thread step needed.
bool fetchHotWaterState(HotWaterState &state);

// Sends the manual-latch toggle command, which TOGGLES whatever channel
// 13's current manual state currently is (not an explicit "set to X") --
// same as dailyPrediction/plot_today_solar.py's own _hw_send_toggle().
// Callers must check the current state (fetchHotWaterState().manualOn)
// before calling this to know which way it will flip. Returns true if the
// toggle command was sent and acknowledged.
bool toggleHotWater();

#endif
// ================= end of HotWaterFetch.h  file
