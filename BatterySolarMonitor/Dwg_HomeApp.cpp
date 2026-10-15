// Dwg_HomeApp.cpp  file =================
// Hand-written subclass of the generated Dwg_Home -- see
// pfodAI-guide.md section 6 ("Refresh is the trap") and
// pfodDwg_json-format.md's insertDwg section: an INSERTED dwg's own
// dwgRefresh_ms is discarded on the wire and schedules nothing -- only
// the TOP-LEVEL dwg a menu Drawing item points at (this one, "Home" in
// Home.pfodMenu_json) would drive a client-side dwgRefresh_ms timer for
// this screen, with every insertDwg'd child (Dwg_StatusApp.cpp,
// PowerChartGrid's SolarPlot1-3/ConsumptionPlot1-3, BatteryChartGrid's
// BatteryPlot1-3) re-fetched only when Home itself is. As of the
// 2026-10-08 Designer regeneration, Dwg_Home's own dwgRefresh_ms
// (Dwg_Home.cpp's constructor) is ALSO 0, like every other dwg's, so
// THAT particular mechanism no longer drives anything.
//
// This screen still auto-refreshes every ~30s regardless, by design, via
// a SEPARATE mechanism: pfodMainMenu::sendMainMenuUpdate() (generated,
// pfodMainMenu.cpp, mirrored in pfodMainMenuApp.cpp's own override)
// unconditionally sends `|+dwgMenuItem_Home_Cmd` (and the TodaySolar/
// Tomorrow ones) on every menu update -- an explicit "refresh this
// drawing item" instruction, independent of dwgRefresh_ms, tied instead
// to the MAIN MENU's own refresh_ms (pfodMainMenu.cpp, 30000) cadence.
// Confirmed in practice via pfod-messages-2026-10-08T03-10-08.164Z.csv:
// the client's periodic "M{V1:.}" version-check gets back a `{;...}`
// menu update carrying that `|+`, and immediately re-fetches this whole
// screen (Status plus every BatteryChartGrid/PowerChartGrid child) in
// response -- confirmed as the intended behaviour, not a bug.
// Re-generating from the pfodWeb Designer overwrites Dwg_Home.h/.cpp and
// leaves this file alone.
//
// Priming this dwg's own pfodAutoIdx/pfodAutoCmd happens in the generated
// Dwg_Home.cpp's own init(), not here -- every dwg primes itself there,
// uniformly, whether or not it also has a hand-written override.
#include "Dwg_Home.h"

class MyHome : public Dwg_Home {
  public:
    virtual void init();
};

// Chains to the base first -- which itself calls init() on every
// insertDwg'd child via their own weak-factory accessors (pfodAI-guide.md
// section 6.6), so this one setting is all that's needed for the whole
// screen to auto-refresh.
void MyHome::init() {
  Dwg_Home::init();
}

MyHome myHome;
Dwg_Home& get_dwg_Home() { return myHome; }
// ============== end of Dwg_HomeApp.cpp  file
