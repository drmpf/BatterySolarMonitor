// Dwg_TomorrowChartApp.cpp  file =================
// Hand-written subclass of the generated Dwg_TomorrowChart -- see
// pfodAI-guide.md section 6 ("Refresh is the trap") and
// pfodDwg_json-format.md's insertDwg section: an INSERTED dwg's own
// dwgRefresh_ms is discarded on the wire and schedules nothing -- only
// the TOP-LEVEL dwg a menu Drawing item points at (this one, "TomorrowChart"
// in Home.pfodMenu_json) would drive a client-side dwgRefresh_ms timer
// for this screen, with every insertDwg'd child (TomorrowSolarPlot1-3,
// TomorrowConsumptionPlot1-3) re-fetched only when TomorrowChart itself
// is. As of the 2026-10-08 Designer regeneration, TomorrowChart's own
// dwgRefresh_ms (Dwg_TomorrowChart.cpp's constructor) is ALSO 0, like
// every other dwg's, so THAT particular mechanism no longer drives
// anything.
//
// This screen still auto-refreshes every ~30s regardless, by design --
// see Dwg_HomeApp.cpp's own comment (same mechanism, this screen's other
// top-level dwg) for the full explanation: pfodMainMenu::sendMainMenuUpdate()
// unconditionally sends `|+dwgMenuItem_Tomorrow_Cmd` on every menu
// update, tied to the MAIN MENU's own refresh_ms (pfodMainMenu.cpp,
// 30000) cadence instead of this dwg's own dwgRefresh_ms.
// Re-generating from the pfodWeb Designer overwrites
// Dwg_TomorrowChart.h/.cpp and leaves this file alone.
//
// Priming this dwg's own pfodAutoIdx/pfodAutoCmd happens in the generated
// Dwg_TomorrowChart.cpp's own init(), not here -- every dwg primes itself
// there, uniformly, whether or not it also has a hand-written override.
#include "Dwg_TomorrowChart.h"

class MyTomorrowChart : public Dwg_TomorrowChart {
  public:
    virtual void init();
};

// Chains to the base first -- which itself calls init() on every
// insertDwg'd child via their own weak-factory accessors (pfodAI-guide.md
// section 6.6), so this one setting is all that's needed for the whole
// screen to auto-refresh.
void MyTomorrowChart::init() {
  Dwg_TomorrowChart::init();
}

MyTomorrowChart myTomorrowChart;
Dwg_TomorrowChart& get_dwg_TomorrowChart() { return myTomorrowChart; }
// ============== end of Dwg_TomorrowChartApp.cpp  file
