// Dwg_ConsumptionPlot3App.cpp  file =================
// Hand-written subclass of the generated Dwg_ConsumptionPlot3 -- the
// Consumption line's last third of the day (idx_consumption_48..67,
// points 48-68, see dwgs/ConsumptionPlot3.pfodDwg_json). Reads
// consumptionData (the model, filled by Dwg_ConsumptionPlot1App.cpp's
// MyConsumptionPlot1::init() -- see that file) and sends each segment as
// an indexed line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_ConsumptionPlot3.h/.cpp and leaves this file alone.
//
// No init() override needed here: this is an insertDwg'd child -- an
// inserted dwg's own dwgRefresh_ms is discarded on the wire
// (pfodAI-guide.md section 6, "Refresh is the trap"), and Dwg_Home's own
// dwgRefresh_ms (the one that would otherwise drive this) is 0 too, as
// of the 2026-10-08 Designer regeneration. This drawing still
// auto-refreshes every ~30s regardless, via a separate mechanism -- see
// Dwg_HomeApp.cpp's own comment (sendMainMenuUpdate()'s `|+` lines, tied
// to the main menu's own refresh_ms). Dwg_ConsumptionPlot3::init()
// is still called -- by Dwg_PowerChartGrid's own init(), via the
// get_dwg_ConsumptionPlot3() weak-factory chain -- it just doesn't need
// overriding here.
#include "Dwg_ConsumptionPlot3.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyConsumptionPlot3 : public Dwg_ConsumptionPlot3 {
  protected:
    virtual void sendIndexedItems();
};

// x positions are design-time fixed (12 + 2*globalPointIndex), matching
// dwgs/ConsumptionPlot3.pfodDwg_json exactly -- only each segment's y
// (from consumptionData's value) and colour come from the model.
// FIRST_POINT is where this file's points start within consumptionData's
// full-day array.
void MyConsumptionPlot3::sendIndexedItems() {
  const int FIRST_POINT = 48;
  const float X_START = 12.0 + 2.0 * FIRST_POINT;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[20] = {
    &idx_consumption_48, &idx_consumption_49, &idx_consumption_50, &idx_consumption_51,
    &idx_consumption_52, &idx_consumption_53, &idx_consumption_54, &idx_consumption_55,
    &idx_consumption_56, &idx_consumption_57, &idx_consumption_58, &idx_consumption_59,
    &idx_consumption_60, &idx_consumption_61, &idx_consumption_62, &idx_consumption_63,
    &idx_consumption_64, &idx_consumption_65, &idx_consumption_66, &idx_consumption_67,
  };
  for (int i = 0; i < 20; i++) {
    int p = FIRST_POINT + i;
    float v0 = consumptionData.points[p].value;
    float v1 = consumptionData.points[p + 1].value;
    if (v0 < 0.0 || v1 < 0.0) {
      continue; // no data yet for this segment -- leave its last-sent/placeholder state
    }
    float xOffset = X_START + X_STEP * i;
    float y0 = screenYForKw(v0);
    float y1 = screenYForKw(v1);
    dwgsPtr->line().idx(*idx[i]).color(consumptionData.points[p + 1].color)
        .size(X_STEP, y1 - y0).offset(xOffset, y0).send();
  }
}

MyConsumptionPlot3 myConsumptionPlot3;
Dwg_ConsumptionPlot3& get_dwg_ConsumptionPlot3() { return myConsumptionPlot3; }
// ============== end of Dwg_ConsumptionPlot3App.cpp  file
