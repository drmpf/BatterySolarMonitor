// DebugLogBuffer.h  file  =================
// Feeds every byte written through TimestampStream.h (i.e. everything any
// debugPtr->print()/println() call anywhere in this project writes,
// timestamp prefix included -- see that file, ALWAYS forwarded regardless
// of whether a real Stream is currently set via its own setDebugOut())
// straight into the pfodWeb "Raw Data" mechanism (ESP_PicoW_pfodWebServer.h's
// getRawDataWriter()) -- added 2026-09-12 for pfodMainMenuApp.cpp's "Log
// Tail" button, redesigned 2026-09-26 (see below) to write straight
// through with no buffer/pump of its own.
//
// PRE-WEBSERVER-STARTUP CAPTURE (added 2026-09-28): getRawDataWriter()'s
// own writeHttpRawData() (ESP_PicoW_pfodWebServer.cpp) silently drops
// every byte written before start_pfodWebServer() has run and allocated
// its backing buffer -- confirmed by direct report: BatterySolarMonitor.ino
// calls loadEnphaseAuth()/loadSiteConfig() well before
// start_pfodWebServer(), so neither function's own debug output (in
// particular their "loaded X/N keys"/unexpected-extra-key WARNING
// messages) ever reached a real Log Tail capture, only the physical
// Serial console. A small pfodCircularLineBuffer (preWebServerBuffer, see
// the .cpp) now captures every byte written before that point instead of
// letting it vanish; notifyWebServerStarted() (called once, right after
// start_pfodWebServer() in setup()) latches debugLogBufferWrite() over to
// the now-valid httpRawData relay for everything from that point on.
//
// RE-SHOWN ON EVERY LOG TAIL OPEN, NOT JUST ONCE (changed 2026-10-09, per
// explicit instruction): debugLogBufferDumpAll() below re-prints
// preWebServerBuffer's own captured text itself, every time Log Tail is
// opened, instead of (as an earlier version of this file did) flushing it
// into httpRawData just once at boot and relying on it still being there.
// This device reboots once a day at midnight (DailyReboot.h), so by the
// time anyone actually checks Log Tail -- typically hours later, e.g. the
// following morning -- ordinary logging has almost certainly pushed that
// one-time flush out of httpRawData's own 16K window through perfectly
// normal eviction, long before any client's own cursor ever reaches it.
// preWebServerBuffer's content is never evicted (nothing writes into it
// again once notifyWebServerStarted() latches), so re-reading it fresh on
// every open always reproduces the same boot-time text regardless of how
// long ago that boot actually was.
//
// HISTORY (kept for context; none of the rest of this applies any more):
// this file used to keep its own separate 16K pfodCircularLineBuffer
// (debugLogBuffer), fed by every debug byte, with a paced pump
// (checkDebugLogBufferDumpPump(), called every main-thread loop()) copying
// from it into ESP_PicoW_pfodWebServer.cpp's own 4K httpRawData relay a
// few hundred bytes at a time -- needed back then because httpRawData was
// too small to hold a 16K backlog in one go (dumping it in one go silently
// truncated to the last ~4K; writing straight to the HTTP reply's own
// String instead fragmented memory, ~12000 individual reallocations).
// Fixed 2026-09-26, per explicit instruction, at the actual source of that
// mismatch instead of working around it here: httpRawData's size is now a
// start_pfodWebServer() argument (this project passes 16384, matching what
// this file used to allocate itself), and ESP_PicoW_pfodWebServer.cpp
// gives every polling client (by IP) its own read cursor into it instead
// of one shared, destructively-cleared position -- see that file's own
// comment for the multi-client bug that fixes. With httpRawData big enough
// to hold the full backlog and each client tracking its own progress
// through it, there is nothing left for a second, separate capture buffer
// or a pump to do: debugLogBufferWrite() just writes straight through, and
// "dump the last 16K again" is exactly resetCurrentRawDataCursor() --
// resetting the CALLING client's own cursor, not touching anything shared.
// (The pre-webserver-startup buffer above is a NEW, different, much
// smaller capture, for a narrow window nothing in the 2026-09-26 redesign
// was ever meant to cover -- not a partial revival of the old design.)
#ifndef DEBUG_LOG_BUFFER_H
#define DEBUG_LOG_BUFFER_H

#include <Arduino.h>

// The mutex that serialises whole debug lines across tasks -- taken by
// TimestampStream::write() on a line's first byte and given back on that
// line's '\n' (see that file's class comment for why a WHOLE line, not
// just each byte, needs to be atomic against other tasks' lines). Lives
// here (not in TimestampStream.h, which is header-only/inline, with
// nowhere of its own for a single shared definition to live) rather than
// for any reason specific to this file any more -- debugLogBufferWrite()
// below is called from WITHIN that already-held lock, so it doesn't take
// it itself; ESP_PicoW_pfodWebServer.cpp's own writeHttpRawData() has its
// own SEPARATE internal lock protecting httpRawData's own read/write
// access (a different concern -- buffer-pointer consistency, not line
// text ordering). Created on first use, not at global-construction time.
SemaphoreHandle_t debugLogLineMutex();

// Call once per byte, from TimestampStream::write() -- forwards to
// getRawDataWriter() (ESP_PicoW_pfodWebServer.h) once notifyWebServerStarted()
// has run, or to the small pre-webserver-startup capture buffer before
// that (see this file's own top comment). No locking of its own either
// way: the caller already holds debugLogLineMutex() for the whole line,
// and getRawDataWriter()'s own write() is separately lock-protected
// (against handle_pfodWebServer()'s own task) inside the library itself.
void debugLogBufferWrite(uint8_t c);

// Call once, immediately after start_pfodWebServer() in setup() (see this
// file's own top comment for why). Latches debugLogBufferWrite() over to
// the now-valid httpRawData relay for everything written from this point
// on -- no longer flushes preWebServerBuffer anywhere itself (see this
// file's own "RE-SHOWN ON EVERY LOG TAIL OPEN" comment above --
// debugLogBufferDumpAll() does that instead, fresh, every time). Safe to
// call more than once (idempotent).
void notifyWebServerStarted();

// Opens the Log Tail raw-data screen: prints `openScreenMsg` (the
// {=Last 16K of Debug Log~C} response) to `out` -- the pfodParser handling
// the button press, which is a Print -- then prints, in order: the last 5
// days' car charging kWh plus today's running total (CarChargeHistory.h's
// printCarChargeHistory(), added 2026-09-28 per explicit instruction);
// then the captured startup log (preWebServerBuffer, re-printed fresh
// every time this is called -- see this file's own top comment for why);
// then calls resetCurrentRawDataCursor() (ESP_PicoW_pfodWebServer.h) so
// THIS client's own read cursor into httpRawData restarts from the oldest
// data still available there, i.e. up to the full backlog httpRawData
// currently holds -- matching the screen's own title ("Last 16K of Debug
// Log") however long the screen's been closed. Affects only the client
// that pressed the button; every other currently-polling client's own
// cursor is untouched, so this can never cause another viewer to lose
// data (the bug the old shared-buffer design had).
// Called from pfodMainMenuApp.cpp's MyMainMenu::onLogTailPressed()
// override (pfodMainMenu.cpp itself is generated code and never edited
// directly, see that file's own comment).
void debugLogBufferDumpAll(Print &out, const __FlashStringHelper *openScreenMsg);

#endif
// ================= end of DebugLogBuffer.h  file
