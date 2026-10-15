#ifndef _TZ_POSIX_H
#define _TZ_POSIX_H
/*   
   tzPosix.h
   by Matthew Ford,  2021/12/06
   (c)2021 Forward Computing and Control Pty. Ltd.
   NSW, Australia  www.forward.com.au
   This code may be freely used for both private and commerical use.
   Provide this copyright is maintained.

   The POSIX tz string parsing is a slight modification of the code from
   https://github.com/ropg/ezTime  
*/

#include "limits.h"

// NOTE if start_month = 0 => no dst
struct posix_tz_data_struct {
  int offset_min;// = 0; // signed +/-
  int dst_offset_min;// // signed +/-  set to INT_MAX if not set/parsed
  uint8_t start_month;// = 0, 1 to 12  0 => no dst
  uint8_t start_week;// = 0,  "5th" week means the last in the mon
  uint8_t start_dow; // = 0, 0 is Sunday
  uint8_t start_time_hr; // = 2, //default 2 if not specified
  uint8_t start_time_min; // = 0;
  uint8_t end_month; // = 0, 1 to 12
  uint8_t end_week; // = 0,  "5th" week means the last in the mon
  uint8_t end_dow; // = 0, 0 is Sunday
  uint8_t end_time_hr; // = 2, //default 2 if not specified
  uint8_t end_time_min; // = 0;
  char tzname[20];
  char dsttzname[20];
};

void setTZoffsetInMins(int min_offset);
// use this if just updating TZ offset so that new name generated
void clearTZnames(struct posix_tz_data_struct& posixData);

void getTZDescription(String& posixTZstr, String &result); // human readable description

void buildPOSIXstr(struct posix_tz_data_struct& posixData, String& result);
void posixTZDataFromStr(String& posixTZstr); // parses POSIX tz str into its components updates static global posixTZ_Data
void posixTZDataFromStr(String& posixTZstr, struct posix_tz_data_struct& posixData);
void buildPOSIXdescription(struct posix_tz_data_struct& posixData, String& result); // human readable description

void cleanUpPosixTZStr(char *tz_str, size_t tz_str_len); // tz_str_len is sizeof of tz_str storage, e.g.  cleanUpPosixTZStr(timeZoneConfig.tzStr,sizeof(timeZoneConfig.tzStr));
void cleanUpPosixTZStr(String& posixTZstr);

void testPosix(); // tests

#endif
