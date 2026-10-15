// InternetPollTask.cpp  file  =================
// Every internet-facing poll in this project (GatewayPollTask.h's gateway
// status, SolarForecastTask.h's solar+weather forecast,
// EnlightenHistoryTask.h's history backfill) used to run on its own
// separate FreeRTOS task. That meant multiple independent
// WiFiClientSecure connections could be open/opening at genuinely the
// same moment -- fine most of the time, but GatewayFetch.cpp's own
// /ivp/ensemble/power bug (two WiFiClientSecure objects coexisting WITHIN
// one function, the outer one not yet destructed when the inner one
// opened) showed just how sensitive this local gateway's minimal embedded
// HTTPS server is to overlapping connections, and there's no way to rule
// out something similar happening ACROSS tasks too.
//
// Running every poll from ONE shared task instead guarantees only one
// internet-facing connection is ever open at a time, full stop -- no
// cross-task overlap is even possible, so the old boot-time startup
// staggers (5s/10s delays in SolarForecastTask.cpp/the now-removed
// WeatherForecastTask.cpp) are gone too; they're no longer needed. The
// cost: a slow/stuck poll now delays the others queued behind it in the
// same round, but every poll is already bounded (~13s worst case: 5s TLS
// handshake + 8s response read, see each XFetch.cpp's
// HANDSHAKE_TIMEOUT_S/RESPONSE_TIMEOUT_MS) and none of these run more
// often than every 5 minutes, so a worst-case serialized delay across all
// of them is a non-issue at this timescale.
//
// Each job still owns its own "is it due yet" state and interval,
// unchanged from when it had its own task -- see checkGatewayPoll()
// (GatewayPollTask.cpp), checkSolarForecastPoll() (SolarForecastTask.cpp --
// since 2026-09-11 this one fetch also covers the daily weather forecast,
// see that file), and checkEnlightenHistoryPoll()/checkAvgConsumptionPoll()
// (EnlightenHistoryTask.cpp/AvgConsumptionTask.cpp, each serviced only
// when its own main-thread state machine -- EnlightenHistoryRefresh.cpp/
// AvgConsumptionRefresh.cpp -- has posted a request via
// requestEnlightenHistoryFetch()/requestAvgConsumptionFetch() -- see
// those files for why the DNS resolve behind them can never move to this
// task). This file just ticks all these in turn.
#include "InternetPollTask.h"
#include "GatewayPollTask.h"
#include "SolarForecastTask.h"
#include "EnlightenHistoryTask.h"
#include "AvgConsumptionTask.h"
#include "EnlightenLoginFetch.h"
#include "CarChargerTask.h"
#include "HotWaterTask.h"
#include "LoadControlTask.h"
#include "DischargeToGridTask.h"
#include "HotDayTask.h"
#include <Arduino.h>

// How often to check whether any job is due. Cheap (a handful of
// millis() comparisons, plus one flag check for Enlighten) compared to
// the 5-30 minute intervals being checked, so a short, simple fixed tick
// is used rather than computing the exact time until the next job's own
// due time.
static const unsigned long TICK_MS = 1000;

static void internetPollTaskFn(void *pvParameters) {
  (void)pvParameters;
  for (;;) {
    checkGatewayPoll();
    checkSolarForecastPoll();
    checkEnlightenHistoryPoll();
    checkAvgConsumptionPoll();
    checkEnlightenLoginPoll(); // self-refreshes gatewayJwt()/enlightenSession() once per boot -- see EnlightenLoginFetch.h
    checkCarChargerPoll();
    checkHotWaterPoll();
    checkLoadControlPoll(); // reads the two polls just above -- see LoadControlTask.cpp
    checkDischargeToGridPoll(); // Enphase discharge-to-grid boot-disable + 5min status poll -- see DischargeToGridTask.h
    checkDischargeToGridLogic(); // auto-discharge decision -- reads the schedule start time checkDischargeToGridPoll() just published, so runs right after it; also reads HotDayState.h's own forecast test, see that function's own comment
    checkHotDayPoll(); // Enphase charge-from-grid status poll + resend-on-mismatch -- see HotDayTask.h
    checkHotDayLogic(); // Hot Day decision (2:02pm trigger) -- reads the schedule end time checkHotDayPoll() just published, so runs right after it
    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
  }
}

void startInternetPollTask() {
  xTaskCreatePinnedToCore(
      internetPollTaskFn,
      "InternetPollTask",
      8192,   // stack -- HTTPS (mbedTLS) + JSON parsing both need headroom;
              // jobs run one at a time (never concurrently), so this only
              // needs to cover the largest single job, not their sum
      NULL,
      1,      // low priority -- background work, never urgent
      NULL,   // no task handle needed
      0       // core 0 -- leave core 1 free for loop()/the pfod connection
  );
}
// ================= end of InternetPollTask.cpp  file
