// TimestampStream.h  file  =================
// Wraps an OPTIONAL real Stream and prepends "[HH:MM:SS] " (local time) to
// the first byte of every line written through it -- lets every existing
// debugPtr->print()/println() call, across every file in this project,
// gain a timestamp without touching any of those call sites. Install via
// setDebugPtr(&timestampDebugStream) in the .ino, in place of
// setDebugPtr(&Serial).
//
// Takes NO Stream at construction (added 2026-09-12, per explicit
// correction) -- the earlier `TimestampStream timestampDebugStream(Serial);`
// bound a Stream& at GLOBAL-CONSTRUCTION time, well before setup() has
// called Serial.begin(), which is the wrong place to be reaching for
// Serial at all. A bare `static TimestampStream timestampDebugStream;`
// (default constructor, no arguments) is always safe regardless of
// construction order, and still fully usable on its own: every byte
// always reaches DebugLogBuffer.h's rolling buffer + live pfodWeb
// raw-data stream (see below), with or without a real Stream ever being
// set. Call setDebugOut(&Serial) from setup(), AFTER Serial.begin(), if
// mirroring to Serial is also wanted -- setDebugOut(nullptr) turns that
// back off again without losing the DebugLogBuffer.h side, so Serial
// output and the Log Tail buffer/stream are fully independent: either,
// neither, or both.
//
// Before NTP has actually synced at least once, time(nullptr) reads as a
// small near-epoch value -- formatting that as a clock time produced a
// plausible-LOOKING but WRONG timestamp (e.g. "[11:00:12]" when the real
// time was actually hours later), which made an ordinary "NTP hasn't
// synced yet" delay look like a multi-hour device freeze in the log. So
// the timestamp is dropped entirely (no prefix at all) until
// missedSNTPupdate() (ntpSupport.h) confirms at least one real sync has
// landed -- see that function's own comment: false only once a sync has
// actually happened.
//
// write() is called concurrently from multiple FreeRTOS tasks -- every
// background poll task (GatewayPollTask, SolarForecastTask,
// EnlightenHistoryTask, ...) shares this same debug output. A single
// logical debug line is usually built from several
// separate print() calls (e.g. debugPtr->print("x="); debugPtr->print(x);
// debugPtr->println("y");) -- without serializing across the WHOLE line,
// two tasks' lines can interleave mid-line (confirmed in practice: lines
// like "GatewayFetch: solar=..." arriving chopped up with another task's
// text spliced into the middle). So the mutex below is taken on the first
// byte of a new line and only released on that line's own '\n', holding
// it across every print() call in between -- a task starting ITS OWN new
// line while another task's is in progress simply blocks in write() until
// the line ahead of it finishes, rather than interleaving into it.
// localtime_r() (not localtime()) is required for the timestamp itself
// since it's reentrant; localtime()'s shared static buffer would let two
// tasks tear each other's result even with the line-level lock in place.
//
// The lock wait is bounded (1s), not portMAX_DELAY: a task that dies or
// (a bug, like the two already found and fixed in ntpSupport.cpp) never
// writes its line's closing '\n' must not permanently wedge every other
// task's debug output. Timing out just means this line proceeds without
// exclusive access -- a rare possible interleave -- instead of a
// permanent freeze of the whole debug stream.
//
// atLineStart/haveLock are 'thread_local', NOT plain per-instance fields
// -- there is only one TimestampStream instance, shared by every task, so
// a plain bool would be shared mutable state read/written from multiple
// tasks with no protection of its own. That was a real, not hypothetical,
// bug: a task's own byte could see the OTHER task's atLineStart==false
// (line "in progress") and skip taking the mutex entirely, writing
// straight into the middle of that other task's line -- explains garbled
// log lines like "SolarForecast: fetch OSolarForecast: solarData/..." --
// and, worse, a task's own '\n' could then see the OTHER task's
// haveLock==true and call xSemaphoreGive() on a mutex THIS task never
// took, which crashed the device outright ("assert failed:
// xTaskPriorityDisinherit ... pxTCB == pxCurrentTCBs[xPortGetCoreID()]",
// seen in deviceLog.txt right as AvgConsumptionTask.cpp's background-task
// print and AvgConsumptionRefresh.cpp's main-thread print landed at
// nearly the same moment). Each task now tracks its OWN
// "am I mid-line / do I hold the mutex" state, so only the task that
// actually took the mutex can ever give it back, and a genuinely
// different task's line-start byte correctly blocks in xSemaphoreTake()
// instead of writing unprotected.
//
// The mutex is DebugLogBuffer.h's debugLogLineMutex() (moved there
// 2026-09-12 -- kept there since this class is header-only/inline, with
// nowhere of its own for a single shared definition to live). Only this
// class actually takes/gives it now (2026-09-26): debugLogBufferDumpAll()
// no longer needs it -- it just prints a header and resets an int, no
// shared buffer state to protect any more -- see DebugLogBuffer.h's own
// comment for the redesign.
//
// Every byte (timestamp prefix included) is ALSO forwarded to
// DebugLogBuffer.h's debugLogBufferWrite() -- added 2026-09-12, per
// explicit instruction, feeding pfodMainMenuApp.cpp's "Log Tail" screen
// (ESP_PicoW_pfodWebServer.h's own raw-data relay, straight through, no
// buffering of its own -- see DebugLogBuffer.h). This happens
// UNCONDITIONALLY, regardless of whether a real Stream is currently set
// via setDebugOut() below -- turning Serial off must not turn the Log
// Tail screen off too; the two are independent, and either or both can
// be on.
#ifndef TIMESTAMP_STREAM_H
#define TIMESTAMP_STREAM_H

