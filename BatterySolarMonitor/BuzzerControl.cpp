// BuzzerControl.cpp
#include "BuzzerControl.h"

#include "DebouncedSwitch.h"
#include "LocalTimeMinutes.h"  // localTimeMinutesSinceMidnight()
#include "ntpSupport.h"        // missedSNTPupdate()
#include "GatewayInstances.h"  // latestBatterySocPercent
#include "GatewayPollTask.h"   // lockGatewayReading()/unlockGatewayReading()
#include <pfodDebugPtr.h>

#define DEBUG // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

static void logLine(const String &line) {
  if (debugPtr) {
    debugPtr->println(line);
  }
}

static const int mainsDetectionPin = D11;
static DebouncedSwitch mainsDetectorSw(mainsDetectionPin);  // monitor a switch on input

static bool mainsOn = false;
static const int buzzerPin = D7;
static const int switchPin = D2;
static const int switchPinGND = D6;     // provides a separate GND pin for the pushbutton
static DebouncedSwitch sw(switchPin);  // monitor a switch on input

// Low-SOC alarm window, minutes since midnight -- 2pm (14:00) to 8pm (20:00)
static const int SOC_ALARM_START_MIN = 14 * 60;
static const int SOC_ALARM_END_MIN = 20 * 60;
static const float SOC_ALARM_THRESHOLD_PERCENT = 15.0;

// How many consecutive battery-SOC fetch failures (GatewayPollTask.h's
// consecutiveBatteryFetchFailures(), each ~FETCH_RETRY_DELAY_MS -- 15s --
// apart once failing, GatewayPollTask.cpp) count as "can't confirm the
// battery's SOC" for long enough to be alarm-worthy in its own right,
// alongside (not instead of) a confirmed low reading -- 20 x 15s = 5
// minutes. Per explicit instruction/bug report (streaming_2026-09-25T09-31-12-797Z.txt):
// a gateway that's merely struggling to connect must fail safe to "not
// low" (isSocLow() already does, via the -1 sentinel) rather than let a
// single stray reading -- confirmed or bogus -- sound the alarm; but a
// SUSTAINED inability to get any reading at all, during the risk window,
// is itself worth flagging, since not knowing the SOC is not the same as
// knowing it's fine.
static const int SOC_ALARM_FAIL_RETRY_COUNT = 20;

// Latched state for the mains-loss alarm -- set on the loss-of-mains edge,
// cleared by either mains being restored or the pushbutton being pressed.
// Needed (rather than driving the buzzer pin directly from isDown()) so
// this alarm can be combined with the low-SOC alarm below without one
// silencing the other on the shared buzzer pin.
static bool mainsAlarmActive = false;

// Latched "cancelled for the rest of this window" state for the low-SOC
// alarm. Set true by a pushbutton press while in the 2pm-8pm window;
// cleared (re-armed) whenever the local time is outside that window, and
// starts false at boot (static initialisation), which is what gives the
// "re-enable at startup and between 2pm and 8pm if not already cancelled
// during that interval" behaviour.
static bool socAlarmCancelled = false;

// initial buzzer
void initBuzzer() {
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(buzzerPin, LOW);
}

// turn buzzer on
void turnBuzzerOn() {
  digitalWrite(buzzerPin, HIGH);
}

// turn buzzer off
void turnBuzzerOff() {
  digitalWrite(buzzerPin, LOW);
}

// Updates mainsAlarmActive from the D11 voltage-detect switch. Does NOT
// touch the buzzer pin directly -- checkMains() combines this with the
// low-SOC alarm and drives the physical pin once, so neither alarm can
// stomp on the other. Logs only on the actual activate/cancel edge, not
// every loop.
void checkForMainsFailure() {
  mainsDetectorSw.update();
  if (mainsDetectorSw.isChanged() && mainsDetectorSw.isDown()) {
    // was high and now low so lost mains
    mainsAlarmActive = true;
    logLine(F("BuzzerControl: mains-loss alarm ACTIVATED (D11 voltage lost)"));
  }
  if (mainsAlarmActive && !mainsDetectorSw.isDown()) {
    mainsAlarmActive = false; // mains restored, cancel buzzer
    logLine(F("BuzzerControl: mains-loss alarm cancelled (mains restored)"));
  }
}

