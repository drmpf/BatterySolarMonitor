// HotWaterTask.h  file  =================
// Stage 2 of the load-control feature: periodic connectivity test only --
// fetches and logs the hot water controller's state every ~5 minutes so
// this can be confirmed working before any control logic is built on top
// of it. Mirrors CarChargerTask.h (stage 1) exactly.
#ifndef HOT_WATER_TASK_H
#define HOT_WATER_TASK_H

// Runs fetchHotWaterState() if due and stores the result (success or
// failure) under the lock below -- see HotWaterInstances.h. Call
// repeatedly from InternetPollTask.cpp's shared background task.
void checkHotWaterPoll();

// Forces the NEXT checkHotWaterPoll() to actually fetch in ~20s instead of
// waiting out the rest of the normal 5-min interval -- call this right
// after a toggle (LoadControlTask.cpp) so the real, settled state gets
// confirmed soon, once the element has had time to complete its
// transition (a few seconds -- see LoadControlTask.cpp's own
// toggle-wrapper comment), rather than leaving the optimistic post-toggle
// guess (manualOn set directly, no fetch) standing for up to 5 more
// minutes.
void requestHotWaterRecheck();

void lockHotWaterState();
void unlockHotWaterState();

#endif
// ================= end of HotWaterTask.h  file
