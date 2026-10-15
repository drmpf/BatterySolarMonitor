// CarChargerTask.h  file  =================
// Stage 1 of the load-control feature: periodic poll -- fetches the car
// charger's state every ~5 minutes, stores it (and an accumulated
// today's-kWh estimate, persisted to LittleFS with a rolling 5-day
// history -- see CarChargeHistory.h, added 2026-09-28) for display, and
// logs it either way. No control logic here yet -- that's a later stage.
#ifndef CAR_CHARGER_TASK_H
#define CAR_CHARGER_TASK_H

// Runs fetchCarChargerState() if due and stores the result (success or
// failure) under the lock below -- see CarChargerInstances.h. Call
// repeatedly from InternetPollTask.cpp's shared background task.
void checkCarChargerPoll();

// Forces the NEXT checkCarChargerPoll() to actually fetch in ~20s instead
// of waiting out the rest of the normal 5-min interval -- call this right
// after a toggle (LoadControlTask.cpp) so the real, settled state gets
// confirmed soon, once the relay has had time to complete its transition
// (up to ~8s -- see LoadControlTask.cpp's own toggle-wrapper comment),
// rather than leaving the optimistic post-toggle guess (chargerOn set
// directly, no fetch) standing for up to 5 more minutes.
//
// Priority-gated (see CarChargerTask.cpp's
// armPollTimerIfHigherPriority()): if a LONGER-settling recheck (V2H's own
// 30s below, or the ~60s post-schedule-boundary follow-up) is already
// counting down, this call is a no-op rather than cutting that longer wait
// short -- each of those needs its own full settle time to actually mean
// anything, and this one's 20s is the shortest of the three.
void requestCarChargerRecheck();

// Same idea as requestCarChargerRecheck() above, but with a longer delay
// (~30s) -- specifically for after a V2H recovery
// toggle-on (LoadControlTask.cpp's carCheckStart(), V2H section), which
// takes ~16-20s+ to actually take effect (2x the charger relay's own
// settling time plus a few more seconds before AC actually starts) --
// longer than the charger relay's own settling time requestCarChargerRecheck()
// is tuned for, so this needs its own longer wait before the real
// post-recovery state (does ac240vActive actually come back?) is worth
// checking. Whichever poll actually lands (this recheck, a higher-priority
// one that pre-empted it, or even the plain 5min cadence) unconditionally
// notifies LoadControlTask.cpp of fresh data either way -- see
// checkCarChargerPoll()'s own sync-point comment and
// LoadControlTask.h's requestLoadControlPollNow() (added 2026-09-19).
void requestCarChargerV2hRecheck();

void lockCarChargerState();
void unlockCarChargerState();

#endif
// ================= end of CarChargerTask.h  file
