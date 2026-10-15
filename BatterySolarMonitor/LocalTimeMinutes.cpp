// LocalTimeMinutes.cpp  file  =================
#include "LocalTimeMinutes.h"
#include <time.h>
#include <stdio.h>

int localTimeMinutesSinceMidnight() {
  time_t now = time(nullptr);
  struct tm *tmPtr = localtime(&now);
  return tmPtr->tm_hour * 60 + tmPtr->tm_min;
}

void localTodayDateString(char *buf, size_t bufSize) {
  time_t now = time(nullptr);
  struct tm *tmPtr = localtime(&now);
  snprintf(buf, bufSize, "%04d-%02d-%02d", tmPtr->tm_year + 1900, tmPtr->tm_mon + 1, tmPtr->tm_mday);
}

void localDateStringDaysAgo(int daysAgo, char *buf, size_t bufSize) {
  time_t then = time(nullptr) - (time_t)daysAgo * 86400L;
  struct tm *tmPtr = localtime(&then);
  snprintf(buf, bufSize, "%04d-%02d-%02d", tmPtr->tm_year + 1900, tmPtr->tm_mon + 1, tmPtr->tm_mday);
}

void localMonthAndDayOfYear(int daysFromToday, int &monthOut, int &dayOfYearOut) {
  time_t then = time(nullptr) + (time_t)daysFromToday * 86400L;
  struct tm *tmPtr = localtime(&then);
  monthOut = tmPtr->tm_mon + 1;
  dayOfYearOut = tmPtr->tm_yday + 1;
}
// ================= end of LocalTimeMinutes.cpp  file
