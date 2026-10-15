// PlotData.h  file  =================
// Hand-written model class (see pfodAI-guide.md section 6, "The shape of the
// logic - model, view, controller"). Holds the sampled values for ONE
// plotted line -- e.g. the Solar line in the "Solar & Consumption" chart.
// Deliberately contains no pfod types (no dwgsPtr, no send...(), no parser
// reference) so it can be filled in and unit tested with no knowledge of how,
// or whether, it is ever displayed.
#ifndef PLOT_DATA_H
#define PLOT_DATA_H

#include <Arduino.h>

// One sampled point of a plotted line.
// color is the pfod colour (0-255, see pfodDwg_json-format.md section 3) to
// draw the line segment leading into this point. Storing a colour per point,
// rather than one colour for the whole line, is what lets a single plotted
// line change colour partway along its length -- e.g. one colour for
// actual/measured values and a different colour for predicted values later
// in the same line.
struct PlotPoint {
  float value;   // the y-axis value at this point
  uint8_t color; // pfod colour index for the segment leading into this point
};

// Holds one plotted line's worth of sampled data. One instance is created
// per chart plot (see PlotDataInstances.h) -- e.g. Battery Charge, Solar,
// Consumption, Tomorrow's Solar, Tomorrow's Avg Consumption.
//
// Points are addressed by time of day, not by array position -- the data
// this feeds (a sensor reading, a forecast value) naturally comes with a
// time, not an index, and a caller supplying it should not need to know how
// points are stored internally.
class PlotData {
  public:
    // 5am - 10pm inclusive, one point every 15 minutes:
    // (17 hours * 60 minutes / 15 minutes) + 1 = 69 points.
    // This matches the point spacing already fixed by the chart drawings
    // (dwgs/SolarPlot1-3.pfodDwg_json etc, 68 line segments each).
    static const int MAX_POINTS = 69;
    static const int START_MINUTES = 5 * 60; // first point is 5am
    static const int STEP_MINUTES = 15;      // one point every 15 minutes
    static const int END_MINUTES = START_MINUTES + (MAX_POINTS - 1) * STEP_MINUTES; // last point is 10pm

    PlotPoint points[MAX_POINTS];
    int numPoints = 0;

    // Discards all stored points, ready to be refilled. Explicitly writes
    // -1 into every point's value (never a real reading -- kW and % are
    // never negative in this project) rather than leaving them at
    // whatever they previously held (a stale/dummy reading) or at their
    // raw zero-initialized default (0.0, which LOOKS like a plausible
    // real reading, unlike -1) -- callers that render every slot
    // regardless of numPoints (each Dwg_XPlot1/2/3App.cpp's
    // sendIndexedItems(), which reads .points[i] directly, not via
    // valueAtTime()) need this so a chart with no real data yet draws as
    // empty/off-scale rather than a misleading dummy curve.
    void clear() {
      for (int i = 0; i < MAX_POINTS; i++) {
        points[i].value = -1.0;
        points[i].color = 0;
      }
      numPoints = 0;
    }

    // Sets the point nearest a given time of day, adding it if it is the
    // latest point seen so far or overwriting it in place if it was already
    // set -- either way the caller just supplies a time, never an index.
    // A time that isn't exactly on this line's 15-minute grid rounds to the
    // nearest slot, so a value polled more often than every 15 minutes
    // (e.g. a consumption reading every 5 minutes) still lands on the fixed
    // sample times: the first two of three 5-minute polls inside one
    // 15-minute window round to, and so overwrite, the same point as the
    // third. Ignored if minutesSinceMidnight is before 5am or after 10pm.
    // minutesSinceMidnight -- time of day, 0-1439 (5am = 300, 10pm = 1320)
    // value -- the y-axis value for this sample
    // color -- pfod colour index for the segment leading into this point
    void setPointAtTime(int minutesSinceMidnight, float value, uint8_t color) {
      int index = indexForTime(minutesSinceMidnight);
      if (index < 0) {
        return;
      }
      points[index].value = value;
      points[index].color = color;
      if (index >= numPoints) {
        numPoints = index + 1;
      }
    }

    // Returns the value at the point nearest a given time of day (same
    // rounding as setPointAtTime()), or 0.0 if minutesSinceMidnight is
    // before 5am or after 10pm, or that slot has never been set --
    // callers reading a value (e.g. to project a later value forward from
    // it) address by time, the same as callers writing one.
    float valueAtTime(int minutesSinceMidnight) const {
      int index = indexForTime(minutesSinceMidnight);
      if (index < 0 || index >= numPoints) {
        return 0.0;
      }
      return points[index].value;
    }

