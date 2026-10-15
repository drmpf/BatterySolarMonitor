// pfodMainMenuApp.cpp  file =================
// Hand-written subclass of the generated pfodMainMenu -- see
// pfodAI-guide.md section 5.2 (and the identical button-hook pattern at
// its SubMenu_Submenu::onSubmenuButtonPressed() example): every generated
// class is reached everywhere else through a weak get_..() accessor
// specifically so a subclass like this one can take over without ever
// editing the generated file -- "Re-generating from the pfodWeb Designer
// overwrites pfodMainMenu.h/.cpp and leaves this file alone."
//
// Overrides onLogTailPressed() (pfodMainMenu.h's protected virtual hook)
// to open the streaming raw-data TEXT screen -- NOT the Raw Message
// Viewer (a different, built-in pfodApp/pfodWeb debug feature for the
// pfod protocol traffic itself); this is the {=<prompt>} "Streaming raw
// data" response, same mechanism the pfodParser library's own
// demoScreens_http.ino example uses for its "Raw Data Readings" screen.
// Immediately followed by the buffer's current contents (DebugLogBuffer.h)
// so the screen shows the recent history right away, not just new debug
// lines from this point on -- see that file's own comment for the full
// design (the 16K rolling buffer, why the same bytes also stream
// continuously via TimestampStream.h regardless of whether this screen is
// even open, so a screen left open keeps updating live, and why the
// header and the dump are sent as one locked unit).
#include "pfodMainMenu.h"
#include "DebugLogBuffer.h"
#include "LoadControlState.h"
#include "BatteryChargeSettings.h"
#include "HotDayState.h" // hotDayActive()/setHotDayActive() -- the HW/EV label's Hot Day settings and the master-disable reversion; readHotDayForecast() -- the status label's "won't discharge tonight (hot day forecast)" case
#include "ntpSupport.h"  // getTZstr()
#include "SiteSettings.h" // siteCarChargerIp()
#include "EnphaseAuth.h" // enlightenSystemId()
#include "DischargeToGridState.h" // autoDischargeEnabled()/dtgPolledEnabled()/dtgScheduleStartMinutes()/dtgScheduleEndMinutes()/dtgScheduleEndSoc() -- autoDischargeEnabled() is now the overall "Auto Battery Charge/Discharge" master switch
#include "DischargeToGridFetch.h" // formatMinutesOfDay()
#include "DischargeToGridTask.h" // requestDischargeToGridDisableNow()
#include "ChargeFromGridState.h" // cfgPolledEnabled()/cfgScheduleStartMinutes()/cfgScheduleEndMinutes()/cfgScheduleTargetSoc()
#include "HotDayTask.h" // requestChargeFromGridDisableNow()
#include "WeatherForecastInstances.h" // tomorrowWeatherForecast -- status label's own cold-day reason, see tomorrowColdDayForecast()
#include "SolarForecastTask.h" // lockSolarForecast()/unlockSolarForecast() -- also guards tomorrowWeatherForecast
#include "PlotDataInstances.h" // tomorrowSolarData/tomorrowAvgConsumptionData/lockTomorrowForecastData() -- status label's own excess reason, see tomorrowExcessTooLow()
#include "AvgConsumptionRefresh.h" // avgConsumptionForecastReady()
#include "SolarForecastRefresh.h"  // solarForecastReady()
#include "CarChargerTask.h" // lockCarChargerState()/unlockCarChargerState() -- status label's own car-charge reason, see carChargeTodayTooLow()
#include "CarChargerInstances.h" // carChargingKwhToday

// label_Set_Time_Timezo_var (plain global, external linkage, defined in
// the generated pfodMainMenu.cpp) is the "current text" pointer
// sendMainMenu()/sendMainMenuUpdate() print for the '[Set Time/Timezone]'
// label -- not declared in pfodMainMenu.h, so declared extern here instead
// of editing that generated header. Per its own generated comment: "change
// this + call sendMainMenuUpdate() to update the label".
extern const char *label_Set_Time_Timezo_var;

// Same pattern, for the 'HW/EV - Battery Charge%...' sub-menu BUTTON's own
// text (submenu_HW_EV_Battery_C_var, generated pfodMainMenu.cpp) -- added
// 2026-09-21, per explicit instruction, so the button shows the actual
// persisted HW/EV turn-on settings (BatteryChargeSettings.h) instead of the
// Designer's static placeholder text.
extern const char *submenu_HW_EV_Battery_C_var;

// Same pattern, for the '[EV Settings]'/'[Enphase Settings]' labels' own
// text (label_EV_Settings_htt_var/label_Enphase_Setting_var, generated
// pfodMainMenu.cpp) -- the Designer bakes in whatever literal LAN IP/
// Enphase system ID the project was generated with, which would otherwise
// hardcode this installation's own real network/account identifiers into
// published source. Rebuilt from site_settings.csv's own loaded config
// instead -- see initDynamicMenuLabels() below.
extern const char *label_EV_Settings_htt_var;
extern const char *label_Enphase_Setting_var;

