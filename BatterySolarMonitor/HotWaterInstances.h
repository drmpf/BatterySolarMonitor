// HotWaterInstances.h  file  =================
#ifndef HOT_WATER_INSTANCES_H
#define HOT_WATER_INSTANCES_H

#include "HotWaterFetch.h" // HotWaterState

// Most recent hot water controller reading -- written by HotWaterTask.cpp's
// background poll, read by Dwg_StatusApp.cpp on the main thread. Always
// access under lockHotWaterState()/unlockHotWaterState() (HotWaterTask.h) --
// same pattern as GatewayInstances.h/GatewayPollTask.h.
extern HotWaterState latestHotWaterState;

// True once the most recent poll succeeded -- false before the first poll,
// and after any poll that failed (latestHotWaterState then still holds
// whatever the last SUCCESSFUL poll saw, stale but not garbage -- callers
// decide whether stale-but-reachable-before is worth showing differently
// from never-reached-at-all).
extern bool hotWaterReachable;

// Snapshot of hwAutoStarted() (LoadControlState.h) -- published by
// LoadControlTask.cpp's checkLoadControlPoll() under the SAME lock as the
// fields above, once per control cycle. Exists so the display
// (Dwg_StatusApp.cpp, LoadStatusFormat.cpp) can show who owns the hot
// water element's current on/off state without calling
// LoadControlState.h's unguarded plain-static accessors directly from the
// main thread (they're only ever safe to call from LoadControlTask.cpp's
// own background task).
extern bool hotWaterAutoStarted;

#endif
// ================= end of HotWaterInstances.h  file
