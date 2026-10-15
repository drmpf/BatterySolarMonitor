// Dwg_BatteryPlot1App.cpp  file =================
// Hand-written subclass of the generated Dwg_BatteryPlot1 -- see
// pfodAI-guide.md section 6, "The shape of the logic - model, view,
// controller". Controller's transfer half for the Battery Charge line's
// first third of the day (idx_battery_0..23, points 0-24, see
// dwgs/BatteryPlot1.pfodDwg_json): reads batteryChargeData (the model,
// PlotData.h/PlotDataInstances.h) and sends each segment as an indexed
// line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_BatteryPlot1.h/.cpp and leaves this file alone.
#include "Dwg_BatteryPlot1.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyBatteryPlot1 : public Dwg_BatteryPlot1 {
  public:
    virtual void init();
  protected:
    virtual void sendIndexedItems();
  private:
    bool dummyDataFilled = false;
};

// Chains to the base first, then -- once only -- clears batteryChargeData
// to PlotData::clear()'s -1 "no data yet" sentinel, so this chart draws
// empty/off-scale instead of a fabricated demo curve until a real feed
// (GatewayRefresh.cpp's live point, BatteryProjection.cpp's projection,
// or EnlightenHistoryRefresh.cpp's merge) actually replaces it. An
// earlier version of this file filled a plausible-looking dummy curve
// instead (a linear rise from 20% to 100%) -- fabricated data that looked
// real on the chart, and (since valueAtTime() can't tell a genuine
// reading from a plausible-looking positive dummy one) could even have
// been mistaken by BatteryProjection.cpp's own "do we have a starting
// point yet" check (soc <= 0.0) for a real seed to project forward from.
// -1 fixes that too: it's always <= 0.0, so that check now only ever
// passes once a genuine reading lands. Clears the whole day so
// BatteryPlot2/3 start from the same clean state.
void MyBatteryPlot1::init() {
  Dwg_BatteryPlot1::init(); // always chain to the base first
  // No dwgRefresh_ms here: as an insertDwg'd child, it would be discarded
  // on the wire and do nothing anyway -- and Dwg_Home's own dwgRefresh_ms
  // (the one that would otherwise drive this) is 0 too, as of the
  // 2026-10-08 Designer regeneration. This drawing still auto-refreshes
  // every ~30s regardless, via a separate mechanism -- see
  // Dwg_HomeApp.cpp's own comment (sendMainMenuUpdate()'s `|+` lines,
  // tied to the main menu's own refresh_ms). See pfodAI-guide.md section
  // 6, "Refresh is the trap", for the dwgRefresh_ms mechanism itself.
  if (dummyDataFilled) {
    return;
  }
  dummyDataFilled = true;

  batteryChargeData.clear();
}

// x positions are design-time fixed (12 + 2*segment), matching
// dwgs/BatteryPlot1.pfodDwg_json exactly -- only each segment's y (from
// batteryChargeData's value) and colour come from the model.
void MyBatteryPlot1::sendIndexedItems() {
  const float X_START = 12.0;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[24] = {
    &idx_battery_0,  &idx_battery_1,  &idx_battery_2,  &idx_battery_3,
    &idx_battery_4,  &idx_battery_5,  &idx_battery_6,  &idx_battery_7,
    &idx_battery_8,  &idx_battery_9,  &idx_battery_10, &idx_battery_11,
    &idx_battery_12, &idx_battery_13, &idx_battery_14, &idx_battery_15,
    &idx_battery_16, &idx_battery_17, &idx_battery_18, &idx_battery_19,
    &idx_battery_20, &idx_battery_21, &idx_battery_22, &idx_battery_23,
  };
  for (int i = 0; i < 24; i++) {
    float v0 = batteryChargeData.points[i].value;
    float v1 = batteryChargeData.points[i + 1].value;
    if (v0 < 0.0 || v1 < 0.0) {
      continue; // no data yet for this segment -- leave its last-sent/placeholder state
    }
    float xOffset = X_START + X_STEP * i;
    float y0 = screenYForPercent(v0);
    float y1 = screenYForPercent(v1);
    dwgsPtr->line().idx(*idx[i]).color(batteryChargeData.points[i + 1].color)
        .size(X_STEP, y1 - y0).offset(xOffset, y0).send();
  }
}

MyBatteryPlot1 myBatteryPlot1;
Dwg_BatteryPlot1& get_dwg_BatteryPlot1() { return myBatteryPlot1; }
// ============== end of Dwg_BatteryPlot1App.cpp  file
