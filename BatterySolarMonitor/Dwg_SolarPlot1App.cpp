// Dwg_SolarPlot1App.cpp  file =================
// Hand-written subclass of the generated Dwg_SolarPlot1 -- see
// pfodAI-guide.md section 6, "The shape of the logic - model, view,
// controller". This is the controller's transfer half for the Solar
// line's first third of the day (segments/idx_solar_0..23, points 0-24,
// see dwgs/SolarPlot1.pfodDwg_json): it reads solarData (the model,
// PlotData.h/PlotDataInstances.h) and sends each segment as an indexed
// line, so the chart reflects whatever solarData currently holds instead
// of the fixed decorative curve pfodWeb generated.
// Re-generating from the pfodWeb Designer overwrites Dwg_SolarPlot1.h/.cpp
// and leaves this file alone.
#include "Dwg_SolarPlot1.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MySolarPlot1 : public Dwg_SolarPlot1 {
  public:
    virtual void init();
  protected:
    virtual void sendIndexedItems();
  private:
    bool dummyDataFilled = false;
};

// Chains to the base first (registers with the parser), then -- once only,
// since both PowerChartGrid and TomorrowChart insertDwg this drawing and so
// both call init() on it -- clears solarData to PlotData::clear()'s -1
// "no data yet" sentinel, so this chart draws empty/off-scale instead of
// a fabricated demo curve until a real feed (GatewayRefresh.cpp's live
// point, SolarForecastRefresh.cpp's forecast, or
// EnlightenHistoryRefresh.cpp's merge) actually replaces it. An earlier
// version of this file filled a plausible-looking dummy curve instead
// (partly ACTUAL-coloured up to a hardcoded "2pm" cutoff, later made
// entirely PREDICTED-coloured) -- either way it was still fabricated
// data that looked real on the chart before anything genuine had arrived.
// Clears the whole day (all of solarData, not just the points this file's
// 24 segments use) so SolarPlot2/3 start from the same clean state.
void MySolarPlot1::init() {
  Dwg_SolarPlot1::init(); // always chain to the base first
  // No dwgRefresh_ms here: as an insertDwg'd child, it would be discarded
  // on the wire and do nothing anyway -- and Dwg_Home's own dwgRefresh_ms
  // (the one that would otherwise drive this) is 0 too, as of the
  // 2026-10-08 Designer regeneration. This drawing still auto-refreshes
  // every ~30s regardless, via a separate mechanism -- see
  // Dwg_HomeApp.cpp's own comment (sendMainMenuUpdate()'s `|+` lines,
  // tied to the main menu's own refresh_ms). See pfodAI-guide.md section
  // 6, "Refresh is the trap", for the dwgRefresh_ms mechanism itself.
  if (dummyDataFilled) {
    return;
  }
  dummyDataFilled = true;

  solarData.clear();
}

// x positions are design-time fixed (12 + 2*segment), matching
// dwgs/SolarPlot1.pfodDwg_json exactly -- only each segment's y (from
// solarData's value) and colour come from the model.
void MySolarPlot1::sendIndexedItems() {
  const float X_START = 12.0;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[24] = {
    &idx_solar_0,  &idx_solar_1,  &idx_solar_2,  &idx_solar_3,
    &idx_solar_4,  &idx_solar_5,  &idx_solar_6,  &idx_solar_7,
    &idx_solar_8,  &idx_solar_9,  &idx_solar_10, &idx_solar_11,
    &idx_solar_12, &idx_solar_13, &idx_solar_14, &idx_solar_15,
    &idx_solar_16, &idx_solar_17, &idx_solar_18, &idx_solar_19,
    &idx_solar_20, &idx_solar_21, &idx_solar_22, &idx_solar_23,
  };
  for (int i = 0; i < 24; i++) {
    float v0 = solarData.points[i].value;
    float v1 = solarData.points[i + 1].value;
    if (v0 < 0.0 || v1 < 0.0) {
      continue; // no data yet for this segment -- leave its last-sent/placeholder state
    }
    float xOffset = X_START + X_STEP * i;
    float y0 = screenYForKw(v0);
    float y1 = screenYForKw(v1);
    dwgsPtr->line().idx(*idx[i]).color(solarData.points[i + 1].color)
        .size(X_STEP, y1 - y0).offset(xOffset, y0).send();
  }
}

MySolarPlot1 mySolarPlot1;
Dwg_SolarPlot1& get_dwg_SolarPlot1() { return mySolarPlot1; }
// ============== end of Dwg_SolarPlot1App.cpp  file
