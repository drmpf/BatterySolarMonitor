// Dwg_SolarPlot3App.cpp  file =================
// Hand-written subclass of the generated Dwg_SolarPlot3 -- the Solar
// line's last third of the day (idx_solar_48..67, points 48-68, see
// dwgs/SolarPlot3.pfodDwg_json). Reads solarData (the model, filled by
// Dwg_SolarPlot1App.cpp's MySolarPlot1::init() -- see that file) and sends
// each segment as an indexed line.
// Re-generating from the pfodWeb Designer overwrites Dwg_SolarPlot3.h/.cpp
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
// to the main menu's own refresh_ms). Dwg_SolarPlot3::init() is
// still called -- by Dwg_PowerChartGrid's own init(), via the
// get_dwg_SolarPlot3() weak-factory chain -- it just doesn't need
// overriding here.
#include "Dwg_SolarPlot3.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MySolarPlot3 : public Dwg_SolarPlot3 {
  protected:
    virtual void sendIndexedItems();
};

// x positions are design-time fixed (12 + 2*globalPointIndex), matching
// dwgs/SolarPlot3.pfodDwg_json exactly -- only each segment's y (from
// solarData's value) and colour come from the model. FIRST_POINT is where
// this file's points start within solarData's full-day array.
void MySolarPlot3::sendIndexedItems() {
  const int FIRST_POINT = 48;
  const float X_START = 12.0 + 2.0 * FIRST_POINT;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[20] = {
    &idx_solar_48, &idx_solar_49, &idx_solar_50, &idx_solar_51,
    &idx_solar_52, &idx_solar_53, &idx_solar_54, &idx_solar_55,
    &idx_solar_56, &idx_solar_57, &idx_solar_58, &idx_solar_59,
    &idx_solar_60, &idx_solar_61, &idx_solar_62, &idx_solar_63,
    &idx_solar_64, &idx_solar_65, &idx_solar_66, &idx_solar_67,
  };
  for (int i = 0; i < 20; i++) {
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

MySolarPlot3 mySolarPlot3;
Dwg_SolarPlot3& get_dwg_SolarPlot3() { return mySolarPlot3; }
// ============== end of Dwg_SolarPlot3App.cpp  file
