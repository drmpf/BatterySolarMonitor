// Dwg_ConsumptionPlot1App.cpp  file =================
// Hand-written subclass of the generated Dwg_ConsumptionPlot1 -- see
// pfodAI-guide.md section 6, "The shape of the logic - model, view,
// controller". Controller's transfer half for the Consumption line's
// first third of the day (idx_consumption_0..23, points 0-24, see
// dwgs/ConsumptionPlot1.pfodDwg_json): reads consumptionData (the model,
// PlotData.h/PlotDataInstances.h) and sends each segment as an indexed
// line.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_ConsumptionPlot1.h/.cpp and leaves this file alone.
#include "Dwg_ConsumptionPlot1.h"
#include "PlotDataInstances.h"
#include "ChartAxis.h"

class MyConsumptionPlot1 : public Dwg_ConsumptionPlot1 {
  public:
    virtual void init();
  protected:
    virtual void sendIndexedItems();
  private:
    bool dummyDataFilled = false;
};

// Chains to the base first, then -- once only, since both PowerChartGrid
// and TomorrowChart insertDwg this drawing -- clears consumptionData to
// PlotData::clear()'s -1 "no data yet" sentinel, so this chart draws
// empty/off-scale instead of a fabricated demo curve until a real feed
// (GatewayRefresh.cpp's live point, AvgConsumptionRefresh.cpp's average
// forecast, or EnlightenHistoryRefresh.cpp's merge) actually replaces it.
// An earlier version of this file filled a plausible-looking dummy curve
// instead (a flat 1kW base plus morning/evening bumps) -- fabricated data
// that looked real on the chart before anything genuine had arrived, and
// (unlike solarData/batteryChargeData, which get repainted by
// SolarForecastRefresh.cpp/BatteryProjection.cpp within seconds/minutes)
// consumptionData had no mechanism to ever correct it before
// AvgConsumptionRefresh.cpp existed. Clears the whole day so
// ConsumptionPlot2/3 start from the same clean state.
void MyConsumptionPlot1::init() {
  Dwg_ConsumptionPlot1::init(); // always chain to the base first
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

  consumptionData.clear();
}

// x positions are design-time fixed (12 + 2*segment), matching
// dwgs/ConsumptionPlot1.pfodDwg_json exactly -- only each segment's y
// (from consumptionData's value) and colour come from the model.
void MyConsumptionPlot1::sendIndexedItems() {
  const float X_START = 12.0;
  const float X_STEP = 2.0;
  pfodAutoIdx* idx[24] = {
    &idx_consumption_0,  &idx_consumption_1,  &idx_consumption_2,  &idx_consumption_3,
    &idx_consumption_4,  &idx_consumption_5,  &idx_consumption_6,  &idx_consumption_7,
    &idx_consumption_8,  &idx_consumption_9,  &idx_consumption_10, &idx_consumption_11,
    &idx_consumption_12, &idx_consumption_13, &idx_consumption_14, &idx_consumption_15,
    &idx_consumption_16, &idx_consumption_17, &idx_consumption_18, &idx_consumption_19,
    &idx_consumption_20, &idx_consumption_21, &idx_consumption_22, &idx_consumption_23,
  };
  for (int i = 0; i < 24; i++) {
    float v0 = consumptionData.points[i].value;
    float v1 = consumptionData.points[i + 1].value;
    if (v0 < 0.0 || v1 < 0.0) {
      continue; // no data yet for this segment -- leave its last-sent/placeholder state
    }
    float xOffset = X_START + X_STEP * i;
    float y0 = screenYForKw(v0);
    float y1 = screenYForKw(v1);
    dwgsPtr->line().idx(*idx[i]).color(consumptionData.points[i + 1].color)
        .size(X_STEP, y1 - y0).offset(xOffset, y0).send();
  }
}

MyConsumptionPlot1 myConsumptionPlot1;
Dwg_ConsumptionPlot1& get_dwg_ConsumptionPlot1() { return myConsumptionPlot1; }
// ============== end of Dwg_ConsumptionPlot1App.cpp  file
