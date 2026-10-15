// EnlightenHistoryTask.h  file  =================
// Runs ONE fetchEnlightenHistoryToday() (EnlightenHistoryFetch.h) attempt
// on InternetPollTask.cpp's shared background task, per request --
// EnlightenHistoryRefresh.cpp is the retry loop: it calls
// requestEnlightenHistoryFetch() again (after a delay, and after
// re-resolving candidate IPs on the main thread -- see
// EnlightenHistoryFetch.h's resolveEnlightenHosts(), whose single-IP
// result changes from call to call) each time an attempt fails, since
// lwIP's DNS resolver only ever returns one address per lookup, so a
// single resolveEnlightenHosts() call can't cache multiple candidates to
// fall back through within one attempt -- retrying across attempts, with
// a fresh resolve each time, is what actually finds a working IP.
//
// The fetch itself is safe on any thread (EnlightenHistoryFetch.h), but
// resolveEnlightenHosts() is NOT -- it must always run on the main thread
// that calls requestEnlightenHistoryFetch(), never on InternetPollTask.cpp's
// shared task, which is exactly why this stays a request/poll handshake
// across the two rather than folding the whole retry loop into the
// shared task.
#ifndef ENLIGHTEN_HISTORY_TASK_H
#define ENLIGHTEN_HISTORY_TASK_H

// Posts a request for InternetPollTask.cpp's shared background task to
// run fetchEnlightenHistoryToday() at its next opportunity. Call only
// from the main Arduino task (EnlightenHistoryRefresh.cpp), and only
// after resolveEnlightenHosts() has already been called on that same
// thread -- see the class comment above. Caller must not call this again
// while enlightenHistoryAttemptRunning() is still true.
void requestEnlightenHistoryFetch();

// Runs the fetch if (and only if) requestEnlightenHistoryFetch() has been
// called and not yet serviced. Call repeatedly from
// InternetPollTask.cpp's shared background task -- a cheap flag check
// and an immediate return if no request is pending.
void checkEnlightenHistoryPoll();

// True from the moment requestEnlightenHistoryFetch() is called until
// checkEnlightenHistoryPoll() finishes servicing it (whether the fetch
// succeeded or failed) -- lets EnlightenHistoryRefresh.cpp know whether
// its last request is still outstanding.
bool enlightenHistoryAttemptRunning();

void lockEnlightenHistory();
void unlockEnlightenHistory();

// Returns true (once) if the most recently finished attempt succeeded
// and staged its result into
// enlightenSolarHistory/enlightenConsumptionHistory/enlightenBatteryHistory
// (EnlightenHistoryInstances.h). Only meaningful once
// enlightenHistoryAttemptRunning() has gone false; clears itself once
// read -- check it exactly once per finished attempt (a false result
// then means that attempt failed).
bool enlightenHistoryReady();

#endif
// ================= end of EnlightenHistoryTask.h  file
