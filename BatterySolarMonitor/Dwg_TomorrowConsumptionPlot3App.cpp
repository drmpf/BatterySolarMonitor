// Dwg_TomorrowConsumptionPlot3App.cpp  file =================
// Hand-written subclass of the generated Dwg_TomorrowConsumptionPlot3 --
// the Tomorrow chart's Avg Consumption line, last third of the day
// (idx_tomorrowConsumption_48..67, points 48-68, see
// dwgs/TomorrowConsumptionPlot3.pfodDwg_json). Reads
// tomorrowAvgConsumptionData (the model, filled by
// Dwg_TomorrowConsumptionPlot1App.cpp's
// MyTomorrowConsumptionPlot1::init() -- see that file) and sends each
// segment as an indexed line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_TomorrowConsumptionPlot3.h/.cpp and leaves this file alone.
#include "Dwg_TomorrowConsumptionPlot3.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyTomorrowConsumptionPlot3 : public Dwg_TomorrowConsumptionPlot3 {
  protected:
    virtual void sendIndexedItems();
};

// x positions are design-time fixed (12 + 2*globalPointIndex), matching
// dwgs/TomorrowConsumptionPlot3.pfodDwg_json exactly -- only each
// segment's y (from tomorrowAvgConsumptionData's value) and colour come
// from the model. FIRST_POINT is where this file's points start within
// tomorrowAvgConsumptionData's full-day array.
void MyTomorrowConsumptionPlot3::sendIndexedItems() {
  const int FIRST_POINT = 48;
  const float X_START = 12.0 + 2.0 * FIRST_POINT;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[20] = {
    &idx_tomorrowConsumption_48, &idx_tomorrowConsumption_49, &idx_tomorrowConsumption_50, &idx_tomorrowConsumption_51,
    &idx_tomorrowConsumption_52, &idx_tomorrowConsumption_53, &idx_tomorrowConsumption_54, &idx_tomorrowConsumption_55,
    &idx_tomorrowConsumption_56, &idx_tomorrowConsumption_57, &idx_tomorrowConsumption_58, &idx_tomorrowConsumption_59,
    &idx_tomorrowConsumption_60, &idx_tomorrowConsumption_61, &idx_tomorrowConsumption_62, &idx_tomorrowConsumption_63,
    &idx_tomorrowConsumption_64, &idx_tomorrowConsumption_65, &idx_tomorrowConsumption_66, &idx_tomorrowConsumption_67,
  };
  for (int i = 0; i < 20; i++) {
    int p = FIRST_POINT + i;
    float v0 = tomorrowAvgConsumptionData.points[p].value;
    float v1 = tomorrowAvgConsumptionData.points[p + 1].value;
    if (v0 < 0.0 || v1 < 0.0) {
      continue; // no data yet for this segment -- leave its last-sent/placeholder state
    }
    float xOffset = X_START + X_STEP * i;
    float y0 = screenYForKw(v0);
    float y1 = screenYForKw(v1);
    dwgsPtr->line().idx(*idx[i]).color(tomorrowAvgConsumptionData.points[p].color)
        .size(X_STEP, y1 - y0).offset(xOffset, y0).send();
  }
}

MyTomorrowConsumptionPlot3 myTomorrowConsumptionPlot3;
Dwg_TomorrowConsumptionPlot3& get_dwg_TomorrowConsumptionPlot3() { return myTomorrowConsumptionPlot3; }
// ============== end of Dwg_TomorrowConsumptionPlot3App.cpp  file
