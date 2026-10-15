// HotWaterFetch.cpp  file  =================
#include "HotWaterFetch.h"
#include "SiteSettings.h" // siteHotWaterIp()
#include <WiFiClient.h>
#include <pfodDebugPtr.h>
#include <string.h> // strlen()

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// Fixed LAN port -- a separate controller on the same local network, not an
// internet hostname, so (unlike SolarForecastFetch.cpp) there's no DNS
// lookup involved at all here. The IP itself is per-site config, from
// siteHotWaterIp() (SiteSettings.h), not hardcoded here.
static const uint16_t PORT = 4989;

static const uint32_t CONNECT_TIMEOUT_MS = 5000;
static const uint32_t RESPONSE_TIMEOUT_MS = 5000;

// Sends 'cmd' (already wrapped in "{...}") and reads bytes until the
// closing '}' of the pfod reply arrives or RESPONSE_TIMEOUT_MS elapses --
// mirrors dailyPrediction/plot_today_solar.py's own _recv() (its
// "while b'}' not in buf" loop), since pfod messages are self-delimiting
// by their closing '}' with nothing else on the wire to frame them.
// Returns the full message (including the '}') on success, or an empty
// String on timeout/disconnect before a complete message arrived -- so an
// empty return is always distinguishable from a real reply.
static String pfodExchange(WiFiClient &client, const String &cmd) {
  client.print(cmd);
  String resp;
  unsigned long startMs = millis();
  while (millis() - startMs < RESPONSE_TIMEOUT_MS) {
    while (client.available()) {
      char c = (char)client.read();
      resp += c;
      if (c == '}') {
        return resp;
      }
    }
    if (!client.connected()) {
      break; // remote closed before a full message arrived
    }
  }
  return String();
}

// {.} -> {,<bg w>~`2500~V3|+A~c1} -- version is the text between the 2nd
// and 3rd '~', up to the following '|' -- e.g. "V3". Matches
// plot_today_solar.py's own "parts = resp.split('~'); parts[2].split('|')[0]".
static String parseFirmwareVersion(const String &resp) {
  int firstTilde = resp.indexOf('~');
  if (firstTilde < 0) {
    return String();
  }
  int secondTilde = resp.indexOf('~', firstTilde + 1);
  if (secondTilde < 0) {
    return String();
  }
  int pipe = resp.indexOf('|', secondTilde + 1);
  if (pipe < 0) {
    return String();
  }
  return resp.substring(secondTilde + 1, pipe);
}

// Connects and does the version handshake, leaving 'client' open ready for
// a follow-up command -- every exchange in this protocol needs the
// version prefix, and Python opens exactly one connection per poll (or per
// toggle) and reuses it for both steps.
static bool connectAndGetVersion(WiFiClient &client, String &version) {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }
  IPAddress ip;
  if (!ip.fromString(siteHotWaterIp())) {
    if (debugPtr) {
      debugPtr->println(F("HotWaterFetch: bad siteHotWaterIp()"));
    }
    return false;
  }
  if (!client.connect(ip, PORT, CONNECT_TIMEOUT_MS)) {
    if (debugPtr) {
      debugPtr->println(F("HotWaterFetch: connect failed"));
    }
    return false;
  }
  String resp = pfodExchange(client, "{.}");
  version = parseFirmwareVersion(resp);
  if (version.length() == 0) {
    if (debugPtr) {
      debugPtr->print(F("HotWaterFetch: version handshake failed, resp="));
      debugPtr->println(resp);
    }
    client.stop();
    return false;
  }
  return true;
}

// {<ver>:c4`0} -> {+|t`16~0~<+4><b>Hot Water 3.8kWhr~~} -- pulls out the
// number between "Hot Water" and "kWhr". Returns 0.0 (not a failure -- see
// fetchHotWaterState()'s own comment) if the pattern isn't found.
static float parseKwhToday(const String &resp) {
  int label = resp.indexOf("Hot Water");
  if (label < 0) {
    return 0.0;
  }
  int start = label + strlen("Hot Water");
  while (start < (int)resp.length() && resp[start] == ' ') {
    start++;
  }
  int end = start;
  while (end < (int)resp.length() && (isDigit(resp[end]) || resp[end] == '.')) {
    end++;
  }
  if (end == start) {
    return 0.0;
  }
  return resp.substring(start, end).toFloat();
}