// Same pattern, for the two items the 2026-10-13 pfodWeb Designer
// regeneration split the old single discharge button into --
// label_Battery_will_di_var (a plain LABEL, placeholder "Battery will
// discharge to grid..\nBattery will charge from grid..") for the
// discharge-to-grid/charge-from-grid STATUS report, and
// button_Auto_Battery_Ch_var (the actual clickable button, placeholder
// "<g><b>Auto Battery Charge/Discharge Enabled</g>\nClick here to Di...")
// for the master switch toggle itself -- added per explicit instruction
// so these show the actual persisted/polled state instead of the
// Designer's static placeholder text. The generated virtual hook for the
// button is onAutoBatteryChargeDisPressed() (pfodMainMenu.h), replacing
// the old onBatteryCanDischargeTPressed() from before this regeneration.
extern const char *label_Battery_will_di_var;
extern const char *button_Auto_Battery_Ch_var;

class MyMainMenu : public pfodMainMenu {
  public:
    virtual void sendMainMenu(pfodParser &parser);
    virtual void sendMainMenuUpdate(pfodParser &parser);
  protected:
    virtual void onAutoBatteryChargeDisPressed(pfodParser &parser);
    virtual void onLogTailPressed(pfodParser &parser);
    virtual void onClearRecoveryFlagAutPressed(pfodParser &parser);
};

// Static, program-lifetime buffer -- label_Set_Time_Timezo_var is a raw
// const char*, so whatever it points at must outlive the parser.print()
// call that reads it; a String local to updateTzFormatLabel() would be
// destroyed (and the pointer left dangling) the moment that function
// returns, before sendMainMenu()/sendMainMenuUpdate() ever prints it.
static String tzFormatLabelText;

// Appends the current POSIX TZ string on its own line below the
// '[Set Time/Timezone](/settz.html)' link -- '\n' inside a pfod label
// renders as a real line break, same convention already used by this
// menu's own "Clear Recovery Flag" button (its 2nd line, below). Recomputed
// fresh every time right before either send method actually prints it
// (see the two overrides below), so it's never stale even though nothing
// pushes an update when the TZ changes elsewhere (webPages.cpp's
// handle_setTZstr()/handle_resetTZ()/handle_setTime()) -- the next time
// this menu is sent for any reason (reconnect, a button press elsewhere in
// the menu, its own refresh) picks up the current value.
static void updateTzFormatLabel() {
  tzFormatLabelText = "[Set Time/Timezone](/settz.html)\n<-3>";
  tzFormatLabelText += getTZstr();
  label_Set_Time_Timezo_var = tzFormatLabelText.c_str();
}

// Same "static buffer + recompute fresh before every send" pattern as
// updateTzFormatLabel() above, for the 'HW/EV - Battery Charge%' sub-menu
// button's own text -- added 2026-09-21, per explicit instruction, so the
// button always shows the actual persisted turn-on settings
// (BatteryChargeSettings.h), which can change from the sub-menu's own
// sliders (SubMenu_HW_EV___Battery_CharApp.cpp) without this main menu ever
// being told directly. Expanded 2026-10-13, per explicit instruction, to
// also show the Hot Day HW/Car% settings and trigger temperature -- same
// reasoning, can change from the same sub-menu's own sliders without this
// main menu being told directly either. The live battery SOC% this once
// also showed (GatewayInstances.h's latestBatterySocPercent) was dropped
// the same day, per explicit instruction, in favour of the trigger
// temperature.
static String batteryChargeLabelText;

// Appends "OFF" instead of a literal "99%"/"100%" -- a turn-on% of 99 or
// 100 means "disabled" (LoadControlTask.cpp's own startConditionsMet()/
// keepConditionsMet() -- never starts, stops if already running), which
// would otherwise misleadingly read as just a very high threshold. Shared
// by all four percentages below (default and Hot Day, HW and EV) -- same
// rule, same threshold, for each.
static void appendOnPercent(int percent) {
  if (percent >= 99) {
    batteryChargeLabelText += "OFF";
  } else {
    batteryChargeLabelText += percent;
    batteryChargeLabelText += "%";
  }
}

static void updateBatteryChargeLabel() {
  int hwOnPercent = hwSocOnPercent();
  int evOnPercent = evSocOnPercent();
  int hotDayTriggerC = tomorrowsMaxTempTriggerC();
  int hotDayHwOnPct = hotDayHwOnPercent();
  int hotDayCarOnPct = hotDayCarOnPercent();

  batteryChargeLabelText = "HW/EV - Battery Charge%\nto start charging.\n<b>Set at HW ";
  appendOnPercent(hwOnPercent);
  batteryChargeLabelText += " EV ";
  appendOnPercent(evOnPercent);
  batteryChargeLabelText += "\nHot Day >";
  batteryChargeLabelText += hotDayTriggerC;
  batteryChargeLabelText += "C HW ";
  appendOnPercent(hotDayHwOnPct);
  batteryChargeLabelText += " EV ";
  appendOnPercent(hotDayCarOnPct);
  batteryChargeLabelText += "</b>\n<i>Click here to change settings";
  submenu_HW_EV_Battery_C_var = batteryChargeLabelText.c_str();
}