// Returns true while local time is within the 2pm-8pm low-SOC alarm
// window. Fails safe to "not in window" (no alarm) if the clock has
// never been NTP-synced (missedSNTPupdate()), same convention used
// throughout this codebase (see LoadControlTask.cpp).
bool inSocAlarmWindow() {
  if (missedSNTPupdate()) {
    return false;
  }
  int mins = localTimeMinutesSinceMidnight();
  return mins >= SOC_ALARM_START_MIN && mins < SOC_ALARM_END_MIN;
}

// Returns true if the last known battery SOC reading is at or below the
// alarm threshold. latestBatterySocPercent is written by the background
// InternetPollTask (GatewayPollTask.cpp) and read here on the main
// thread, so it must be accessed under lockGatewayReading()/
// unlockGatewayReading(). A -1 reading (last fetch failed / not yet
// known) fails safe to "not low" (no alarm).
bool isSocLow() {
  float soc;
  lockGatewayReading();
  soc = latestBatterySocPercent;
  unlockGatewayReading();
  if (soc < 0) {
    return false;
  }
  return soc <= SOC_ALARM_THRESHOLD_PERCENT;
}

// True once the battery-SOC fetch has failed SOC_ALARM_FAIL_RETRY_COUNT
// times in a row -- see that constant's own comment for why this is
// treated as alarm-worthy alongside (not instead of) isSocLow().
bool socFetchStuck() {
  return consecutiveBatteryFetchFailures() >= SOC_ALARM_FAIL_RETRY_COUNT;
}

// Called every loop from the main .ino loop(). Combines the mains-loss
// alarm and the low-SOC alarm and drives the single shared buzzer pin
// from the OR of the two, so each alarm's cancel logic only affects its
// own reason and can never mask the other's. Each alarm's activation and
// cancellation is logged once, on the edge, not every loop.
void checkMains() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  checkForMainsFailure();

  sw.update();               // call this every loop to update switch state
  bool buttonPressed = sw.isDown();  // debounced pushbutton grounding D2

  if (buttonPressed && mainsAlarmActive) {
    mainsAlarmActive = false; // pushbutton also silences the mains-loss alarm
    logLine(F("BuzzerControl: mains-loss alarm cancelled (pushbutton pressed)"));
  }

  bool inWindow = inSocAlarmWindow();
  if (!inWindow) {
    socAlarmCancelled = false; // outside the window -- re-arm for next time
  }

  // Sounding BEFORE the button is applied -- a press only cancels the
  // low-SOC alarm if it was actually the (or a) reason the buzzer was
  // sounding at that moment, same precondition the mains-loss alarm above
  // already uses (buttonPressed && mainsAlarmActive). Without this check a
  // press that was only meant to silence a concurrent mains-loss alarm
  // (both alarms share the one pushbutton) would also latch
  // socAlarmCancelled for the rest of the window even though the low-SOC
  // alarm was never sounding -- confirmed as a real bug from a device log
  // where a mains-loss cancel at 16:10 (inside the window) silently armed
  // this latch with SOC never having dropped low at all.
  static bool socAlarmWasActive = false;
  bool socLow = isSocLow();
  bool socStuck = socFetchStuck();
  bool socAlarmActive = inWindow && !socAlarmCancelled && (socLow || socStuck);

  if (buttonPressed && socAlarmActive) {
    socAlarmCancelled = true;  // cancelled for the rest of this window
    socAlarmActive = false;
    logLine(F("BuzzerControl: low-SOC alarm cancelled (pushbutton pressed) -- won't sound again until the next 2pm-8pm window"));
  }

  if (socAlarmActive && !socAlarmWasActive) {
    if (socLow) {
      lockGatewayReading();
      float socPct = latestBatterySocPercent;
      unlockGatewayReading();
      logLine("BuzzerControl: low-SOC alarm ACTIVATED (SOC=" + String(socPct, 1) + "%)");
    } else {
      logLine("BuzzerControl: low-SOC alarm ACTIVATED (battery-SOC fetch failed "
          + String(consecutiveBatteryFetchFailures()) + " times in a row -- can't confirm SOC)");
    }
  }
  socAlarmWasActive = socAlarmActive;

  if (mainsAlarmActive || socAlarmActive) {
    turnBuzzerOn();
  } else {
    turnBuzzerOff();
  }
}

void initializeBuzzer() {
  pinMode(switchPinGND, OUTPUT);
  digitalWrite(switchPinGND, LOW);  // provide a GND for the switch
  initBuzzer();
  mainsOn = false;
}
