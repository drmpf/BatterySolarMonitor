// LocalTimeMinutes.h  file  =================
#ifndef LOCAL_TIME_MINUTES_H
#define LOCAL_TIME_MINUTES_H

#include <stddef.h> // size_t

// Returns the current local time as minutes-since-midnight (0-1439),
// reading the same system clock ntpSupport.h keeps NTP-synced. Used to
// find which PlotData 15-min slot "now" falls in (GatewayRefresh.h) and
// to stop a forecast refresh overwriting already-elapsed/actual points
// (SolarForecastMapping.h's skipBeforeMinutes).
int localTimeMinutesSinceMidnight();

// Writes today's local date as "YYYY-MM-DD" into buf (bufSize must be at
// least 11). Used to build the Enlighten history query's date= parameter
// (EnlightenHistoryFetch.h).
void localTodayDateString(char *buf, size_t bufSize);

// Writes the local date 'daysAgo' days before today as "YYYY-MM-DD" into
// buf (bufSize must be at least 11, daysAgo >= 1). Used to build each of
// AvgConsumptionTask.h's 5 past-day Enlighten history queries. Correctly
// handles month/year rollovers -- subtracts whole days in epoch seconds
// and lets localtime() renormalize, rather than doing calendar
// arithmetic by hand.
void localDateStringDaysAgo(int daysAgo, char *buf, size_t bufSize);

// Writes the local month (1-12) and day-of-year (1-366) for
// "today + daysFromToday" (0 = today, 1 = tomorrow, negative = past).
// Used by SunPosition.h to compute solar declination/the AEST-AEDT DST
// offset for a forecast day that isn't necessarily today.
void localMonthAndDayOfYear(int daysFromToday, int &monthOut, int &dayOfYearOut);

#endif
// ================= end of LocalTimeMinutes.h  file