// Static, program-lifetime buffers -- same "must outlive the parser.print()
// call" reasoning as tzFormatLabelText above.
static String evSettingsLabelText;
static String enphaseSettingsLabelText;

// Rebuilds the '[EV Settings]'/'[Enphase Settings]' menu links from
// site_settings.csv's own loaded config (siteCarChargerIp()/
// enlightenSystemId()), instead of the literal LAN IP/Enphase system ID
// the pfodWeb Designer baked into pfodMainMenu.cpp's generated defaults for
// this installation. Unlike updateTzFormatLabel()/updateBatteryChargeLabel()
// above, this is called ONCE from setup() (BatterySolarMonitor.ino, right
// after loadSiteConfig()/loadEnphaseAuth() succeed) rather than fresh
// before every send -- both source values are fixed config, loaded once at
// boot and never changed at runtime, so there's nothing to go stale.
void initDynamicMenuLabels() {
  evSettingsLabelText = "[EV Settings](http://";
  evSettingsLabelText += siteCarChargerIp();
  evSettingsLabelText += "/index.html)";
  label_EV_Settings_htt_var = evSettingsLabelText.c_str();

  enphaseSettingsLabelText = "[Enphase Settings](https://enlighten.enphaseenergy.com/web/";
  enphaseSettingsLabelText += enlightenSystemId();
  enphaseSettingsLabelText += ")";
  label_Enphase_Setting_var = enphaseSettingsLabelText.c_str();
}

// Static, program-lifetime buffers -- same "must outlive the parser.print()
// call" reasoning as tzFormatLabelText above.
static String autoBatteryButtonLabelText;
static String batteryStatusLabelText;

// Rebuilds button_Auto_Battery_Ch_var from the master switch latch alone
// (DischargeToGridState.h's autoDischargeEnabled(), now the overall "Auto
// Battery Charge/Discharge" switch covering discharge-to-grid AND Hot
// Day/charge-from-grid) -- per explicit instruction (2026-10-13):
//   enabled  -> "Auto Battery Charge/Discharge Enabled", click to Disable
//   disabled -> "Auto Battery Charge/Discharge Disabled", click to Enable
// Recomputed fresh before every send (same pattern as
// updateTzFormatLabel()/updateBatteryChargeLabel() above).
static void updateAutoBatteryButtonLabel() {
  if (autoDischargeEnabled()) {
    autoBatteryButtonLabelText = "<g><b>Auto Battery Charge/Discharge Enabled</g>\nClick here to Disable";
  } else {
    autoBatteryButtonLabelText = "<r><b>Auto Battery Charge/Discharge Disabled</r>\nClick here to Enable";
  }
  button_Auto_Battery_Ch_var = autoBatteryButtonLabelText.c_str();
}

// Appends "<end%>% between <start> and <end>" to batteryStatusLabelText --
// the schedule detail shared by both the "will" and "can" discharge-to-
// grid cases below (DischargeToGridFetch.h's formatMinutesOfDay()), "?"
// for anything not polled yet -- never guesses a %.
static void appendDtgWindowDetail() {
  float endSoc = dtgScheduleEndSoc();
  if (endSoc < 0.0) {
    batteryStatusLabelText += "?";
  } else {
    batteryStatusLabelText += String((int)endSoc);
  }
  batteryStatusLabelText += "%\nbetween ";
  batteryStatusLabelText += formatMinutesOfDay(dtgScheduleStartMinutes());
  batteryStatusLabelText += " and ";
  batteryStatusLabelText += formatMinutesOfDay(dtgScheduleEndMinutes());
}

// Appends "<start> and <end>" to batteryStatusLabelText -- the cfg
// schedule's own window, shared by every "charge from grid"/"charge from
// grid tomorrow" case below. Same window both "today" and "tomorrow" read
// -- it's the SAME recurring Enphase cfg schedule (confirmed from the HAR:
// "days":[1..7]), just whichever calendar day it's about to apply (or is
// already applying) to; there's no separate "tomorrow's schedule" to
// fetch. "?" for anything not polled yet -- never guesses a time.
static void appendCfgWindowDetail() {
  batteryStatusLabelText += formatMinutesOfDay(cfgScheduleStartMinutes());
  batteryStatusLabelText += " and ";
  batteryStatusLabelText += formatMinutesOfDay(cfgScheduleEndMinutes());
}

