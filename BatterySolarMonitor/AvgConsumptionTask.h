// AvgConsumptionTask.h  file  =================
// Runs a batch of up to DAYS_TO_AVERAGE (AvgConsumptionTask.cpp)
// sequential fetchEnlightenConsumptionForDate() calls (EnlightenHistoryFetch.h)
// on InternetPollTask.cpp's shared background task, per request --
// AvgConsumptionRefresh.cpp is the one-shot-per-boot trigger: it calls
// requestAvgConsumptionFetch() once, after resolveEnlightenHosts() has
// run on the main thread (same DNS-from-background-task hazard as
// EnlightenHistoryTask.h, same fix -- a request/poll handshake instead of
// resolving here).
//
// Each of the up to 5 requests folds into avgConsumptionProfile
// (AvgConsumptionInstances.h) as soon as IT succeeds (not after all 5),
// so a partial result is usable immediately if some days fail -- see
// AvgConsumptionProfile.h's addDay(). The whole batch can take a while
// (up to ~5x13s worst case, one request's own bound -- see
// EnlightenHistoryFetch.cpp's HANDSHAKE_TIMEOUT_S/RESPONSE_TIMEOUT_MS)
// but runs entirely on the shared background task, never blocking
// loop() -- the rest of the chart keeps updating normally throughout.
#ifndef AVG_CONSUMPTION_TASK_H
#define AVG_CONSUMPTION_TASK_H

// Posts a request for InternetPollTask.cpp's shared background task to
// run the up-to-5-day fetch batch at its next opportunity. Call only from
// the main Arduino task (AvgConsumptionRefresh.cpp), and only after
// resolveEnlightenHosts() has already been called on that same thread --
// see the class comment above. Caller must not call this again while
// avgConsumptionAttemptRunning() is still true.
void requestAvgConsumptionFetch();

// Runs the fetch batch if (and only if) requestAvgConsumptionFetch() has
// been called and not yet serviced. Call repeatedly from
// InternetPollTask.cpp's shared background task -- a cheap flag check
// and an immediate return if no request is pending.
void checkAvgConsumptionPoll();

// True from the moment requestAvgConsumptionFetch() is called until
// checkAvgConsumptionPoll() finishes the whole batch (regardless of how
// many of the up-to-5 days succeeded) -- lets AvgConsumptionRefresh.cpp
// know whether its request is still outstanding.
bool avgConsumptionAttemptRunning();

void lockAvgConsumptionProfile();
void unlockAvgConsumptionProfile();

#endif
// ================= end of AvgConsumptionTask.h  file