    // Returns the exact grid time (minutes since midnight) that
    // valueAtTime()/setPointAtTime() would resolve 'minutesSinceMidnight'
    // to -- the same round-to-nearest-slot rule as this class's own
    // (private) indexForTime(), exposed so callers that need to reason
    // about slot boundaries independently (e.g. "which slots might
    // already hold a live reading, so a forecast refresh must not
    // overwrite them, vs. which are purely future and safe to") can match
    // it exactly, rather than re-deriving their own rounding and risking
    // a mismatch. Confirmed as a real, non-hypothetical risk: both
    // BatteryProjection.cpp's firstProjectedMinute and
    // SolarForecastRefresh.cpp's skipBeforeMinutes independently
    // reimplemented a FLOOR-based rounding that silently disagreed with
    // this round-to-NEAREST rule roughly half the time (whenever
    // minutesSinceMidnight fell in the latter half of its own 15-min
    // window), each time landing on the SAME slot valueAtTime() had just
    // read the live/actual value from and overwriting it with stale or
    // predicted data one step early -- both now call this instead.
    // Clamps to START_MINUTES/END_MINUTES if out of range (callers that
    // need to detect "no data here at all" should check that themselves,
    // same as valueAtTime()'s own 0.0 "no data" caveat -- this method
    // only answers "which slot", never "is there a value there").
    static int nearestSlotMinutes(int minutesSinceMidnight) {
      if (minutesSinceMidnight < START_MINUTES) {
        return START_MINUTES;
      }
      if (minutesSinceMidnight > END_MINUTES) {
        return END_MINUTES;
      }
      int offset = minutesSinceMidnight - START_MINUTES;
      int index = (offset + STEP_MINUTES / 2) / STEP_MINUTES;
      return START_MINUTES + index * STEP_MINUTES;
    }

    // Total energy (kWh) over this line's whole 5am-10pm range, summing
    // each point's kW over its own STEP_MINUTES slot -- same per-slot
    // convention as EnlightenHistoryFetch.cpp's own totalKwh calculation.
    // A point still holding the -1 "no data yet" sentinel (PlotPoint's own
    // clear()) contributes 0, not -1 -- unset slots are excluded from the
    // total rather than subtracted from it. Reads whatever is CURRENTLY in
    // points[] regardless of colour, so for a line that mixes ACTUAL
    // (elapsed, measured) and PREDICTED (future, forecast) points -- e.g.
    // solarData/consumptionData -- this naturally blends real-so-far with
    // forecast-for-the-rest, improving as the day's real readings replace
    // earlier forecast values. Ignores numPoints (reads all MAX_POINTS
    // slots directly), matching every Dwg_XPlot1/2/3App.cpp's own
    // sendIndexedItems() convention.
    float totalKwh() const {
      float total = 0.0;
      for (int i = 0; i < MAX_POINTS; i++) {
        if (points[i].value < 0.0) {
          continue; // no data yet for this slot
        }
        total += points[i].value * (STEP_MINUTES / 60.0);
      }
      return total;
    }

  private:
    // Converts a time of day (minutes since midnight) to the index of the
    // nearest point on this line's 15-minute grid, or -1 if the time is
    // before 5am or after 10pm.
    // Rounding can never overflow past the last slot (MAX_POINTS-1),
    // regardless of STEP_MINUTES being odd or even: the largest offset
    // reaching this line is END_MINUTES-START_MINUTES, i.e. exactly
    // (MAX_POINTS-1)*STEP_MINUTES (see END_MINUTES above), and
    // floor(STEP_MINUTES/2) is always < STEP_MINUTES, so
    // (MAX_POINTS-1)*STEP_MINUTES + floor(STEP_MINUTES/2) is always
    // < MAX_POINTS*STEP_MINUTES. This only holds because END_MINUTES is
    // derived from MAX_POINTS/STEP_MINUTES, not a hardcoded value -- keep
    // it that way if either constant changes.
    int indexForTime(int minutesSinceMidnight) const {
      if (minutesSinceMidnight < START_MINUTES || minutesSinceMidnight > END_MINUTES) {
        return -1;
      }
      int offset = minutesSinceMidnight - START_MINUTES;
      return (offset + STEP_MINUTES / 2) / STEP_MINUTES; // round to nearest slot
    }
};

#endif
// ================= end of PlotData.h  file