// {<ver>:c8`0} -> {+|t`19~12~<+4><b>Solar Hot Water/n2:01pm~~} -- matches
// dailyPrediction/docs/hot_water_control.md's response-text table.
static HotWaterSource parseHwSource(const String &resp) {
  if (resp.indexOf("Solar Hot Water") >= 0) {
    return HW_SOURCE_SOLAR;
  }
  if (resp.indexOf("Off Peak Hot Water") >= 0) {
    return HW_SOURCE_OFFPEAK;
  }
  if (resp.indexOf("Hot Water OFF") >= 0) {
    return HW_SOURCE_OFF;
  }
  if (resp.indexOf("No WiFi Connection") >= 0) {
    return HW_SOURCE_NO_WIFI;
  }
  return HW_SOURCE_UNKNOWN;
}

bool fetchHotWaterState(HotWaterState &state) {
  WiFiClient client;
  String version;
  if (!connectAndGetVersion(client, version)) {
    return false;
  }

  // {<ver>:c2`0} -> channel states, e.g.
  // {+|C`13~9~3~~|h`1|h`2|h`3|uh`4|...|uh`7|uh`8|...}
  // Required -- this is the field the control logic depends on, so a
  // failure here fails the whole fetch (matching Python: c2 has no
  // separate try/except of its own, unlike c4/c8 below).
  String resp = pfodExchange(client, "{" + version + ":c2`0}");
  if (resp.length() == 0) {
    if (debugPtr) {
      debugPtr->println(F("HotWaterFetch: c2 channel-state request failed"));
    }
    client.stop();
    return false;
  }
  state.manualOn = resp.indexOf("|C`13~9") >= 0;
  state.powered = resp.indexOf("|uh`7") >= 0;

  // c4 (today's kWh), c5 (fully-heated flag) and c8 (heating source) are
  // all best-effort -- a failure on any one of them just leaves that field
  // at its default (0.0 / false / UNKNOWN) rather than failing the fetch,
  // matching plot_today_solar.py's own per-step try/except around these
  // three steps specifically.
  String kwhResp = pfodExchange(client, "{" + version + ":c4`0}");
  if (kwhResp.length() > 0) {
    state.kwhToday = parseKwhToday(kwhResp);
  } else if (debugPtr) {
    debugPtr->println(F("HotWaterFetch: c4 kWh request failed, kwhToday left at default"));
  }

  // {<ver>:c5`0} -> {+|t`17~2~<+3><b>Hot Water was Fully Heated~~}
  String fullyHeatedResp = pfodExchange(client, "{" + version + ":c5`0}");
  if (fullyHeatedResp.length() > 0) {
    state.fullyHeated = fullyHeatedResp.indexOf("Fully Heated") >= 0;
  } else if (debugPtr) {
    debugPtr->println(F("HotWaterFetch: c5 fully-heated request failed, fullyHeated left at default"));
  }

  String sourceResp = pfodExchange(client, "{" + version + ":c8`0}");
  if (sourceResp.length() > 0) {
    state.hwSource = parseHwSource(sourceResp);
  } else if (debugPtr) {
    debugPtr->println(F("HotWaterFetch: c8 source request failed, hwSource left UNKNOWN"));
  }

  client.stop();

  if (debugPtr) {
    debugPtr->print(F("HotWaterFetch: manualOn="));
    debugPtr->print(state.manualOn ? F("true") : F("false"));
    debugPtr->print(F(" powered="));
    debugPtr->print(state.powered ? F("true") : F("false"));
    debugPtr->print(F(" kwhToday="));
    debugPtr->print(state.kwhToday, 2);
    debugPtr->print(F(" fullyHeated="));
    debugPtr->print(state.fullyHeated ? F("true") : F("false"));
    debugPtr->print(F(" hwSource="));
    debugPtr->println((int)state.hwSource);
  }
  return true;
}

bool toggleHotWater() {
  WiFiClient client;
  String version;
  if (!connectAndGetVersion(client, version)) {
    return false;
  }

  // {<ver>:A~c3`1`0`0} toggles whichever way channel 13's manual latch
  // currently is -- callers must check fetchHotWaterState()'s manualOn
  // first to know which way this will flip (same toggle-then-check-
  // current-state safety guard dailyPrediction/docs/hot_water_control.md
  // describes for hw_check_stop()/hw_check_start()).
  String resp = pfodExchange(client, "{" + version + ":A~c3`1`0`0}");
  client.stop();
  if (resp.length() == 0) {
    if (debugPtr) {
      debugPtr->println(F("HotWaterFetch: toggle failed (no ack)"));
    }
    return false;
  }
  if (debugPtr) {
    debugPtr->println(F("HotWaterFetch: manual latch toggled"));
  }
  return true;
}
// ================= end of HotWaterFetch.cpp  file
