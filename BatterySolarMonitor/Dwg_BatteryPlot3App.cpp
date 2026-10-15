// Dwg_BatteryPlot3App.cpp  file =================
// Hand-written subclass of the generated Dwg_BatteryPlot3 -- the Battery
// Charge line's last third of the day (idx_battery_48..67, points 48-68,
// see dwgs/BatteryPlot3.pfodDwg_json). Reads batteryChargeData (the
// model, filled by Dwg_BatteryPlot1App.cpp's MyBatteryPlot1::init() --
// see that file) and sends each segment as an indexed line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_BatteryPlot3.h/.cpp and leaves this file alone.
//
// No init() override needed here: this is an insertDwg'd child, and only
// the top-level dwg (Dwg_Home, via Dwg_HomeApp.cpp) actually drives an
// auto-refresh .
#include "Dwg_BatteryPlot3.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyBatteryPlot3 : public Dwg_BatteryPlot3 {
  protected:
    virtual void sendIndexedItems();
};

// x positions are design-time fixed (12 + 2*globalPointIndex), matching
// dwgs/BatteryPlot3.pfodDwg_json exactly -- only each segment's y (from
// batteryChargeData's value) and colour come from the model. FIRST_POINT
// is where this file's points start within batteryChargeData's full-day
// array.
void MyBatteryPlot3::sendIndexedItems() {
  const int FIRST_POINT = 48;
  const float X_START = 12.0 + 2.0 * FIRST_POINT;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[20] = {
    &idx_battery_48, &idx_battery_49, &idx_battery_50, &idx_battery_51,
    &idx_battery_52, &idx_battery_53, &idx_battery_54, &idx_battery_55,
    &idx_battery_56, &idx_battery_57, &idx_battery_58, &idx_battery_59,
    &idx_battery_60, &idx_battery_61, &idx_battery_62, &idx_battery_63,
    &idx_battery_64, &idx_battery_65, &idx_battery_66, &idx_battery_67,
  };
  for (int i = 0; i < 20; i++) {
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

MyBatteryPlot3 myBatteryPlot3;
Dwg_BatteryPlot3& get_dwg_BatteryPlot3() { return myBatteryPlot3; }
// ============== end of Dwg_BatteryPlot3App.cpp  file