#include <Arduino.h>
#include <time.h>
#include "ntpSupport.h"
#include "DebugLogBuffer.h"

class TimestampStream : public Stream {
  public:
    TimestampStream() {}

    // Points this stream's OPTIONAL real output at 'out' -- call from
    // setup(), AFTER Serial.begin() (or whatever real stream is being
    // used), not at global-construction time -- see this class's own top
    // comment for why. Safe to call again later to switch to a different
    // real stream. Pass nullptr to turn the real-stream side back off --
    // write()/writeTimestamp() then only reach DebugLogBuffer.h, same as
    // before setDebugOut() was ever called; the Log Tail buffer/stream
    // itself is never affected either way.
    void setDebugOut(Stream *out) { realStream = out; }

    size_t write(uint8_t c) override {
      if (atLineStart) {
        // Held until this same line's closing '\n' below -- see the
        // class comment for why this spans multiple write() calls, and
        // why the wait is bounded rather than portMAX_DELAY.
        haveLock = (xSemaphoreTake(debugLogLineMutex(), pdMS_TO_TICKS(1000)) == pdTRUE);
        writeTimestamp();
        atLineStart = false;
      }
      // Report success (1) even with no real stream set, matching what a
      // real write() would return -- callers must never see a "0 bytes
      // written" just because setDebugOut() was never called (or was
      // passed nullptr).
      size_t n = realStream ? realStream->write(c) : 1;
      debugLogBufferWrite(c); // ALWAYS -- see this class's own top comment
      if (c == '\n') {
        atLineStart = true;
        if (haveLock) {
          xSemaphoreGive(debugLogLineMutex());
          haveLock = false;
        }
      }
      return n;
    }

    int available() override { return realStream ? realStream->available() : 0; }
    int read() override { return realStream ? realStream->read() : -1; }
    int peek() override { return realStream ? realStream->peek() : -1; }
    void flush() override { if (realStream) { realStream->flush(); } }

  private:
    Stream *realStream = nullptr; // NOT a reference -- see setDebugOut() above
    inline static thread_local bool atLineStart = true;
    inline static thread_local bool haveLock = false; // did THIS line's start actually acquire the mutex -- see write()

    // The line mutex itself lives in DebugLogBuffer.h (debugLogLineMutex(),
    // created on first use, not at global-construction time) since this
    // class has nowhere of its own for a single shared definition to live
    // (header-only/inline) -- see that file's own comment on the mutex.

    void writeTimestamp() {
      if (missedSNTPupdate()) {
        return; // no real sync yet -- no prefix, rather than a misleading clock reading
      }
      time_t now = time(nullptr);
      struct tm tmNow;
      localtime_r(&now, &tmNow);
      char buf[12]; // "[HH:MM:SS] " + NUL
      snprintf(buf, sizeof(buf), "[%02d:%02d:%02d] ", tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
      if (realStream) {
        realStream->print(buf);
      }
      for (const char *p = buf; *p; p++) {
        debugLogBufferWrite((uint8_t)*p); // capture the timestamp prefix too, ALWAYS -- see this class's own top comment
      }
    }
};

#endif
// ================= end of TimestampStream.h  file
