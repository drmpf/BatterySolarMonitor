// GatewayPollTask.h  file  =================
// Runs fetchGatewayPower() (GatewayFetch.h) on a fixed interval, so a
// slow/failed gateway request never blocks the pfod HTTP server.
//
// gatewayIp() (EnphaseAuth.h) is already an IP literal, not a hostname, so
// unlike SolarForecastFetch.cpp's api.open-meteo.com fetch, this never
// triggers a DNS lookup and needs no resolve-on-main-thread step -- safe
// to call fetchGatewayPower() directly from any task.
//
// checkGatewayPoll() runs on InternetPollTask.cpp's single shared
// background task, alongside the other internet-facing polls (Solar
// forecast -- which also covers the daily weather forecast, see
// SolarForecastTask.h -- and Enlighten history) -- see that file for why
// they all now share one task instead of each having its own.
#ifndef GATEWAY_POLL_TASK_H
#define GATEWAY_POLL_TASK_H

// Runs fetchGatewayPower() if due (POLL_INTERVAL_MS normally, sooner via
// FETCH_RETRY_DELAY_MS if the last attempt failed, either the whole fetch
// or just its battery-status sub-fetch -- see GatewayPollTask.cpp) and
// stores a successful result under
// the lock below. Call repeatedly from InternetPollTask.cpp's shared
// background task -- a cheap millis() check and an immediate return if
// not due yet; does the actual (~1-4s) network I/O inline when it is.
void checkGatewayPoll();

void lockGatewayReading();
void unlockGatewayReading();

// Returns true (once) the first time this is called after a successful
// poll updated latestSolarKw/latestConsumeKw (GatewayInstances.h) --
// clears the flag, so each update is picked up exactly once by whoever
// calls this (GatewayRefresh.cpp).
bool gatewayReadingUpdated();

// How many battery-SOC fetches (fetchGatewayPower()'s own batterySocPercent
// -- either the whole fetch failed, or just its battery-status sub-fetch
// did) have failed in a row, right up to and including the most recent
// poll -- reset to 0 the moment any poll gets a valid (>=0.0) reading.
// Not consumed/cleared by reading -- unlike gatewayReadingUpdated(), any
// number of callers can check this at any time (BuzzerControl.cpp uses it
// to require several consecutive failures, not just one, before treating
// "can't confirm the battery's SOC" as alarm-worthy -- see that file).
int consecutiveBatteryFetchFailures();

#endif
// ================= end of GatewayPollTask.h  file
