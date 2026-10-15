// HotDayTask.h  file  =================
// Runs on InternetPollTask.cpp's shared background task, right after
// DischargeToGridTask.cpp's own two functions (checkHotDayPoll()/
// checkHotDayLogic() read dtgScheduleStartMinutes()-independent state, but
// DischargeToGridTask.cpp's checkDischargeToGridLogic() reads THIS file's
// own readHotDayForecast() (HotDayState.h) on every tick, so ordering
// between the two task pairs doesn't matter either way -- listed after
// discharge-to-grid in InternetPollTask.cpp purely to keep the two
// "battery auto-control" features' ticks textually adjacent). Added
// 2026-10-13, per explicit instruction, as the "Hot Day" feature: on a
// forecast-hot tomorrow, pre-charge the battery from the grid (Enphase's
// own chargeFromGrid toggle, ChargeFromGridFetch.h) and switch the HW/Car
// start% over to the Hot Day settings (BatteryChargeSettings.h), instead
// of letting tonight's ordinary discharge-to-grid schedule run. Two jobs,
// same shape as DischargeToGridTask.h's own pair:
//   - a recurring ~5min status poll (ChargeFromGridFetch.cpp's
//     fetchChargeFromGridStatus()) that keeps ChargeFromGridState.h's
//     cfgPolledEnabled()/cfgScheduleEndMinutes() current, resends Enphase's
//     own chargeFromGrid setting whenever it doesn't match what
//     HotDayState.h's hotDayActive() says it should be (same "assume
//     success, let the next real poll correct it either way" convention
//     as DischargeToGridTask.cpp's runStatusPoll()), and is also the
//     one-shot "force cfg off now" action's own retry mechanism -- see
//     requestChargeFromGridDisableNow() below.
//   - the Hot Day DECISION itself (checkHotDayLogic() below) -- tests
//     tomorrow's forecast continuously from midnight (HotDayState.h's
//     readHotDayForecast()), but only ACTS (flips hotDayActive(), and so
//     the HW/Car%/chargeFromGrid it implies) once per day, at 2 minutes
//     past the cfg schedule's own configured end time (per explicit
//     instruction: "2mins after the charge from grid schedule ends") --
//     NOT gated to the schedule's end time happening TODAY in the way
//     DischargeToGridTask.cpp's own schedule-start check is; this is a
//     fixed daily trigger, re-armed fresh every day by the
//     tomorrowHotDayDecided latch below being cleared by a plain time
//     comparison (every day before its own trigger time is reached), not
//     by a reboot -- see that flag's own comment (HotDayTask.cpp) for why
//     this one specifically can't rely on the daily reboot the way
//     DischargeToGridTask.cpp's own haveDtgDecision/lastDtgDecision do.
//
// This file never touches discharge-to-grid itself -- the "disable
// discharge to grid (if it is enabled)" half of a hot-day forecast is
// DischargeToGridTask.cpp's own job, via its checkDischargeToGridLogic()
// reading HotDayState.h's readHotDayForecast() directly on every tick
// (continuous, independent of this file's own once-a-day 2:02pm gate).
#ifndef HOT_DAY_TASK_H
#define HOT_DAY_TASK_H

// Call repeatedly from InternetPollTask.cpp's shared background task.
void checkHotDayPoll();

// Arms a one-shot "force Enphase's chargeFromGrid off now" action to run
// at its very next opportunity (the next background-task tick) -- call
// from pfodMainMenuApp.cpp's onAutoBatteryChargeDisPressed() right after
// the master "Auto Battery" switch is turned off. Mirrors
// DischargeToGridTask.h's requestDischargeToGridDisableNow() exactly
// (same one-shot-assumed-success shape, same reliance on the recurring
// status poll above to resend if the first attempt doesn't land).
void requestChargeFromGridDisableNow();

// The Hot Day decision -- call repeatedly from InternetPollTask.cpp's
// shared background task, same as checkHotDayPoll() above (right after
// it, in practice, since this reads that poll's own
// cfgScheduleEndMinutes()). Per explicit instruction:
//   - runs at all only while the master "Auto Battery" switch
//     (DischargeToGridState.h's autoDischargeEnabled()) is enabled --
//     disabled, this returns immediately every call and never acts
//     itself (pfodMainMenuApp.cpp's onAutoBatteryChargeDisPressed() owns
//     the immediate "disable both and revert to default" transition when
//     the switch is first turned off).
//   - before the daily trigger time (2 minutes past
//     ChargeFromGridState.h's cfgScheduleEndMinutes()), does nothing --
//     the forecast test itself runs continuously (see HotDayState.h's
//     readHotDayForecast(), also read by DischargeToGridTask.cpp), but
//     the actual hotDayActive() change only happens once that time is
//     reached, per explicit instruction ("the actual change ... only
//     happens at 2:02pm").
//   - acts at most once per day -- tomorrowHotDayDecided (cleared by a
//     plain time comparison every day before its own trigger time, not
//     by a reboot -- see that flag's own comment, HotDayTask.cpp) stops
//     this from re-deciding on every tick for the rest of the day once
//     today's trigger time has passed.
void checkHotDayLogic();

#endif
// ================= end of HotDayTask.h  file
