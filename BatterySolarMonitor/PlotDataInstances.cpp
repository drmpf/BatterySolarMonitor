// PlotDataInstances.cpp  file  =================
// Definitions for the plot-data model instances declared in
// PlotDataInstances.h -- see that file for what each one is.
#include "PlotDataInstances.h"
#include <Arduino.h>

PlotData batteryChargeData;
PlotData solarData;
PlotData consumptionData;
PlotData tomorrowSolarData;
PlotData tomorrowAvgConsumptionData;

// Lazy-created -- FreeRTOS primitives aren't safe to create before the
// scheduler/heap are up (same reasoning as SolarForecastTask.cpp's own
// mutex()). Recursive -- see lockTomorrowForecastData()'s own header
// comment (PlotDataInstances.h) for why a plain mutex won't do here.
static SemaphoreHandle_t tomorrowForecastMutex = NULL;

static SemaphoreHandle_t mutex() {
  if (tomorrowForecastMutex == NULL) {
    tomorrowForecastMutex = xSemaphoreCreateRecursiveMutex();
  }
  return tomorrowForecastMutex;
}

void lockTomorrowForecastData() {
  xSemaphoreTakeRecursive(mutex(), portMAX_DELAY);
}

void unlockTomorrowForecastData() {
  xSemaphoreGiveRecursive(mutex());
}
// ================= end of PlotDataInstances.cpp  file
