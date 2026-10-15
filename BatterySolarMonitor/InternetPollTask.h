// InternetPollTask.h  file  =================
// Starts the single shared background task that every internet-facing
// poll in this project (GatewayPollTask.h, SolarForecastTask.h --
// solar+weather combined, EnlightenHistoryTask.h) now runs on -- see the
// .cpp file for why they were consolidated from separate tasks into this
// one.
#ifndef INTERNET_POLL_TASK_H
#define INTERNET_POLL_TASK_H

// Call once from setup(), after WiFi is connected AND after
// resolveSolarForecastHost() has already been called on the main task
// (SolarForecastFetch.h -- its DNS resolve can never happen on this
// shared background task, same reasoning as always).
void startInternetPollTask();

#endif
// ================= end of InternetPollTask.h  file
