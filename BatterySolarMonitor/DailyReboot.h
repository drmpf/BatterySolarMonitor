// DailyReboot.h  file  =================
// Reboots the ESP32 once, shortly after local midnight each day, purely as
// routine housekeeping (clears any heap fragmentation/small leaks that may
// have accumulated over a full day of WiFi/HTTPS/JSON churn, rather than
// letting them run unbounded across weeks of uptime).
#ifndef DAILY_REBOOT_H
#define DAILY_REBOOT_H

// Call every loop(). Detects the local-time midnight rollover (minutes-
// since-midnight wrapping from a high value back to a low one) and calls
// ESP.restart() the first time it's seen each day. Cheap when nothing is
// due (one int compare). No-op until NTP has synced at least once --
// see missedSNTPupdate() -- so it can never mistake an un-synced clock's
// startup value for a real midnight.
void checkDailyReboot();

#endif
// ================= end of DailyReboot.h  file
