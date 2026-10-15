// SubMenu_HW_EV___Battery_CharApp.cpp  file =================
// Hand-written subclass of the generated SubMenu_HW_EV___Battery_Char --
// same weak-accessor override pattern as pfodMainMenuApp.cpp (see that
// file's own top comment): "Re-generating from the pfodWeb Designer
// overwrites SubMenu_HW_EV___Battery_Char.h/.cpp and leaves this file
// alone." Added 2026-09-21, per explicit instruction, to back the two
// sliders ('HW On at' / 'EV Charging at') with BatteryChargeSettings.h's
// LittleFS-persisted turn-on thresholds instead of the Designer's
// placeholder in-RAM-only values. Extended 2026-10-13, per explicit
// instruction, for the sub-menu's three new "Hot Day" sliders
// ('Tomorrows Max' / 'Hot Day HW start' / 'Hot Day Car start'), same
// pattern, same BatteryChargeSettings.h persistence.
#include "SubMenu_HW_EV___Battery_Char.h"
#include "BatteryChargeSettings.h"

// pwm_HW_On_at_var/pwm_EV_Charging_at_var/pwm_Tomorrows_Max_var/
// pwm_Hot_Day_HW_star_var/pwm_Hot_Day_Car_sta_var (plain globals, external
// linkage, defined in the generated .cpp) are the "current value" ints
// sendMenu()/sendMenuUpdate() print for the five sliders -- not declared
// in the generated header, so declared extern here instead of editing
// that file. The generated handleCmd() writes the CLIENT's raw reported
// value into these itself, unclamped, before calling the matching
// onXxxChanged() hook below, and echoes back whatever these hold via
// sendMenuUpdate() right after that call returns -- so every hook below
// must write the CLAMPED value back into its own var too (not just
// persist it), or an out-of-range value the client sends (confirmed in
// practice for the first two sliders: a client sent a raw 0, below that
// slider's own configured 10 minimum, and the device echoed that same
// unclamped 0 straight back, even though setHwSocOnPercent() had already
// correctly clamped its OWN persisted copy to 10 -- a real, observed
// mismatch between what's displayed and what's actually stored) gets
// visibly corrected back to the client instead of silently diverging from
// what's actually persisted.
extern int pwm_HW_On_at_var;
extern int pwm_EV_Charging_at_var;
extern int pwm_Tomorrows_Max_var;
extern int pwm_Hot_Day_HW_star_var;
extern int pwm_Hot_Day_Car_sta_var;

class MySubMenu_HW_EV___Battery_Char : public SubMenu_HW_EV___Battery_Char {
  public:
    virtual void init();
  protected:
    virtual void onHwOnAtChanged(int value);
    virtual void onEvChargingAtChanged(int value);
    virtual void onTomorrowsMaxChanged(int value);
    virtual void onHotDayHwStartChanged(int value);
    virtual void onHotDayCarStartChanged(int value);
};

// Seeds all five sliders' starting values from the persisted settings
// BEFORE the base init() runs -- that primes this sub-menu's pfodAutoCmds
// by sending a full sendMenu() to a local discard sink (see the generated
// comment), which reads pwm_HW_On_at_var/pwm_EV_Charging_at_var/
// pwm_Tomorrows_Max_var/pwm_Hot_Day_HW_star_var/pwm_Hot_Day_Car_sta_var
// directly, so the very first real client to open this sub-menu sees the
// actual persisted settings instead of the Designer's placeholder values.
void MySubMenu_HW_EV___Battery_Char::init() {
  pwm_HW_On_at_var = hwSocOnPercent();
  pwm_EV_Charging_at_var = evSocOnPercent();
  pwm_Tomorrows_Max_var = tomorrowsMaxTempTriggerC();
  pwm_Hot_Day_HW_star_var = hotDayHwOnPercent();
  pwm_Hot_Day_Car_sta_var = hotDayCarOnPercent();
  SubMenu_HW_EV___Battery_Char::init();
}

void MySubMenu_HW_EV___Battery_Char::onHwOnAtChanged(int value) {
  setHwSocOnPercent(value);
  pwm_HW_On_at_var = hwSocOnPercent(); // reflect the clamped value back to the client, not the raw one it sent
}

void MySubMenu_HW_EV___Battery_Char::onEvChargingAtChanged(int value) {
  setEvSocOnPercent(value);
  pwm_EV_Charging_at_var = evSocOnPercent(); // reflect the clamped value back to the client, not the raw one it sent
}

void MySubMenu_HW_EV___Battery_Char::onTomorrowsMaxChanged(int value) {
  setTomorrowsMaxTempTriggerC(value);
  pwm_Tomorrows_Max_var = tomorrowsMaxTempTriggerC(); // reflect the clamped value back to the client, not the raw one it sent
}

void MySubMenu_HW_EV___Battery_Char::onHotDayHwStartChanged(int value) {
  setHotDayHwOnPercent(value);
  pwm_Hot_Day_HW_star_var = hotDayHwOnPercent(); // reflect the clamped value back to the client, not the raw one it sent
}

void MySubMenu_HW_EV___Battery_Char::onHotDayCarStartChanged(int value) {
  setHotDayCarOnPercent(value);
  pwm_Hot_Day_Car_sta_var = hotDayCarOnPercent(); // reflect the clamped value back to the client, not the raw one it sent
}

MySubMenu_HW_EV___Battery_Char mySubMenu_HW_EV___Battery_Char;
SubMenu_HW_EV___Battery_Char& get_subMenu_HW_EV___Battery_Char() { return mySubMenu_HW_EV___Battery_Char; } // replaces the weak default sketch-wide
// ============== end of SubMenu_HW_EV___Battery_CharApp.cpp  file