// Appends "<target%>%" (just the percent, no window) to
// batteryStatusLabelText -- cfgScheduleTargetSoc() (ChargeFromGridState.h)
// is the % the battery is charged UP TO. Added 2026-10-14, per explicit
// instruction ("the charge from grid has an upper % as well"), originally
// for line 4's raw Enphase status readout only, then reused by line 2
// further down (per a follow-up explicit instruction -- "should be ..
// will charge from grid to % tomorrow...") once it turned out both
// needed the same figure, just worded into different surrounding text.
// "?" if not polled yet -- never guesses a %, same convention as
// appendDtgWindowDetail()'s own endSoc check.
static void appendCfgTargetSocPercent() {
  float targetSoc = cfgScheduleTargetSoc();
  if (targetSoc < 0.0) {
    batteryStatusLabelText += "?";
  } else {
    batteryStatusLabelText += String((int)targetSoc);
  }
  batteryStatusLabelText += "%";
}

// Appends "<target%>% between <start> and <end>" to batteryStatusLabelText
// -- same self-contained "%, then the window" shape as
// appendDtgWindowDetail() above, built from appendCfgTargetSocPercent()
// and appendCfgWindowDetail() just above. Used by line 4's raw Enphase
// status readout, further down.
static void appendCfgWindowDetailWithTargetSoc() {
  appendCfgTargetSocPercent();
  batteryStatusLabelText += " between ";
  appendCfgWindowDetail();
}

// True only if tomorrow's forecast max temp has actually been received
// AND is <= DischargeToGridTask.h's own coldDayMaxTempC() -- mirrors
// DischargeToGridTask.cpp's own Cold Day gate exactly (same fields/lock
// HotDayState.cpp's own readHotDayForecast() already reads for its own
// hot-day comparison, read directly here rather than through that
// function since this is a different comparison), so this label can
// never disagree with the real decision. Never guesses "cold" from a
// forecast that hasn't arrived yet -- returns false (not this reason;
// the generic fallback further down then applies), same as every other
// "not ready" case in this project.
static bool tomorrowColdDayForecast() {
  lockSolarForecast();
  bool ready = tomorrowWeatherForecast.received;
  float tempMaxC = tomorrowWeatherForecast.tempMaxC;
  unlockSolarForecast();
  return ready && (tempMaxC <= coldDayMaxTempC());
}

// True only if both of tomorrow's forecast totals have actually arrived
// AND the resulting excess is below DischargeToGridTask.h's own
// minTomorrowExcessKwh() -- mirrors DischargeToGridTask.cpp's own excess
// threshold exactly (same totals/lock/readiness gate
// checkDischargeToGridLogic() itself reads). Never guesses from
// still-dummy-fill/placeholder data -- returns false while not ready.
static bool tomorrowExcessTooLow() {
  lockTomorrowForecastData();
  bool ready = avgConsumptionForecastReady() && solarForecastReady();
  float tomorrowSolarKwh = ready ? tomorrowSolarData.totalKwh() : 0.0;
  float avgConsumptionKwh = ready ? tomorrowAvgConsumptionData.totalKwh() : 0.0;
  unlockTomorrowForecastData();
  return ready && ((tomorrowSolarKwh - avgConsumptionKwh) < minTomorrowExcessKwh());
}

// True if today's car-charging energy so far is below
// DischargeToGridTask.h's own minCarChargeTodayKwh() -- mirrors
// DischargeToGridTask.cpp's own car-charge threshold exactly.
// carChargingKwhToday (CarChargerInstances.h) always holds a real value
// (0.0 at worst, from a genuine midnight reset or a genuinely-first-boot
// default), unlike the forecast totals above -- there's no "not ready"
// case to guard here.
static bool carChargeTodayTooLow() {
  lockCarChargerState();
  float carChargeTodayKwh = carChargingKwhToday;
  unlockCarChargerState();
  return carChargeTodayKwh < minCarChargeTodayKwh();
}

