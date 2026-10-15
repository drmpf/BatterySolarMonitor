// Dwg_TomorrowConsumptionPlot1App.cpp  file =================
// Hand-written subclass of the generated Dwg_TomorrowConsumptionPlot1 --
// see pfodAI-guide.md section 6, "The shape of the logic - model, view,
// controller". Controller's transfer half for the Tomorrow chart's Avg
// Consumption line, first third of the day
// (idx_tomorrowConsumption_0..23, points 0-24, see
// dwgs/TomorrowConsumptionPlot1.pfodDwg_json). Reads
// tomorrowAvgConsumptionData (the model, PlotData.h/PlotDataInstances.h)
// and sends each segment as an indexed line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_TomorrowConsumptionPlot1.h/.cpp and leaves this file alone.
#include "Dwg_TomorrowConsumptionPlot1.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyTomorrowConsumptionPlot1 : public Dwg_TomorrowConsumptionPlot1 {
  public:
    virtual void init();
  protected:
    virtual void sendIndexedItems();
  private:
    bool dummyDataFilled = false;
};

// Chains to the base first, then -- once only -- clears
// tomorrowAvgConsumptionData to PlotData::clear()'s -1 "no data yet"
// sentinel, so this chart draws empty/off-scale instead of a fabricated
// demo curve until AvgConsumptionRefresh.cpp's paintForecast() fills it
// with the real 5-day average (it repaints the WHOLE day at once, same as
// SolarForecastRefresh.cpp does for tomorrowSolarData -- there's no
// "actual" boundary for a day that hasn't happened yet). Fills the whole
// day so TomorrowConsumptionPlot2/3 start from the same clean state.
void MyTomorrowConsumptionPlot1::init() {
  Dwg_TomorrowConsumptionPlot1::init(); // always chain to the base first
  if (dummyDataFilled) {
    return;
  }
  dummyDataFilled = true;

  tomorrowAvgConsumptionData.clear();
}

// x positions are design-time fixed (12 + 2*segment), matching
// dwgs/TomorrowConsumptionPlot1.pfodDwg_json exactly -- only each
// segment's y (from tomorrowAvgConsumptionData's value) and colour come
// from the model.
void MyTomorrowConsumptionPlot1::sendIndexedItems() {
  const float X_START = 12.0;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[24] = {
    &idx_tomorrowConsumption_0,  &idx_tomorrowConsumption_1,  &idx_tomorrowConsumption_2,  &idx_tomorrowConsumption_3,
    &idx_tomorrowConsumption_4,  &idx_tomorrowConsumption_5,  &idx_tomorrowConsumption_6,  &idx_tomorrowConsumption_7,
    &idx_tomorrowConsumption_8,  &idx_tomorrowConsumption_9,  &idx_tomorrowConsumption_10, &idx_tomorrowConsumption_11,
    &idx_tomorrowConsumption_12, &idx_tomorrowConsumption_13, &idx_tomorrowConsumption_14, &idx_tomorrowConsumption_15,
    &idx_tomorrowConsumption_16, &idx_tomorrowConsumption_17, &idx_tomorrowConsumption_18, &idx_tomorrowConsumption_19,
    &idx_tomorrowConsumption_20, &idx_tomorrowConsumption_21, &idx_tomorrowConsumption_22, &idx_tomorrowConsumption_23,
  };
  for (int i = 0; i < 24; i++) {
    float v0 = tomorrowAvgConsumptionData.points[i].value;
    float v1 = tomorrowAvgConsumptionData.points[i + 1].value;
    if (v0 < 0.0 || v1 < 0.0) {
      continue; // no data yet for this segment -- leave its last-sent/placeholder state
    }
    float xOffset = X_START + X_STEP * i;
    float y0 = screenYForKw(v0);
    float y1 = screenYForKw(v1);
    dwgsPtr->line().idx(*idx[i]).color(tomorrowAvgConsumptionData.points[i].color)
        .size(X_STEP, y1 - y0).offset(xOffset, y0).send();
  }
}

MyTomorrowConsumptionPlot1 myTomorrowConsumptionPlot1;
Dwg_TomorrowConsumptionPlot1& get_dwg_TomorrowConsumptionPlot1() { return myTomorrowConsumptionPlot1; }
// ============== end of Dwg_TomorrowConsumptionPlot1App.cpp  file
