// Dwg_TomorrowSolarPlot3App.cpp  file =================
// Hand-written subclass of the generated Dwg_TomorrowSolarPlot3 -- the
// Tomorrow chart's Solar line, last third of the day
// (idx_tomorrowSolar_48..67, points 48-68, see
// dwgs/TomorrowSolarPlot3.pfodDwg_json). Reads tomorrowSolarData (the
// model, filled by Dwg_TomorrowSolarPlot1App.cpp's
// MyTomorrowSolarPlot1::init() -- see that file) and sends each segment
// as an indexed line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_TomorrowSolarPlot3.h/.cpp and leaves this file alone.
//
// No init() override needed here: this is an insertDwg'd child -- an
// inserted dwg's own dwgRefresh_ms is discarded on the wire
// (pfodAI-guide.md section 6, "Refresh is the trap"), and its top-level
// dwg (Dwg_TomorrowChart)'s own dwgRefresh_ms is 0 too, as of the
// 2026-10-08 Designer regeneration. This drawing still auto-refreshes
// every ~30s regardless, via a separate mechanism -- see
// Dwg_TomorrowChartApp.cpp's own comment (sendMainMenuUpdate()'s `|+`
// lines, tied to the main menu's own refresh_ms).
#include "Dwg_TomorrowSolarPlot3.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyTomorrowSolarPlot3 : public Dwg_TomorrowSolarPlot3 {
  protected:
    virtual void sendIndexedItems();
};

// x positions are design-time fixed (12 + 2*globalPointIndex), matching
// dwgs/TomorrowSolarPlot3.pfodDwg_json exactly -- only each segment's y
// (from tomorrowSolarData's value) and colour come from the model.
// FIRST_POINT is where this file's points start within tomorrowSolarData's
// full-day array.
void MyTomorrowSolarPlot3::sendIndexedItems() {
  const int FIRST_POINT = 48;
  const float X_START = 12.0 + 2.0 * FIRST_POINT;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[20] = {
    &idx_tomorrowSolar_48, &idx_tomorrowSolar_49, &idx_tomorrowSolar_50, &idx_tomorrowSolar_51,
    &idx_tomorrowSolar_52, &idx_tomorrowSolar_53, &idx_tomorrowSolar_54, &idx_tomorrowSolar_55,
    &idx_tomorrowSolar_56, &idx_tomorrowSolar_57, &idx_tomorrowSolar_58, &idx_tomorrowSolar_59,
    &idx_tomorrowSolar_60, &idx_tomorrowSolar_61, &idx_tomorrowSolar_62, &idx_tomorrowSolar_63,
    &idx_tomorrowSolar_64, &idx_tomorrowSolar_65, &idx_tomorrowSolar_66, &idx_tomorrowSolar_67,
  };
  for (int i = 0; i < 20; i++) {
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

MyTomorrowSolarPlot3 myTomorrowSolarPlot3;
Dwg_TomorrowSolarPlot3& get_dwg_TomorrowSolarPlot3() { return myTomorrowSolarPlot3; }
// ============== end of Dwg_TomorrowSolarPlot3App.cpp  file
