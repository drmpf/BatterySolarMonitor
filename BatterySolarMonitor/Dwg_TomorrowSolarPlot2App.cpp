// Dwg_TomorrowSolarPlot2App.cpp  file =================
// Hand-written subclass of the generated Dwg_TomorrowSolarPlot2 -- the
// Tomorrow chart's Solar line, middle third of the day
// (idx_tomorrowSolar_24..47, points 24-48, see
// dwgs/TomorrowSolarPlot2.pfodDwg_json). Reads tomorrowSolarData (the
// model, filled by Dwg_TomorrowSolarPlot1App.cpp's
// MyTomorrowSolarPlot1::init(), which always runs first -- see that file)
// and sends each segment as an indexed line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_TomorrowSolarPlot2.h/.cpp and leaves this file alone.
//
// No init() override needed here: this is an insertDwg'd child -- an
// inserted dwg's own dwgRefresh_ms is discarded on the wire
// (pfodAI-guide.md section 6, "Refresh is the trap"), and its top-level
// dwg (Dwg_TomorrowChart)'s own dwgRefresh_ms is 0 too, as of the
// 2026-10-08 Designer regeneration. This drawing still auto-refreshes
// every ~30s regardless, via a separate mechanism -- see
// Dwg_TomorrowChartApp.cpp's own comment (sendMainMenuUpdate()'s `|+`
// lines, tied to the main menu's own refresh_ms).
#include "Dwg_TomorrowSolarPlot2.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyTomorrowSolarPlot2 : public Dwg_TomorrowSolarPlot2 {
  protected:
    virtual void sendIndexedItems();
};

// x positions are design-time fixed (12 + 2*globalPointIndex), matching
// dwgs/TomorrowSolarPlot2.pfodDwg_json exactly -- only each segment's y
// (from tomorrowSolarData's value) and colour come from the model.
// FIRST_POINT is where this file's points start within tomorrowSolarData's
// full-day array.
void MyTomorrowSolarPlot2::sendIndexedItems() {
  const int FIRST_POINT = 24;
  const float X_START = 12.0 + 2.0 * FIRST_POINT;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[24] = {
    &idx_tomorrowSolar_24, &idx_tomorrowSolar_25, &idx_tomorrowSolar_26, &idx_tomorrowSolar_27,
    &idx_tomorrowSolar_28, &idx_tomorrowSolar_29, &idx_tomorrowSolar_30, &idx_tomorrowSolar_31,
    &idx_tomorrowSolar_32, &idx_tomorrowSolar_33, &idx_tomorrowSolar_34, &idx_tomorrowSolar_35,
    &idx_tomorrowSolar_36, &idx_tomorrowSolar_37, &idx_tomorrowSolar_38, &idx_tomorrowSolar_39,
    &idx_tomorrowSolar_40, &idx_tomorrowSolar_41, &idx_tomorrowSolar_42, &idx_tomorrowSolar_43,
    &idx_tomorrowSolar_44, &idx_tomorrowSolar_45, &idx_tomorrowSolar_46, &idx_tomorrowSolar_47,
  };
  for (int i = 0; i < 24; i++) {
    int p = FIRST_POINT + i;
    float v0 = tomorrowSolarData.points[p].value;
    float v1 = tomorrowSolarData.points[p + 1].value;
    if (v0 < 0.0 || v1 < 0.0) {
      continue; // no data yet for this segment -- leave its last-sent/placeholder state
    }
    float xOffset = X_START + X_STEP * i;
    float y0 = screenYForKw(v0);
    float y1 = screenYForKw(v1);
    dwgsPtr->line().idx(*idx[i]).color(tomorrowSolarData.points[p].color)
        .size(X_STEP, y1 - y0).offset(xOffset, y0).send();
  }
}

MyTomorrowSolarPlot2 myTomorrowSolarPlot2;
Dwg_TomorrowSolarPlot2& get_dwg_TomorrowSolarPlot2() { return myTomorrowSolarPlot2; }
// ============== end of Dwg_TomorrowSolarPlot2App.cpp  file
