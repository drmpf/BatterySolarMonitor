// AvgConsumptionProfile.h  file  =================
// Hand-written model class (see pfodAI-guide.md section 6, "The shape of
// the logic - model, view, controller"). Holds a running average of true
// home consumption (kW) per 15-min slot, built incrementally from however
// many of the last few calendar days' Enlighten history
// (EnlightenHistoryFetch.h's fetchEnlightenConsumptionForDate()) actually
// succeeded -- see AvgConsumptionTask.h. Matches
// dailyPrediction/plot_today_solar.py's load_avg_consumption_profile()
// (data/consumption_15min_history.csv), same idea, ported.
//
// Uses PlotData's own 5am-10pm/15-min grid sizing (PlotData::MAX_POINTS)
// so it addresses the same way as the chart it forecasts, but is NOT a
// PlotData/chart line itself -- no colour, and folding in a new day
// updates every slot's EXISTING average in place rather than overwriting
// it wholesale.
#ifndef AVG_CONSUMPTION_PROFILE_H
#define AVG_CONSUMPTION_PROFILE_H

#include "PlotData.h"

class AvgConsumptionProfile {
  public:
    float avgKw[PlotData::MAX_POINTS];

    // Whole-day total energy (kWh, midnight-to-midnight -- see
    // EnlightenHistoryFetch.h's fetchEnlightenConsumptionForDate()
    // totalKwhOut), tracked across the same days folded into avgKw[]
    // above -- avgDailyKwh via the same incremental-mean update, maxDailyKwh
    // as a running max. Feeds Dwg_StatusApp.cpp's Usage line ("Usage
    // X.XkWh avg, Y.YkWh max").
    float avgDailyKwh = 0.0;
    float maxDailyKwh = 0.0;

    // Number of days successfully folded in so far (i.e. addDay() calls,
    // not slotSampleCount[] below). 0 doubles as this profile's own "no
    // forecast available" sentinel -- see hasData() -- there's no separate
    // "-1" field since a count of 0 is already unambiguous, and the count
    // itself is useful (how many days the current average actually
    // reflects).
    int successCount = 0;

    // Per-slot version of successCount above -- how many of the days
    // folded into avgKw[i] actually had a real reading for THAT slot, not
    // just how many days succeeded overall. Needed because a single day's
    // own dayKw[] (EnlightenHistoryFetch.h's fetchEnlightenConsumptionForDate())
    // can have gaps at specific slots (Enlighten hasn't backfilled them)
    // while still having enough real data elsewhere to count as a
    // successful day -- addDay() uses this, not successCount, as each
    // slot's own incremental-mean denominator, so a day's gap at slot i
    // is excluded from avgKw[i] instead of dragging it toward zero.
    int slotSampleCount[PlotData::MAX_POINTS];

    void clear() {
      for (int i = 0; i < PlotData::MAX_POINTS; i++) {
        avgKw[i] = 0.0;
        slotSampleCount[i] = 0;
      }
      avgDailyKwh = 0.0;
      maxDailyKwh = 0.0;
      successCount = 0;
    }

    // Folds one more day's already-parsed 5am-10pm profile (dayKw) and
    // whole-day total (dayTotalKwh) into the running averages via an
    // incremental mean update:
    //     newAvg = oldAvg + (dayValue - oldAvg) / newCount
    // -- correct regardless of how many days have been folded in already
    // (including the first), so every successful call immediately
    // produces a usable average (over whatever count it's now at)
    // instead of requiring all days up front. maxDailyKwh is a plain
    // running max, not an average, so the first day's total seeds it
    // directly rather than starting from 0.0.
    //
    // dayKw[i] < 0.0 (fetchEnlightenConsumptionForDate()'s own "no data
    // for this slot today" sentinel, matching PlotData's convention) is
    // skipped entirely for that slot -- confirmed as a real, not
    // hypothetical, risk: blending a fabricated 0.0 in for a slot
    // Enlighten simply hadn't backfilled yet would silently drag avgKw[i]
    // toward zero every time that gap happened to land on the same slot,
    // even though other days had perfectly good data there.
    void addDay(const float dayKw[PlotData::MAX_POINTS], float dayTotalKwh) {
      successCount++;
      for (int i = 0; i < PlotData::MAX_POINTS; i++) {
        if (dayKw[i] < 0.0) {
          continue; // no data for this slot today -- don't count it
        }
        slotSampleCount[i]++;
        avgKw[i] += (dayKw[i] - avgKw[i]) / slotSampleCount[i];
      }
      avgDailyKwh += (dayTotalKwh - avgDailyKwh) / successCount;
      if (successCount == 1 || dayTotalKwh > maxDailyKwh) {
        maxDailyKwh = dayTotalKwh;
      }
    }

    // False until at least one day has been successfully folded in --
    // callers (BatteryProjection.cpp, AvgConsumptionRefresh.cpp) must
    // check this before trusting/plotting avgKw[]. Doesn't check
    // slotSampleCount[] per-slot -- a slot with zero samples so far (every
    // folded-in day happened to have a gap exactly there, vastly less
    // likely the more days succeed) is left at its clear()'d 0.0, the same
    // pragmatic "usable once at least one day succeeds" tolerance this
    // profile already applies at the whole-day level.
    bool hasData() const {
      return successCount > 0;
    }
};

#endif
// ================= end of AvgConsumptionProfile.h  file
