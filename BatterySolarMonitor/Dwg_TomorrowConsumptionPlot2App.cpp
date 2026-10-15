// Dwg_TomorrowConsumptionPlot2App.cpp  file =================
// Hand-written subclass of the generated Dwg_TomorrowConsumptionPlot2 --
// the Tomorrow chart's Avg Consumption line, middle third of the day
// (idx_tomorrowConsumption_24..47, points 24-48, see
// dwgs/TomorrowConsumptionPlot2.pfodDwg_json). Reads
// tomorrowAvgConsumptionData (the model, filled by
// Dwg_TomorrowConsumptionPlot1App.cpp's
// MyTomorrowConsumptionPlot1::init(), which always runs first -- see that
// file) and sends each segment as an indexed line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_TomorrowConsumptionPlot2.h/.cpp and leaves this file alone.
#include "Dwg_TomorrowConsumptionPlot2.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyTomorrowConsumptionPlot2 : public Dwg_TomorrowConsumptionPlot2 {
  protected:
    virtual void sendIndexedItems();
};

// x positions are design-time fixed (12 + 2*globalPointIndex), matching
// dwgs/TomorrowConsumptionPlot2.pfodDwg_json exactly -- only each
// segment's y (from tomorrowAvgConsumptionData's value) and colour come
// from the model. FIRST_POINT is where this file's points start within
// tomorrowAvgConsumptionData's full-day array.
void MyTomorrowConsumptionPlot2::sendIndexedItems() {
  const int FIRST_POINT = 24;
  const float X_START = 12.0 + 2.0 * FIRST_POINT;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[24] = {
    &idx_tomorrowConsumption_24, &idx_tomorrowConsumption_25, &idx_tomorrowConsumption_26, &idx_tomorrowConsumption_27,
    &idx_tomorrowConsumption_28, &idx_tomorrowConsumption_29, &idx_tomorrowConsumption_30, &idx_tomorrowConsumption_31,
    &idx_tomorrowConsumption_32, &idx_tomorrowConsumption_33, &idx_tomorrowConsumption_34, &idx_tomorrowConsumption_35,
    &idx_tomorrowConsumption_36, &idx_tomorrowConsumption_37, &idx_tomorrowConsumption_38, &idx_tomorrowConsumption_39,
    &idx_tomorrowConsumption_40, &idx_tomorrowConsumption_41, &idx_tomorrowConsumption_42, &idx_tomorrowConsumption_43,
    &idx_tomorrowConsumption_44, &idx_tomorrowConsumption_45, &idx_tomorrowConsumption_46, &idx_tomorrowConsumption_47,
  };
  for (int i = 0; i < 24; i++) {
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

MyTomorrowConsumptionPlot2 myTomorrowConsumptionPlot2;
Dwg_TomorrowConsumptionPlot2& get_dwg_TomorrowConsumptionPlot2() { return myTomorrowConsumptionPlot2; }
// ============== end of Dwg_TomorrowConsumptionPlot2App.cpp  file
