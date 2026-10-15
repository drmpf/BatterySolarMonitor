// Dwg_ConsumptionPlot2App.cpp  file =================
// Hand-written subclass of the generated Dwg_ConsumptionPlot2 -- the
// Consumption line's middle third of the day (idx_consumption_24..47,
// points 24-48, see dwgs/ConsumptionPlot2.pfodDwg_json). Reads
// consumptionData (the model, filled by Dwg_ConsumptionPlot1App.cpp's
// MyConsumptionPlot1::init(), which always runs first -- see that file)
// and sends each segment as an indexed line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_ConsumptionPlot2.h/.cpp and leaves this file alone.
//
// No init() override needed here: this is an insertDwg'd child -- an
// inserted dwg's own dwgRefresh_ms is discarded on the wire
// (pfodAI-guide.md section 6, "Refresh is the trap"), and Dwg_Home's own
// dwgRefresh_ms (the one that would otherwise drive this) is 0 too, as
// of the 2026-10-08 Designer regeneration. This drawing still
// auto-refreshes every ~30s regardless, via a separate mechanism -- see
// Dwg_HomeApp.cpp's own comment (sendMainMenuUpdate()'s `|+` lines, tied
// to the main menu's own refresh_ms). Dwg_ConsumptionPlot2::init()
// is still called -- by Dwg_PowerChartGrid's own init(), via the
// get_dwg_ConsumptionPlot2() weak-factory chain -- it just doesn't need
// overriding here.
#include "Dwg_ConsumptionPlot2.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyConsumptionPlot2 : public Dwg_ConsumptionPlot2 {
  protected:
    virtual void sendIndexedItems();
};

// x positions are design-time fixed (12 + 2*globalPointIndex), matching
// dwgs/ConsumptionPlot2.pfodDwg_json exactly -- only each segment's y
// (from consumptionData's value) and colour come from the model.
// FIRST_POINT is where this file's points start within consumptionData's
// full-day array.
void MyConsumptionPlot2::sendIndexedItems() {
  const int FIRST_POINT = 24;
  const float X_START = 12.0 + 2.0 * FIRST_POINT;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[24] = {
    &idx_consumption_24, &idx_consumption_25, &idx_consumption_26, &idx_consumption_27,
    &idx_consumption_28, &idx_consumption_29, &idx_consumption_30, &idx_consumption_31,
    &idx_consumption_32, &idx_consumption_33, &idx_consumption_34, &idx_consumption_35,
    &idx_consumption_36, &idx_consumption_37, &idx_consumption_38, &idx_consumption_39,
    &idx_consumption_40, &idx_consumption_41, &idx_consumption_42, &idx_consumption_43,
    &idx_consumption_44, &idx_consumption_45, &idx_consumption_46, &idx_consumption_47,
  };
  for (int i = 0; i < 24; i++) {
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

MyConsumptionPlot2 myConsumptionPlot2;
Dwg_ConsumptionPlot2& get_dwg_ConsumptionPlot2() { return myConsumptionPlot2; }
// ============== end of Dwg_ConsumptionPlot2App.cpp  file