// Rebuilds label_Battery_will_di_var -- the plain status LABEL the
// 2026-10-13 pfodWeb Designer regeneration added alongside the button
// above, whose placeholder text ("Battery will discharge to grid..\n
// Battery will charge from grid..") is exactly the two-line report per
// explicit instruction: "if disabled, just report discharge to grid and
// charge from grid settings, but do not automatically re-enable". Line 2
// still reports Enphase's own actual polled state UNCONDITIONALLY,
// regardless of the master switch -- both status polls
// (DischargeToGridTask.cpp's/HotDayTask.cpp's) run every background-task
// tick independent of autoDischargeEnabled(), only the DECISION logic is
// gated on it. Line 1, as of 2026-10-14, is the one exception -- per
// explicit instruction ("Battery auto disabled => will not discharge"),
// it checks the master switch FIRST and reports "will not discharge
// \n(auto disabled)" rather than Enphase's raw polled state, since that
// state can briefly still read stale/"on" in the short window right
// after the switch is disabled, before the resulting disable request has
// actually been confirmed off. tomorrowHot/tomorrowReady (read ONCE,
// via HotDayState.h's readHotDayForecast(), and shared by both lines) is
// the SAME test DischargeToGridTask.cpp's own checkDischargeToGridLogic()
// uses to hold discharge off all evening -- never guesses if the forecast
// isn't in yet:
//   line 1 -- discharge-to-grid: checked in this order --
//               - !autoDischargeEnabled() (added 2026-10-14, per explicit
//                 instruction -- "Battery auto disabled => will not
//                 discharge") -> "will not discharge\n(auto disabled)",
//                 taking priority over dtgPolledEnabled() just below: the
//                 master switch being off means the system's own INTENT
//                 is already "off" (onAutoBatteryChargeDisPressed()'s
//                 one-shot disable request, backed by runStatusPoll()'s
//                 own ongoing resend-on-mismatch for as long as this stays
//                 true), so this must not fall through to a possibly-stale
//                 "will discharge" reading from the brief window before
//                 that poll has actually confirmed Enphase caught up.
//             "will" (bold red) if Enphase's own dtgControl.enabled is
//             currently true; else reports SPECIFICALLY which of
//             checkDischargeToGridLogic()'s own gates is currently
//             withholding discharge (added 2026-10-14, per explicit
//             instruction -- "show what is stopping the discharge to
//             grid"), tested in the SAME order that function itself tests
//             them so this label can never name a reason that isn't
//             really the (first) one actually blocking it:
//               - tomorrowReady && tomorrowHot (added 2026-10-13, per
//                 explicit instruction -- "Battery can discharge is not
//                 accurate if it is a hot day tomorrow") -> "won't
//                 discharge tonight\n(hot day forecast)".
//               - tomorrowColdDayForecast() -> "won't discharge tonight
//                 \n(cold day forecast)".
//               - tomorrowExcessTooLow() -> "won't discharge tonight
//                 \n(tomorrow's forecast excess <NkWh)" (N = this file's
//                 own minTomorrowExcessKwh(), read from DischargeToGridTask.h
//                 so the number can never drift out of sync with the real
//                 threshold).
//               - carChargeTodayTooLow() -> "won't discharge tonight
//                 \n(car charge today <NkWh)" (N = minCarChargeTodayKwh(),
//                 same reasoning).
//             Every "won't discharge tonight"/"will not discharge" case
//             above puts its own "(reason)" on a SEPARATE line (added
//             2026-10-14, per explicit instruction -- "add new line
//             before reason in each case a reason is given") -- the
//             dtgControl.enabled==true and genuinely-available cases
//             don't have a parenthetical reason at all, so they're
//             unaffected.
//               - none of the above -> none of the gates are blocking it,
//                 so it WILL discharge (changed from "can" 2026-10-14, per
//                 explicit instruction -- "why would it be can if no 1 to
//                 4 conditions?"): "will discharge to <end%>% between
//                 <start> and <end>" (appendDtgWindowDetail()). Whole line
//                 red like the dtgControl.enabled==true case above (added
//                 2026-10-14, per explicit instruction -- "if Battery will
//                 dischage make that line red"), just not ALSO bold --
//                 this is the decision logic's own intent, not yet
//                 confirmed by an actual Enphase poll, but still a "will
//                 discharge" statement, so still red.
//   line 2 -- charge-from-grid, expanded 2026-10-13 per explicit
//             instruction to also report the Hot Day forecast/schedule,
//             not just today's live on/off; the target% was added
//             2026-10-14, per a follow-up explicit instruction ("should
//             be .. will charge from grid to % tomorrow...") -- reuses
//             appendCfgTargetSocPercent() (added for line 4, just above),
//             so this and line 4 can never show a different % for the
//             same underlying cfgScheduleTargetSoc():
//             Every "will charge from grid..." case below is a WHOLE LINE
//             red (added 2026-10-14, per explicit instruction -- "if
//             battery will charge tomorrow or today make that line red"),
//             "will" ALSO bold for the live/confirmed case only, same
//             shape as line 1's own red cases above; "will not charge
//             from grid" is NOT red (it's a "will NOT" statement):
//               - cfgPolledEnabled() true (bold red "will", live/confirmed) ->
//                 "will charge from grid to <target%>% between <start>
//                 and <end>" (appendCfgTargetSocPercent() +
//                 appendCfgWindowDetail()) -- PLUS, if tomorrowReady &&
//                 tomorrowHot too (today's own Hot Day carrying straight
//                 into tomorrow's), a SECOND line, also red, not bold:
//                 "Battery will charge from grid to <target%>% tomorrow
//                 between <start> and <end>".
//               - cfgPolledEnabled() false but tomorrowReady && tomorrowHot
//                 (forecast/plan, not yet live, so red but not bold) ->
//                 "will charge from grid to <target%>% tomorrow between
//                 <start> and <end>".
//               - neither -> "will not charge from grid" (not red).
//             Both "today" and "tomorrow" read the SAME cfg schedule
//             window (appendCfgWindowDetail()'s own comment) -- there's
//             only one recurring Enphase schedule, not a separate one per
//             day.
//   lines 3/4 -- added 2026-10-14, per explicit instruction ("regardless
//             of auto state ... should be the current enphase setting"):
//             a plain, raw readout of Enphase's own actual polled on/off
//             state, UNCONDITIONALLY -- unlike lines 1/2 above (which, as
//             of 2026-10-14/2026-10-13 respectively, report INTENT/
//             reasoning, not always the raw reading), these two never
//             take the master switch or any gate into account at all:
//             Both lines italic (added 2026-10-14, per explicit
//             instruction -- "make Enphase settings italic"): a SINGLE
//             <i> opened at the start of line 3, left unclosed -- no
//             need to close it at the end of line 3 and reopen it on
//             line 4 (per explicit correction), since the \n between them
//             doesn't reset the markup; it covers both lines through to
//             the end of the whole label, same "leave the last tag in a
//             string unclosed" convention updateBatteryChargeLabel()'s
//             own trailing <i> CTA line already follows.
//               - "Enphase Discharge to Grid down to <end%>% between
//                 <start> and <end>" (appendDtgWindowDetail()) if
//                 dtgPolledEnabled(), else "Enphase Discharge to Grid
//                 disabled". Wording changed from "enabled <end%>%" to
//                 "down to <end%>%" 2026-10-14, per explicit instruction.
//               - "Enphase Charge from Grid up to <target%>% between
//                 <start> and <end>" (appendCfgWindowDetailWithTargetSoc())
//                 if cfgPolledEnabled(), else "Enphase Charge from Grid
//                 disabled". The target% added 2026-10-14, per explicit
//                 instruction ("the charge from grid has an upper % as
//                 well") -- cfgScheduleTargetSoc() (ChargeFromGridState.h),
//                 the mirror-image of line 3's own dtgScheduleEndSoc().
static void updateBatteryStatusLabel() {
  bool tomorrowHot = false;
  bool tomorrowReady = readHotDayForecast(tomorrowHot);

  batteryStatusLabelText = "";
  if (!autoDischargeEnabled()) {
    // Added 2026-10-14, per explicit instruction ("Battery auto disabled
    // => will not discharge") -- takes priority over dtgPolledEnabled()
    // below: the master switch being off means
    // onAutoBatteryChargeDisPressed() has already fired
    // requestDischargeToGridDisableNow(), and runStatusPoll()'s own
    // resend-on-mismatch keeps actively forcing Enphase off for as long
    // as this stays true, so the real INTENT is "will not discharge"
    // even during the brief window right after disabling, before that
    // poll has actually confirmed Enphase caught up -- showing the stale
    // "will discharge" reading in that window would look alarming/wrong
    // immediately after the user just disabled it.
    batteryStatusLabelText += "Battery will not discharge\n(auto disabled)";
  } else if (dtgPolledEnabled()) {
    // Whole line red (added 2026-10-14, per explicit instruction -- "if
    // Battery will dischage make that line red"), "will" ALSO bold --
    // <b>'s own </b> only terminates the <b> itself (and anything nested
    // inside it, which here is nothing), not the enclosing <r>, so the
    // rest of the line after "will" stays red; the explicit trailing
    // </r> below is still needed since line 2 follows on the same label.
    batteryStatusLabelText += "<r>Battery <b>will</b> discharge to ";
    appendDtgWindowDetail();
    batteryStatusLabelText += "</r>";
  } else if (tomorrowReady && tomorrowHot) {
    batteryStatusLabelText += "Battery won't discharge tonight\n(hot day forecast)";
  } else if (tomorrowColdDayForecast()) {
    batteryStatusLabelText += "Battery won't discharge tonight\n(cold day forecast)";
  } else if (tomorrowExcessTooLow()) {
    // "<NkWh" -- a bare '<' here briefly broke rendering on a real device
    // (menuLabel.jpg, 2026-10-14: pfodButtonRenderer.js/redraw.js's
    // pfodWeb_src tag scanner mis-parsed this as a colour tag, via
    // pfodColorTagToHex()'s own parseInt() leniency -- "10kWh)..." parsed
    // as palette colour 10, lime/green). Reverted back to '<' once that
    // root cause was fixed at the source (pfodWeb_src/redraw.js's
    // pfodColorTagToHex(), pfodWeb_src/pfodButtonRenderer.js's scanner),
    // per explicit instruction, rather than permanently avoiding '<' in
    // ESP32-side display text.
    batteryStatusLabelText += "Battery won't discharge tonight\n(tomorrow's forecast excess <";
    batteryStatusLabelText += String((int)minTomorrowExcessKwh());
    batteryStatusLabelText += "kWh)";
  } else if (carChargeTodayTooLow()) {
    // Same '<' -- see tomorrowExcessTooLow()'s own branch just above.
    batteryStatusLabelText += "Battery won't discharge tonight\n(car charge today <";
    batteryStatusLabelText += String((int)minCarChargeTodayKwh());
    batteryStatusLabelText += "kWh)";
  } else {
    // Same whole-line-red treatment as the confirmed case above, just no
    // bold -- this is the decision logic's own intent, not yet confirmed
    // by an actual Enphase poll (see this function's own header comment),
    // but it's still a "will discharge" statement, so still red.
    batteryStatusLabelText += "<r>Battery will discharge to ";
    appendDtgWindowDetail();
    batteryStatusLabelText += "</r>";
  }

  if (cfgPolledEnabled()) {
    // Same whole-line-red treatment as line 1's own confirmed case, same
    // reasoning (added 2026-10-14, per explicit instruction -- "if
    // battery will charge tomorrow or today make that line red").
    batteryStatusLabelText += "\n<r>Battery <b>will</b> charge from grid to ";
    appendCfgTargetSocPercent();
    batteryStatusLabelText += "\nbetween ";
    appendCfgWindowDetail();
    batteryStatusLabelText += "</r>";
    if (tomorrowReady && tomorrowHot) {
      batteryStatusLabelText += "\n<r>Battery will charge from grid to ";
      appendCfgTargetSocPercent();
      batteryStatusLabelText += " tomorrow\nbetween ";
      appendCfgWindowDetail();
      batteryStatusLabelText += "</r>";
    }
  } else if (tomorrowReady && tomorrowHot) {
    batteryStatusLabelText += "\n<r>Battery will charge from grid to ";
    appendCfgTargetSocPercent();
    batteryStatusLabelText += " tomorrow\n\etween ";
    appendCfgWindowDetail();
    batteryStatusLabelText += "</r>";
  } else {
    batteryStatusLabelText += "\nBattery will not charge from grid";
  }

  // Lines 3/4 -- added 2026-10-14, per explicit instruction ("regardless
  // of auto state ... should be the current enphase setting"): Enphase's
  // own actual polled on/off state, UNCONDITIONALLY, regardless of the
  // master switch OR any of lines 1/2's own intent/reasoning above --
  // these two are a plain, raw status readout, not a decision report.
  batteryStatusLabelText += "\n<i>Enphase Discharge to Grid ";
  if (dtgPolledEnabled()) {
    batteryStatusLabelText += "down to ";
    appendDtgWindowDetail();
  } else {
    batteryStatusLabelText += "disabled";
  }

  batteryStatusLabelText += "\nEnphase Charge from Grid ";
  if (cfgPolledEnabled()) {
    batteryStatusLabelText += "up to ";
    appendCfgWindowDetailWithTargetSoc();
  } else {
    batteryStatusLabelText += "disabled";
  }

  label_Battery_will_di_var = batteryStatusLabelText.c_str();
}

