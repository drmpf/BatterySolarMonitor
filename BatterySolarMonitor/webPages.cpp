/**
   webPages.cpp
   by Matthew Ford,  2021/12/06
   (c)2021 Forward Computing and Control Pty. Ltd.
   NSW, Australia  www.forward.com.au
   This code may be freely used for both private and commerical use.
   Provide this copyright is maintained.

   Serves /settz.html (from LittleFS, with %VAR% substitution) and handles
   its /setTZstr, /resetTZ and /setTime buttons -- registered on the same
   WebServer instance ESP_PicoW_pfodWebServer.cpp already runs, via that
   file's getPfodHttpServer() accessor.
*/

#include "webPages.h"
#include <ESP_PicoW_pfodWebServer.h>
#include <ESP_PicoW_LittleFSsupport.h>
#include <LittleFS.h>
#include <pfodDebugPtr.h>

#include "tzPosix.h"
#include "ntpSupport.h"
#include "SafeString.h"
#include <time.h>

// normally DEBUG is commented out
#define DEBUG  // this file's debug output is on by default -- explicitly requested, to check the TZ_STR substitution/setting logic
static Print* debugPtr = NULL;  // local to this file

static bool webPagesAdded = false;

static void handle_settzHtml();
static void handle_setTZstr();  // setter button
static void handle_resetTZ();
static void handle_setTime();

static bool convertHH_MMtoMins(SafeString& hh_mm, int& mins_out);
static void redirect(const char* url);
static String processor(const String& var);

static String correctedTZstr;     // empty if no problems
static String correctedTZstrDsc;
static String userInputTZstr;
static String setMsg;

void addSetTzWebPages() {
#ifdef DEBUG
  debugPtr = getDebugPtr();
#endif
  if (webPagesAdded) {
    return;
  }
  getPfodHttpServer().on("/settz.html", HTTP_GET, handle_settzHtml);
  getPfodHttpServer().on("/setTZstr", HTTP_POST, handle_setTZstr);
  getPfodHttpServer().on("/resetTZ", HTTP_GET, handle_resetTZ);
  getPfodHttpServer().on("/setTime", HTTP_POST, handle_setTime);
  webPagesAdded = true;
}

// 303 See Other, NOT 307 Temporary Redirect -- confirmed via HAR capture
// to be the actual root cause of the "%TZ_STR% never substituted" report:
// this is only ever called from a POST handler (handle_setTZstr()'s
// needs-correction branch) redirecting to a GET-only page. 307 preserves
// the original request's method AND body by spec -- the browser correctly
// re-POSTs to /settz.html, which has no POST handler registered (only
// HTTP_GET), so it falls through to ESP_PicoW_pfodWebServer.cpp's generic
// onNotFound()/handleFileRead() static-file server instead of
// handle_settzHtml() -- serving the RAW file (with its normal
// Cache-Control: max-age=<cacheSec>) and none of the %VAR% substitution.
// 303 is the standard POST-redirect-GET status specifically for this case:
// it tells the browser to follow up with a GET regardless of the original
// method, landing correctly on handle_settzHtml().
static void redirect(const char* url) {
  if (debugPtr) {
    debugPtr->print("Redirect to: ");
    debugPtr->println(url);
  }
  getPfodHttpServer().sendHeader("Location", url);
  getPfodHttpServer().send(303);
}

// settz.html is opened from the main menu as its own browser tab (via
// pfodWeb.html's window.open('', '_blank')), not navigated to from an
// index.html this sketch has -- so once a setting is actually applied
// there is nothing to redirect back to. Since that tab was opened by
// script, window.close() from inside it is allowed to succeed; the
// visible message is only a fallback for browsers that block it anyway.
static void closeTabPage(const char* message) {
  if (debugPtr) {
    debugPtr->print("closeTabPage: ");
    debugPtr->println(message);
  }
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>";
  html += "<body style=\"font-family:sans-serif;text-align:center;margin-top:40px;\">";
  html += message;
  html += "<br>You can close this tab now.";
  html += "<script>window.close();</script></body></html>";
  getPfodHttpServer().send(200, "text/html", html);
}

