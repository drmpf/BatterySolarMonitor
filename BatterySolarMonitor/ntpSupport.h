#ifndef _NTP_H
#define _NTP_H
/*
   ntpSupport.h
   by Matthew Ford,  2021/12/06
   (c)2021 Forward Computing and Control Pty. Ltd.
   NSW, Australia  www.forward.com.au
   This code may be freely used for both private and commerical use.
   Provide this copyright is maintained.
*/
#include <Arduino.h>
void initializeNtpSupport(); // loads and sets tz Must be called by setup()

void processNTP(); // request ntp update at regualar intervals, must be called each loop()

void forceNTPupdate(); // force re-request of time

bool missedSNTPupdate(); // returns true if no update for alst 70 mins
void setTime(long epochSecs, int us); // epochSec is Unix time, Unix time starts on Jan 1 1970.

String getCurrentTZ(); // from env
String getCurrentTZdescription(); // from evn

String getTZstr(); // get the current tz string i.e. timeZoneConfig.tzStr
void resetDefaultTZstr(); // reset tz to default one (from get_ntpSupport_DefaultTZ) is it is defined
String getCurrentTime_hhmm(); // returns local time as hh:mm
String getCurrentTime_hhmmAM_PM(); //return local time as hh:mm AM/PM
String getCurrentDayOfWeek();
String getCurrentDayOfMonth();
String getCurrentMonth();
String getCurrentYear();
String getUTCTime(); // returns UTC time as hh:mm:ss
String getLocalTime_s(); // local time HH:MM:ss in sec

void setTZfromPOSIXstr(const char* tz_str); // sets flag to save config
bool saveTZconfigIfNeeded(); // saves any TZ config changes returns true if save happened

void setNtpSupportDebug(Stream* debugOutPtr); // for debug output

#endif
