// Dwg_SolarPlot2App.cpp  file =================
// Hand-written subclass of the generated Dwg_SolarPlot2 -- the Solar
// line's middle third of the day (idx_solar_24..47, points 24-48, see
// dwgs/SolarPlot2.pfodDwg_json). Reads solarData (the model, filled by
// Dwg_SolarPlot1App.cpp's MySolarPlot1::init(), which always runs first --
// see pfodAI-guide.md section 6.6, init() chains happen at startup, before
// any sendIndexedItems() can be triggered) and sends each segment as an
// indexed line.
// Re-generating from the pfodWeb Designer overwrites Dwg_SolarPlot2.h/.cpp
// and leaves this file alone.
//
// No init() override needed here: this is an insertDwg'd child -- an
// inserted dwg's own dwgRefresh_ms is discarded on the wire
// (pfodAI-guide.md section 6, "Refresh is the trap"; also
// pfodDwg_json-format.md's insertDwg section), and Dwg_Home's own
// dwgRefresh_ms (the one that would otherwise drive this) is 0 too, as
// of the 2026-10-08 Designer regeneration. This drawing still
// auto-refreshes every ~30s regardless, via a separate mechanism -- see
// Dwg_HomeApp.cpp's own comment (sendMainMenuUpdate()'s `|+` lines, tied
// to the main menu's own refresh_ms). Dwg_SolarPlot2::init() is
// still called -- by Dwg_PowerChartGrid's own init(), via the
// get_dwg_SolarPlot2() weak-factory chain -- it just doesn't need
// overriding here.
#include "Dwg_SolarPlot2.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MySolarPlot2 : public Dwg_SolarPlot2 {
  protected:
    virtual void sendIndexedItems();
};

// x positions are design-time fixed (12 + 2*globalPointIndex), matching
// dwgs/SolarPlot2.pfodDwg_json exactly -- only each segment's y (from
// solarData's value) and colour come from the model. FIRST_POINT is where
// this file's points start within solarData's full-day array.
void MySolarPlot2::sendIndexedItems() {
  const int FIRST_POINT = 24;
  const float X_START = 12.0 + 2.0 * FIRST_POINT;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[24] = {
    &idx_solar_24, &idx_solar_25, &idx_solar_26, &idx_solar_27,
    &idx_solar_28, &idx_solar_29, &idx_solar_30, &idx_solar_31,
    &idx_solar_32, &idx_solar_33, &idx_solar_34, &idx_solar_35,
    &idx_solar_36, &idx_solar_37, &idx_solar_38, &idx_solar_39,
    &idx_solar_40, &idx_solar_41, &idx_solar_42, &idx_solar_43,
    &idx_solar_44, &idx_solar_45, &idx_solar_46, &idx_solar_47,
  };
  for (int i = 0; i < 24; i++) {
    int p = FIRST_POINT + i;
    float v0 = solarData.points[p].value;
    float v1 = solarData.points[p + 1].value;
    if (v0 < 0.0 || v1 < 0.0) {
      continue; // no data yet for this segment -- leave its last-sent/placeholder state
    }
    float xOffset = X_START + X_STEP * i;
    float y0 = screenYForKw(v0);
    float y1 = screenYForKw(v1);
    dwgsPtr->line().idx(*idx[i]).color(solarData.points[p + 1].color)
        .size(X_STEP, y1 - y0).offset(xOffset, y0).send();
  }
}

MySolarPlot2 mySolarPlot2;
Dwg_SolarPlot2& get_dwg_SolarPlot2() { return mySolarPlot2; }
// ============== end of Dwg_SolarPlot2App.cpp  file