// return true if valid input, result returned in mins_out var
// convert to mins
static bool convertHH_MMtoMins(SafeString& hh_mm, int& mins_out) {
  cSF(token, 20);
  int idx = 0;
  idx = hh_mm.stoken(token, idx, ':');
  int hr = 0;
  int mins = 0;
  if (token.toInt(hr) && (hr >= 0) && (hr <= 23)) {
    idx = hh_mm.stoken(token, idx, ':');
    if (token.toInt(mins) && (mins >= 0) && (mins <= 59)) {
      mins_out = hr * 60 + mins;
      if (debugPtr) {
        debugPtr->print("dayMins:");
        debugPtr->print(mins_out);
        debugPtr->println();
      }
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
}

// GET handler for /settz.html -- reads the file from LittleFS and
// substitutes the %VAR% placeholders it uses.
static void handle_settzHtml() {
  if (debugPtr) {
    debugPtr->println(" handle_settzHtml");
  }
  File file = LittleFS.open("/settz.html", "r");
  if (!file) {
    getPfodHttpServer().send(404, "text/plain", "settz.html not found");
    return;
  }
  // NOTE: not file.readString() -- Stream::readString() calls timedRead(),
  // which busy-waits up to its 1s timeout past EOF before returning. This
  // sketch's loop() (NTP/solar/gateway polling) sits behind this server's
  // one blocking handleClient() call, so that stall would delay everything
  // else each time this page is loaded. available()/read() hits EOF (and
  // stops) immediately instead, matching this project's own file-reading
  // convention elsewhere (e.g. SiteSettings.cpp's readStringUntil() loop).
  String html;
  html.reserve(file.size() + 1);
  while (file.available()) {
    html += (char)file.read();
  }
  file.close();

  html.replace("%TIME%", processor("TIME"));
  html.replace("%TZ_STR%", processor("TZ_STR"));
  html.replace("%TZ_DESC_STR%", processor("TZ_DESC_STR"));
  html.replace("%TZ_CORRECTED%", processor("TZ_CORRECTED"));
  html.replace("%TZ_STR_SET_CORRECTED%", processor("TZ_STR_SET_CORRECTED"));

  if (debugPtr) {
    debugPtr->print("handle_settzHtml sending substituted page, length=");
    debugPtr->println(html.length());
  }
  // This page is generated fresh (from live TZ state) on every request --
  // never let a browser cache it. Confirmed via HAR capture that the
  // original "%TZ_STR% never substituted" report was actually a DIFFERENT
  // bug (redirect()'s 307->303 fix, see that function's own comment): the
  // POST-redirected request landed on ESP_PicoW_pfodWebServer.cpp's
  // generic static-file handler instead of this one, which is why THAT
  // response carried a Cache-Control: max-age=<cacheSec> (10min) header --
  // this handler never actually sent that. Kept anyway as correct general
  // practice for a dynamically-generated page, and as a second line of
  // defence against exactly that class of mix-up recurring. Same headers
  // as this project's other dynamic JSON/status responses (e.g. the old
  // EV_Charger_V2H_switch webPages.cpp handle_getState()).
  getPfodHttpServer().sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  getPfodHttpServer().sendHeader("Pragma", "no-cache");
  getPfodHttpServer().sendHeader("Expires", "0");
  getPfodHttpServer().send(200, "text/html", html);
}

// does the %..% replacement for settz.html
static String processor(const String& var) {
  String rtnString = "";
  if (var == "TZ_STR") {
    if (setMsg.length()) {
      rtnString = correctedTZstr;
      correctedTZstr = "";  // finished  with this
    } else {
      rtnString = getTZstr();
    }
  } else if (var == "TZ_DESC_STR") {
    if (correctedTZstrDsc.length()) {
      rtnString = correctedTZstrDsc;
      correctedTZstrDsc = "";  // finished  with this
    } else {
      rtnString = getTZstr();
    }
    String TZstr = rtnString;
    struct posix_tz_data_struct tzdata;
    posixTZDataFromStr(TZstr, tzdata);
    buildPOSIXdescription(tzdata, rtnString);
    rtnString.replace("\n", "<br>");
  } else if (var == "TZ_CORRECTED") {
    if (userInputTZstr.length()) {
      rtnString = "Time Zone string has been cleaned up from<br>";
      rtnString += userInputTZstr;
      rtnString += "<br>to";
      userInputTZstr = "";  // finished  with this
    }
  } else if (var == "TZ_STR_SET_CORRECTED") {
    rtnString = setMsg;
    setMsg = "";
  } else if (var == "TIME") {
    rtnString = getCurrentTime_hhmm();
  }
  if (debugPtr) {
    debugPtr->print("processing %");
    debugPtr->print(var);
    debugPtr->print("% = '");
    debugPtr->print(rtnString);
    debugPtr->println("'");
  }
  return rtnString;
}

static void handle_setTZstr() {
  if (debugPtr) {
    debugPtr->println(" handle_setTZstr");
  }
  bool haveArg = getPfodHttpServer().hasArg("TZ_INPUT_STR");
  if (debugPtr) {
    debugPtr->print("  TZ_INPUT_STR arg present: ");
    debugPtr->println(haveArg ? "yes" : "NO -- form field missing, will apply an EMPTY tz string");
  }
  String newTZ;
  if (haveArg) {
    userInputTZstr = "";
    correctedTZstr = "";
    correctedTZstrDsc = "";
    setMsg = "";
    String rawArg = getPfodHttpServer().arg("TZ_INPUT_STR");
    String inputStr = rawArg;
    inputStr.trim();
    String cleanStr = rawArg;
    cleanUpPosixTZStr(cleanStr);
    bool needsCorrection = (inputStr != cleanStr);
    if (debugPtr) {
      debugPtr->print("  raw submitted:   '"); debugPtr->print(rawArg); debugPtr->println("'");
      debugPtr->print("  trimmed input:   '"); debugPtr->print(inputStr); debugPtr->println("'");
      debugPtr->print("  cleaned/rebuilt: '"); debugPtr->print(cleanStr); debugPtr->println("'");
      debugPtr->print("  needsCorrection (trimmed != cleaned): ");
      debugPtr->println(needsCorrection ? "YES -- will re-show settz.html for confirm, not apply yet" : "no -- applying now");
    }
    if (needsCorrection) {
      userInputTZstr = inputStr;
      correctedTZstr = cleanStr;
      correctedTZstrDsc = cleanStr;
      setMsg = "Select the Set TZ String button again to set<br>this cleaned up time zone string";
    } else {
      newTZ = cleanStr;
      correctedTZstrDsc = cleanStr;
    }
  }
  if (setMsg.length()) {
    redirect("/settz.html");
  } else {
    // set new tz
    if (debugPtr) {
      debugPtr->print("  applying newTZ: '"); debugPtr->print(newTZ); debugPtr->println("'");
    }
    setTZfromPOSIXstr(newTZ.c_str());  // cleans up and sets save flag
    saveTZconfigIfNeeded();            // write to file
    if (debugPtr) {
      debugPtr->print("  getTZstr() now reads: '"); debugPtr->print(getTZstr()); debugPtr->println("'");
    }
    closeTabPage("Time zone set.");
  }
}

static void handle_resetTZ() {
  if (debugPtr) {
    debugPtr->println(" handle_resetTZ");
  }
  resetDefaultTZstr();  // also saves
  closeTabPage("Time zone reset to default.");
}

static void handle_setTime() {
  if (debugPtr) {
    debugPtr->println(" handle_setTime");
  }
  cSF(sfTimeSetting, 20);
  if (getPfodHttpServer().hasArg("TIME")) {
    String timeValue = getPfodHttpServer().arg("TIME");
    if (debugPtr) {
      debugPtr->print("Time Value: ");
      debugPtr->println(timeValue);
    }
    sfTimeSetting = timeValue.c_str();
    int dayMins = 0;
    if (convertHH_MMtoMins(sfTimeSetting, dayMins)) {
      // get utc time
      time_t now = time(nullptr);
      struct tm* tmPtr = gmtime(&now);
      int utcDayMins = tmPtr->tm_hour * 60 + tmPtr->tm_min;
      int tzDiffMins = (utcDayMins - dayMins);
      if (debugPtr) {
        debugPtr->print("tzDiff :");
        debugPtr->print(tzDiffMins);
        debugPtr->println();
      }
      // round to 5min
      int sign = 1;
      if (tzDiffMins < 0) {
        sign = -1;
      }
      int tzDiffMinRoundedUnsigned = ((tzDiffMins * sign * 60 + 150) / 300 * 300) / 60;
      if (debugPtr) {
        debugPtr->print("tzDiffRounded :");
        debugPtr->print(tzDiffMinRoundedUnsigned);
        debugPtr->println();
      }
      int hr_offset = tzDiffMinRoundedUnsigned / 60;
      int min_offset = tzDiffMinRoundedUnsigned - hr_offset * 60;
      tzDiffMins = tzDiffMinRoundedUnsigned * sign;
      hr_offset *= sign;
      if (debugPtr) {
        debugPtr->print("offset ");
        debugPtr->print(hr_offset);
        debugPtr->print(':');
        debugPtr->print(min_offset);
        debugPtr->println();
      }
      // offsets in range -12 < offset <= +12  i.e. -11:45 is the smallest offset and +12:00 is the largest
      //         mins in range -720 < offsetMin <= +720
      // e.g. LT (localTime) = 14:00,  UTC=04:00  tzoffset = +10:00
      //      LT = 08:00  UTC = 22:00  tzoffset =  -14 => <=-12 so add 24,  -14+24 = +10
      //      LT = 14:00  UTC = 22:00  tzoffset = -8:00
      //      LT = 20:00  UTC = 4:00   tzoffset = 16:00 => >12 so subtract 24,  16-24 = -8:00
      if (tzDiffMins <= -720) {
        tzDiffMins += (24 * 60);
      } else if (tzDiffMins > 720) {
        tzDiffMins -= (24 * 60);
      }
      sign = 1;
      if (tzDiffMins < 0) {
        sign = -1;
      }
      tzDiffMinRoundedUnsigned = tzDiffMins * sign;
      hr_offset = tzDiffMinRoundedUnsigned / 60;
      min_offset = tzDiffMinRoundedUnsigned - hr_offset * 60;
      hr_offset *= sign;
      if (debugPtr) {
        debugPtr->print("offset (+/-12)");
        debugPtr->print(hr_offset);
        debugPtr->print(':');
        debugPtr->print(min_offset);
        debugPtr->println();
      }
      setTZoffsetInMins(tzDiffMins);  // update in min
      saveTZconfigIfNeeded();         // write to file
    }                                  // else error ignore
  }
  closeTabPage("Time set.");
}
