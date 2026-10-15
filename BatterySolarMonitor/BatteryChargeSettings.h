// BatteryChargeSettings.h  file  =================
// Persists the two independent SOC% turn-on thresholds (HW and EV/car)
// LoadControlTask.cpp's hwCheckStart()/hwCheckStop()/carCheckStart() used
// to gate on directly, and that the pfodWeb "HW/EV - Battery Charge%"
// sub-menu's two sliders (SubMenu_HW_EV___Battery_Char.h/.cpp) read and
// set live. Added 2026-09-21, per explicit instruction, replacing the
// single hardcoded SOC_CONTROL_START/SOC_CONTROL_STOP pair
// LoadControlTask.cpp used to apply to both loads alike. As of 2026-10-13,
// LoadControlTask.cpp no longer calls hwSocOnPercent()/evSocOnPercent()
// (or their OffPercent() siblings) directly -- it calls HotDayState.h's
// effectiveHwSocOnPercent()/effectiveEvSocOnPercent() instead, which read
// THESE accessors (the "default" values) only while a Hot Day isn't
// active, and the Hot Day accessors further below otherwise. These
// accessors themselves are unchanged; only who calls them directly has.
//
// The turn-off threshold is never independently stored -- per explicit
// instruction it is always exactly 5% below that load's own turn-on
// threshold, so there is only one persisted number per load, not two.
//
// Mirrors LoadControlState.h's LittleFS persistence pattern (mutex-guarded,
// lazy-loaded on first use, only written on an actual change) -- same
// cross-task shape too: LoadControlTask.cpp reads these on the shared
// background task (InternetPollTask.cpp) while a person moving a slider in
// the pfodWeb sub-menu writes them from whatever task services that HTTP
// request.
#ifndef BATTERY_CHARGE_SETTINGS_H
#define BATTERY_CHARGE_SETTINGS_H

// Defaults used both on a genuinely first boot (no file yet) and whenever
// the saved file is missing/unreadable/out of range -- per explicit
// instruction, now DIFFERENT per load (HW 40%, EV/battery 35%), replacing
// the single shared default (35% for both) this originally used, which
// itself replaced the original shared SOC_CONTROL_START both loads used
// before this file existed.
static const int BATTERY_CHARGE_DEFAULT_HW_ON_PERCENT = 40;
static const int BATTERY_CHARGE_DEFAULT_EV_ON_PERCENT = 35;

// Defaults for the "Hot Day" trigger temperature and the HW/Car start%
// to switch to on a hot day -- used both on a genuinely first boot (no
// file yet) and whenever the saved Hot Day group is missing/unreadable/
// out of range (see loadIfNeeded()'s own all-or-nothing group handling,
// BatteryChargeSettings.cpp). See HotDayState.h for what actually USES
// these (hotDayActive()-gated effectiveHwSocOnPercent()/
// effectiveEvSocOnPercent(), and readHotDayForecast()'s own trigger
// comparison) -- this file only stores them.
static const int HOT_DAY_DEFAULT_MAX_TEMP_TRIGGER_C = 30;
static const int HOT_DAY_DEFAULT_HW_ON_PERCENT = 95;
static const int HOT_DAY_DEFAULT_CAR_ON_PERCENT = 90;

// Turn-on / turn-off SOC% thresholds. On first call, loads
// /battery_charge_settings.txt from LittleFS; if it's missing, unreadable,
// or either saved value parses outside the sub-menu sliders' own valid
// range (10-100, per SubMenu_HW_EV___Battery_Char.cpp's own prompt text),
// BOTH thresholds default to their own BATTERY_CHARGE_DEFAULT_HW_ON_PERCENT/
// BATTERY_CHARGE_DEFAULT_EV_ON_PERCENT and the file is rewritten fresh with
// those defaults -- a partially-valid file is treated the same as a
// missing one, not patched field-by-field.
int hwSocOnPercent();
int hwSocOffPercent();  // hwSocOnPercent() - 5
int evSocOnPercent();
int evSocOffPercent();  // evSocOnPercent() - 5

// Called from the HW/EV Battery Charge% sub-menu's slider hooks
// (onHwOnAtChanged()/onEvChargingAtChanged(), overridden in
// SubMenu_HW_EV___Battery_CharApp.cpp) whenever a person moves a slider.
// Updates the in-RAM cache immediately (so hwSocOnPercent()/evSocOnPercent()
// reflect it right away) and, only if the value actually changed, restarts
// a 1s debounce timer instead of writing to LittleFS on the spot -- a
// slider drag sends many intermediate messages in quick succession, each
// with a different value, and a real physical flash write on every single
// one (added up over repeated slider use) is exactly the write-wear
// LoadControlState.h's own comment already flags as worth avoiding. See
// checkBatteryChargeSettingsSave() for the commit side of this. `percent`
// is clamped to the sliders' own 10-100 range before being stored, so an
// out-of-range value can never reach the persisted file.
void setHwSocOnPercent(int percent);
void setEvSocOnPercent(int percent);

// Hot Day settings -- same persistence/debounce/clamp shape as the two
// pairs above, saved into the SAME /battery_charge_settings.txt file, but
// validated as their OWN SEPARATE all-or-nothing group, not merged into
// hw_on/ev_on's -- see BatteryChargeSettings.cpp's own loadIfNeeded() for
// why: these three fields didn't exist when hw_on/ev_on did, so merging
// the groups would silently reset an existing device's already-valid
// hw_on/ev_on to their own defaults too, purely because these newly-added
// fields are (quite normally) absent from an older saved file. Added
// 2026-10-13, per explicit instruction, for the
// HW/EV - Battery Charge% sub-menu's new "Set Hot Day trigger
// Temperature..." controls. hotDayHwOnPercent()/hotDayCarOnPercent() are
// the HW/Car start% to switch to WHILE a hot day is active
// (HotDayState.h's hotDayActive()) -- NOT read directly by
// LoadControlTask.cpp; see HotDayState.h's effectiveHwSocOnPercent()/
// effectiveEvSocOnPercent() for the actual hotDayActive()-gated choice
// between these and hwSocOnPercent()/evSocOnPercent() above.
// tomorrowsMaxTempTriggerC() is the forecast max-temp threshold
// (HotDayState.h's readHotDayForecast() compares tomorrow's forecast
// against this) -- clamped to the sub-menu slider's own 20-40 deg C range.
int tomorrowsMaxTempTriggerC();
void setTomorrowsMaxTempTriggerC(int tempC);
int hotDayHwOnPercent();
int hotDayHwOffPercent();  // hotDayHwOnPercent() - 5
void setHotDayHwOnPercent(int percent);
int hotDayCarOnPercent();
int hotDayCarOffPercent();  // hotDayCarOnPercent() - 5
void setHotDayCarOnPercent(int percent);

// Call every loop() from the main .ino loop() (unconditionally, like every
// other pfodDelay-based "check" function in this project) -- commits
// whichever threshold(s) changed to LittleFS once the debounce timer
// started by setHwSocOnPercent()/setEvSocOnPercent() elapses with no
// further change in that window, rather than on every intermediate slider
// message. A no-op on every call where nothing is pending.
void checkBatteryChargeSettingsSave();

#endif
// ================= end of BatteryChargeSettings.h  file
