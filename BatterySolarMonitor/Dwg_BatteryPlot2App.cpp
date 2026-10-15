// Dwg_BatteryPlot2App.cpp  file =================
// Hand-written subclass of the generated Dwg_BatteryPlot2 -- the Battery
// Charge line's middle third of the day (idx_battery_24..47, points
// 24-48, see dwgs/BatteryPlot2.pfodDwg_json). Reads batteryChargeData
// (the model, filled by Dwg_BatteryPlot1App.cpp's MyBatteryPlot1::init(),
// which always runs first -- see that file) and sends each segment as an
// indexed line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_BatteryPlot2.h/.cpp and leaves this file alone.
//
// No init() override needed here: this is an insertDwg'd child, and only
// the top-level dwg (Dwg_Home, via Dwg_HomeApp.cpp) actually drives an
// auto-refresh.
#include "Dwg_BatteryPlot2.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyBatteryPlot2 : public Dwg_BatteryPlot2 {
  protected:
    virtual void sendIndexedItems();
};

// x positions are design-time fixed (12 + 2*globalPointIndex), matching
// dwgs/BatteryPlot2.pfodDwg_json exactly -- only each segment's y (from
// batteryChargeData's value) and colour come from the model. FIRST_POINT
// is where this file's points start within batteryChargeData's full-day
// array.
void MyBatteryPlot2::sendIndexedItems() {
  const int FIRST_POINT = 24;
  const float X_START = 12.0 + 2.0 * FIRST_POINT;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[24] = {
    &idx_battery_24, &idx_battery_25, &idx_battery_26, &idx_battery_27,
    &idx_battery_28, &idx_battery_29, &idx_battery_30, &idx_battery_31,
    &idx_battery_32, &idx_battery_33, &idx_battery_34, &idx_battery_35,
    &idx_battery_36, &idx_battery_37, &idx_battery_38, &idx_battery_39,
    &idx_battery_40, &idx_battery_41, &idx_battery_42, &idx_battery_43,
    &idx_battery_44, &idx_battery_45, &idx_battery_46, &idx_battery_47,
  };
  for (int i = 0; i < 24; i++) {
    int p = FIRST_POINT + i;
    float v0 = batteryChargeData.points[p].value;
    float v1 = batteryChargeData.points[p + 1].value;
    if (v0 < 0.0 || v1 < 0.0) {
      continue; // no data yet for this segment -- leave its last-sent/placeholder state
    }
    float xOffset = X_START + X_STEP * i;
    float y0 = screenYForPercent(v0);
    float y1 = screenYForPercent(v1);
    dwgsPtr->line().idx(*idx[i]).color(batteryChargeData.points[p + 1].color)
        .size(X_STEP, y1 - y0).offset(xOffset, y0).send();
  }
}

MyBatteryPlot2 myBatteryPlot2;
Dwg_BatteryPlot2& get_dwg_BatteryPlot2() { return myBatteryPlot2; }
// ============== end of Dwg_BatteryPlot2App.cpp  file