void MyMainMenu::sendMainMenu(pfodParser &parser) {
  updateTzFormatLabel();
  updateBatteryChargeLabel();
  updateBatteryStatusLabel();
  updateAutoBatteryButtonLabel();
  pfodMainMenu::sendMainMenu(parser);
}

// Fully overridden, NOT delegated -- unlike sendMainMenu() above, the
// regenerated pfodMainMenu::sendMainMenuUpdate() (2026-09-17 rebuild) no
// longer refreshes most items by default, only the 3 drawing menu items
// (Home/TodaySolar/Tomorrow); it doesn't touch label_Set_Time_Timezo_Cmd
// at all any more, so calling it wouldn't send the TZ text no matter how
// current updateTzFormatLabel() left the underlying var. And since
// parser.menuUpdate()/endOfMsg() bracket the whole {;...} message, there's
// no way to call the base version first and then print more afterward --
// the message would already be closed. So this duplicates the base's own
// 3 drawing-item lines (per explicit instruction: only the Set
// Time/Timezone label needs refreshing here, every other label/button is
// static and intentionally left unrefreshed) and adds the TZ label update.
// 2026-09-21: also added the HW/EV Battery Charge% sub-menu button's own
// text update (updateBatteryChargeLabel()), for the same reason -- its
// underlying settings can change from that sub-menu's own sliders without
// this main menu being told directly.
// NOTE: if the Designer ever changes which drawing items the regenerated
// sendMainMenuUpdate() itself refreshes, re-sync those 3 lines by hand --
// check pfodMainMenu.cpp's own version after any future regeneration.
void MyMainMenu::sendMainMenuUpdate(pfodParser &parser) {
  updateTzFormatLabel();
  updateBatteryChargeLabel();
  updateBatteryStatusLabel();
  updateAutoBatteryButtonLabel();
  parser.menuUpdate();  // start an Update Menu pfod message. Send {;
  parser.print(F("|+")); // drawing menu item update
  parser.print(dwgMenuItem_Home_Cmd);
  parser.print(F("|+")); // drawing menu item update
  parser.print(dwgMenuItem_TodaySolar_Cmd);
  parser.print(F("|+")); // drawing menu item update
  parser.print(dwgMenuItem_Tomorrow_Cmd);
  parser.label(label_Set_Time_Timezo_Cmd); // start Label
  parser.print(F("~"));
  parser.print(label_Set_Time_Timezo_var); // current TZ text, set by updateTzFormatLabel() above
  parser.button(submenu_HW_EV_Battery_C_Cmd); // start Button (opens sub-menu)
  parser.print(F("~"));
  parser.print(submenu_HW_EV_Battery_C_var); // current HW/EV settings text, set by updateBatteryChargeLabel() above
  parser.label(label_Battery_will_di_Cmd); // start Label
  parser.print(F("~"));
  parser.print(label_Battery_will_di_var); // current discharge/charge-from-grid status text, set by updateBatteryStatusLabel() above
  parser.button(button_Auto_Battery_Ch_Cmd); // start Button
  parser.print(F("~"));
  parser.print(button_Auto_Battery_Ch_var); // current master-switch text, set by updateAutoBatteryButtonLabel() above
  parser.endOfMsg();  // close pfod message. Send }
}

