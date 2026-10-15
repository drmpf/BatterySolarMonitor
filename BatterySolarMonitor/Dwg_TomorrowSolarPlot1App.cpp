// Dwg_TomorrowSolarPlot1App.cpp  file =================
// Hand-written subclass of the generated Dwg_TomorrowSolarPlot1 -- see
// pfodAI-guide.md section 6, "The shape of the logic - model, view,
// controller". Controller's transfer half for the Tomorrow chart's Solar
// line, first third of the day (idx_tomorrowSolar_0..23, points 0-24, see
// dwgs/TomorrowSolarPlot1.pfodDwg_json -- a copy of SolarPlot1.pfodDwg_json
// with its own name/idxNames, made so the Tomorrow chart has independent
// drawings instead of sharing SolarPlot1-3 with PowerChartGrid/today's
// chart). Reads tomorrowSolarData (the model, PlotData.h/
// PlotDataInstances.h) and sends each segment as an indexed line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_TomorrowSolarPlot1.h/.cpp and leaves this file alone.
#include "Dwg_TomorrowSolarPlot1.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyTomorrowSolarPlot1 : public Dwg_TomorrowSolarPlot1 {
  public:
    virtual void init();
  protected:
    virtual void sendIndexedItems();
  private:
    bool dummyDataFilled = false;
};

// Chains to the base first, then -- once only -- clears tomorrowSolarData
// to PlotData::clear()'s -1 "no data yet" sentinel, so this chart draws
// empty/off-scale instead of a fabricated demo curve until
// SolarForecastRefresh.cpp's first successful fetch fills it with a real
// Open-Meteo forecast (which it does for the WHOLE day at once, no
// "actual" boundary to worry about -- see mapSolarForecastToPlotData()'s
// own default skipBeforeMinutes). Fills the whole day so TomorrowSolarPlot2/3
// start from the same clean state.
void MyTomorrowSolarPlot1::init() {
  Dwg_TomorrowSolarPlot1::init(); // always chain to the base first
  // No dwgRefresh_ms here: as an insertDwg'd child, it would be discarded
  // on the wire and do nothing anyway -- and the top-level dwg it's
  // inserted into (Dwg_TomorrowChart)'s own dwgRefresh_ms is 0 too, as
  // of the 2026-10-08 Designer regeneration. This drawing still
  // auto-refreshes every ~30s regardless, via a separate mechanism -- see
  // Dwg_TomorrowChartApp.cpp's own comment (sendMainMenuUpdate()'s `|+`
  // lines, tied to the main menu's own refresh_ms). See pfodAI-guide.md
  // section 6, "Refresh is the trap", for the dwgRefresh_ms mechanism
  // itself.
  if (dummyDataFilled) {
    return;
  }
  dummyDataFilled = true;

  tomorrowSolarData.clear();
}

// x positions are design-time fixed (12 + 2*segment), matching
// dwgs/TomorrowSolarPlot1.pfodDwg_json exactly -- only each segment's y
// (from tomorrowSolarData's value) and colour come from the model.
void MyTomorrowSolarPlot1::sendIndexedItems() {
  const float X_START = 12.0;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[24] = {
    &idx_tomorrowSolar_0,  &idx_tomorrowSolar_1,  &idx_tomorrowSolar_2,  &idx_tomorrowSolar_3,
    &idx_tomorrowSolar_4,  &idx_tomorrowSolar_5,  &idx_tomorrowSolar_6,  &idx_tomorrowSolar_7,
    &idx_tomorrowSolar_8,  &idx_tomorrowSolar_9,  &idx_tomorrowSolar_10, &idx_tomorrowSolar_11,
    &idx_tomorrowSolar_12, &idx_tomorrowSolar_13, &idx_tomorrowSolar_14, &idx_tomorrowSolar_15,
    &idx_tomorrowSolar_16, &idx_tomorrowSolar_17, &idx_tomorrowSolar_18, &idx_tomorrowSolar_19,
    &idx_tomorrowSolar_20, &idx_tomorrowSolar_21, &idx_tomorrowSolar_22, &idx_tomorrowSolar_23,
  };
  for (int i = 0; i < 24; i++) {
    float v0 = tomorrowSolarData.points[i].value;
    float v1 = tomorrowSolarData.points[i + 1].value;
    if (v0 < 0.0 || v1 < 0.0) {
      continue; // no data yet for this segment -- leave its last-sent/placeholder state
    }
    float xOffset = X_START + X_STEP * i;
    float y0 = screenYForKw(v0);
    float y1 = screenYForKw(v1);
    dwgsPtr->line().idx(*idx[i]).color(tomorrowSolarData.points[i].color)
        .size(X_STEP, y1 - y0).offset(xOffset, y0).send();
  }
}

MyTomorrowSolarPlot1 myTomorrowSolarPlot1;
Dwg_TomorrowSolarPlot1& get_dwg_TomorrowSolarPlot1() { return myTomorrowSolarPlot1; }
// ============== end of Dwg_TomorrowSolarPlot1App.cpp  file