// "Auto Battery Charge/Discharge Enabled/Disabled" button -- toggles the
// OVERALL master switch (DischargeToGridState.h's autoDischargeEnabled()),
// which as of 2026-10-13 gates BOTH DischargeToGridTask.cpp's discharge
// decision AND HotDayTask.cpp's Hot Day decision, per explicit
// instruction. Switching TO disabled immediately performs the whole
// "stop check and disable both discharge to grid and charge from grid
// and switch HW/car back to the default setting" transition in one shot:
//   - requestDischargeToGridDisableNow() arms DischargeToGridTask.cpp's
//     one-shot action to force Enphase's dtgControl.enabled to false
//     (unchanged from before this feature).
//   - requestChargeFromGridDisableNow() arms HotDayTask.cpp's own
//     equivalent one-shot action for Enphase's chargeFromGrid flag.
//   - setHotDayActive(false) immediately reverts the HW/Car% in effect
//     (HotDayState.h's effectiveHwSocOnPercent()/effectiveEvSocOnPercent(),
//     read by LoadControlTask.cpp) back to the ordinary defaults, rather
//     than leaving them on the Hot Day settings until the next 2:02pm.
// Switching TO enabled only changes the latch -- deciding whether/when to
// actually turn Enphase's settings back on, and whether tomorrow is a Hot
// Day, is DischargeToGridTask.cpp's/HotDayTask.cpp's own decision logic,
// which both read their own forecast/schedule data directly themselves,
// starting completely fresh (see each one's own disabled-branch comment).
// Responds with sendMainMenuUpdate() (not a bare "{}") so the button's own
// text reflects the new state immediately, instead of waiting for the next
// full refresh.
void MyMainMenu::onAutoBatteryChargeDisPressed(pfodParser &parser) {
  bool nowEnabled = !autoDischargeEnabled();
  setAutoDischargeEnabled(nowEnabled);
  if (!nowEnabled) {
    requestDischargeToGridDisableNow();
    requestChargeFromGridDisableNow();
    setHotDayActive(false);
  }
  sendMainMenuUpdate(parser);
}

void MyMainMenu::onLogTailPressed(pfodParser &parser) {
  // Prints the {=…~C} (clears any existing raw data on THIS client's own
  // screen) then resets this same client's own raw-data read cursor back
  // to the start -- see DebugLogBuffer.h's own comment. Purely a per-client
  // reset now, not a shared/locked operation -- no other viewer's own Log
  // Tail screen is affected either way.
  debugLogBufferDumpAll(parser, F("{=Last 16K of Debug Log~C}"));
}

// "Clear Recovery Flag" button -- lets a person manually cancel a pending
// V2H auto-recovery (LoadControlTask.cpp's carCheckStart(), see
// esp32_control_structure.md sec 5a), the same LittleFS-persisted flag
// v2hScheduleConfirmsOn() already cancels automatically outside the
// charger's own Auto on-window. Only clears the flag -- does not touch
// V2H itself -- matching that same "stand down" behaviour: whatever
// already turned V2H off (a real trip, or the charger's own schedule)
// is left exactly as it is, this just stops the next scheduled retry.
void MyMainMenu::onClearRecoveryFlagAutPressed(pfodParser &parser) {
  setCarV2hRecoveryPending(false);
  parser.print(F("{}"));
}

MyMainMenu myMainMenu;
pfodMainMenu& get_pfodMainMenu() { return myMainMenu; } // replaces the weak default sketch-wide
// ============== end of pfodMainMenuApp.cpp  file
