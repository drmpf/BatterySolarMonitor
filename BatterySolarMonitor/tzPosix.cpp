/*
   tzPosix.cpp
   by Matthew Ford,  2021/12/06
   (c)2021 Forward Computing and Control Pty. Ltd.
   NSW, Australia  www.forward.com.au
   This code may be freely used for both private and commerical use.
   Provide this copyright is maintained.

   The POSIX tz string parsing is a slight modification of the code from
   https://github.com/ropg/ezTime

   Note: carefully ESP8266 TZ env uses -ve tz offset, i.e. Sydney EST is +10 but TZ str is EST-10....
   ESP8266 does not handle /2 or dst shift spec
   ESP8266 insists on DST name if have dst
*/
#include <Arduino.h>
#include "tzPosix.h"
#include "ntpSupport.h"
#include "SafeString.h"

static Stream* debugPtr = NULL;  // local to this file

// NOTE if start_month = 0 => no dst
//struct posix_tz_data_struct {
//  int offset_min;// = 0; // signed +/-
//  int dst_offset_min;// // signed +/-
//  uint8_t start_month;// = 0, 1 to 12  0 => no dst
//  uint8_t start_week;// = 0,  "5th" week means the last in the mon
//  uint8_t start_dow; // = 0, 0 is Sunday
//  uint8_t start_time_hr; // = 2, //default 2 if not specified
//  uint8_t start_time_min; // = 0;
//  uint8_t end_month; // = 0, 1 to 12
//  uint8_t end_week; // = 0,  "5th" week means the last in the mon
//  uint8_t end_dow; // = 0, 0 is Sunday
//  uint8_t end_time_hr; // = 2, //default 2 if not specified
//  uint8_t end_time_min; // = 0;
//  char tzname[20];
//  char dsttzname[20];
//};

static struct posix_tz_data_struct posixTZ_Data;

void buildPOSIXstr(struct posix_tz_data_struct& posixData, String& result);
void buildPOSIXdescription(struct posix_tz_data_struct& posixData, String& result);

// tz_str is updated with cleaned up tz POSIX string
// tz_str_len is sizeof of tz_str storage
// eg    cleanUpPosixTZStr(timeZoneConfig.tzStr,sizeof(timeZoneConfig.tzStr));
void cleanUpPosixTZStr(char *tz_str, size_t tz_str_len) {
  String tzStr = tz_str;
  cleanUpPosixTZStr(tzStr);
  strlcpy(tz_str, tzStr.c_str(), tz_str_len);
}

void cleanUpPosixTZStr(String& posixTZstr) {
  if (debugPtr) {
    debugPtr->print("cleanUpPosixTZStr:"); debugPtr->println(posixTZstr);
  }
  struct posix_tz_data_struct posixData;
  posixTZDataFromStr(posixTZstr, posixData);
  buildPOSIXstr(posixData, posixTZstr);
}

void printPosixData(struct posix_tz_data_struct& posixData, Stream& out) {
  out.print(" tzname:"); out.print(posixData.tzname);   out.print(" dsttzname:"); out.println(posixData.dsttzname);
  out.print(" offset_min:"); out.println(posixData.offset_min); out.print(" dst_offset_min:"); out.println(posixData.dst_offset_min);
  out.print(" start_month:"); out.print(posixData.start_month); out.print(" start_week:"); out.print(posixData.start_week);  out.print(" start_dow:"); out.print(posixData.start_dow);  out.print(" start_time_hr:"); out.print(posixData.start_time_hr);   out.print(" start_time_min:"); out.println(posixData.start_time_min);
  out.print(" end_month:"); out.print(posixData.end_month);  out.print(" end_week:"); out.print(posixData.end_week); out.print(" end_dow:"); out.print(posixData.end_dow); out.print(" end_time_hr:"); out.print(posixData.end_time_hr);   out.print(" end_time_min:"); out.println(posixData.end_time_min);
}

// set values to zero/defaults, GMT0
void zero_posix_tz_data_struct(struct posix_tz_data_struct& posixData) {
  posixData.offset_min = 0;
  posixData.dst_offset_min = INT_MAX;
  posixData.start_month = 0; //1 to 12  0 => no dst
  posixData.start_week = 0; // correct to 5 if missing, "5th" week means the last in the mon
  posixData.start_dow = 0;// 0 is Sunday
  posixData.start_time_hr = 2; //default 2 if not specified
  posixData.start_time_min = 0; // correct to start_month+6 if missing
  posixData.end_month = 0;// 1 to 12  correct to == start_week if missing
  posixData.end_week = 0;  // correct to 5 if missing, "5th" week means the last in the mon
  posixData.end_dow = 0;// 0 is Sunday
  posixData.end_time_hr = 2; //default 2 if not specified
  posixData.end_time_min = 0;
  posixData.tzname[0] = '\0';
  posixData.dsttzname[0] = '\0';
}

// Only used to set tzoffset from current time
// NO DST here
void setTZoffsetInMins(int min_offset) {
  if (debugPtr) {
    debugPtr->print("setTZoffset:"); debugPtr->println(min_offset);
  }
  int sign = 1;
  if (min_offset < 0) {
    sign = -1;
  }
  min_offset *= sign;
  zero_posix_tz_data_struct(posixTZ_Data); // remove dst
  posixTZ_Data.offset_min = min_offset;
  posixTZ_Data.dst_offset_min = min_offset;
  if (debugPtr) {
    printPosixData(posixTZ_Data, *debugPtr);
  }
  // fix up name
  String result;
  buildPOSIXstr(posixTZ_Data, result); // sets tzname and cleans up struct.
  if (debugPtr) {
    debugPtr->print(result);
  }
  if (debugPtr) {
    printPosixData(posixTZ_Data, *debugPtr);
  }
  //  saveTZstr(result.c_str()); // update file
  //  clearRebootFile();
  //  ESP.restart(); // see https://github.com/esp8266/Arduino/issues/1017  seems to work here
  setTZfromPOSIXstr(result.c_str()); // update envir var
}

// only called with +ve value now
// only works for +ve values
static void print2digits(String &result, int num) {
  if (num < 10) {
    result += '0';
  }
  result += num;
}

// limit mins to +/-16hrs = 960
int cleanMin(int minIn) {
  if (minIn  < -960) {
    return -960;
  }
  if (minIn > 960) {
    return 960;
  }
  return minIn;
}

// used to clean up dst start/end time mins
uint8_t cleanTimeMin(uint minIn) {
  if (minIn > 59) { // unsigned
    minIn = minIn % 60;
  }
  return minIn;
}

// would expect to be 0 to 23 but Jerusalem  == "IST-2IDT,M3.4.4/26,M10.5.0"
// so allow upto 30??
uint8_t cleanHrStart(uint8_t hrIn) {
  if (hrIn > 30) { // unsigned
    hrIn = hrIn % 24; // clean up to 0 to 23
  }
  return hrIn;
}

uint8_t cleanWeek(uint8_t weekIn) {
  if (weekIn == 0) {
    weekIn = 5; // default
  }
  if (weekIn > 5) { // unsigned 0 allowed
    weekIn = weekIn % 5 + 1; // 1 to 5
  }
  return weekIn;
}

uint8_t cleanDay(uint8_t dayIn) {
  if (dayIn > 6) { // unsigned
    dayIn = dayIn % 7;
  }
  return dayIn;
}
uint8_t cleanMonth(uint8_t monthIn) {
  if (monthIn > 12) { // unsigned 0 allowed
    monthIn = monthIn % 12 + 1; // 1 to 12
  }
  return monthIn;
}

// input in mins, output sgn,hr,mm updated via reference
void minsToSignHrMin(int mins, int8_t& sgn, uint8_t& hh, uint8_t& mm) {
  sgn = 1;
  if (mins < 0) {
    sgn = -1;
    mins = -mins;
  }
  hh = mins / 60;
  mm = mins % 60;
}

// appends to String rtn +/-hh:mm in POSIX format
// only show :mm if nonzero
void POSIXTohhmmOffset(int mins, String& rtn) {
  int8_t sgn = 1; uint8_t hh = 0; uint8_t mm = 0;
  minsToSignHrMin(mins, sgn, hh, mm);
  if (sgn < 0) {
    rtn += '-';
  }
  rtn += hh;
  if (mm) {
    rtn += ':';
    print2digits(rtn, mm);
  }
}

// appends to String rtn +/-hhmm in GMT format i.e. leading 0 if <10hrs anD change sign from POSIX tz offset
// only show mm if nonzero
void GMThhmmOffset(int mins, String& rtn) {
  int8_t sgn = 1; uint8_t hh = 0; uint8_t mm = 0;
  minsToSignHrMin(mins, sgn, hh, mm);
  if (sgn >= 0) {
    rtn += '-';
  } else {
    rtn += '+';
  }
  print2digits(rtn, hh);
  if (mm) {
    print2digits(rtn, mm);
  }
}

// use this if just updating offset so that new name generated
void clearTZnames(struct posix_tz_data_struct& posixData) {
  posixData.tzname[0] = '\0';
  posixData.dsttzname[0] = '\0';
}

void cleanUpPosixData(struct posix_tz_data_struct& posixData) {
  // trim names
  cSFA(sfTzname, posixData.tzname);
  sfTzname.trim();
  cSFA(sfDstTzname, posixData.dsttzname);
  sfDstTzname.trim();
  // ESP8266 insists on DST name if have dst  see below
  posixData.offset_min = cleanMin(posixData.offset_min);
  posixData.start_month = cleanMonth(posixData.start_month); //1 to 12  0 => no dst
  if (posixData.start_month == 0) { // no dst
    posixData.dst_offset_min = INT_MAX;
  } else {
    if (posixData.dst_offset_min == INT_MAX) {
      // not set default to offset_min - 60;
      posixData.dst_offset_min = posixData.offset_min - 60;
    }
    posixData.dst_offset_min = cleanMin(posixData.dst_offset_min);
  }
  posixData.start_week = cleanWeek(posixData.start_week); // correct to 5 if missing, "5th" week means the last in the mon
  posixData.start_dow = cleanDay(posixData.start_dow);// 0 is Sunday
  posixData.start_time_hr = cleanHrStart(posixData.start_time_hr); //default 2 if not specified
  posixData.start_time_min = cleanTimeMin(posixData.start_time_min); // correct to start_month+6 if missing
  posixData.end_month = cleanMonth(posixData.end_month);// 1 to 12  correct to == start_mon + 6 if missing
  if ((posixData.start_month) && (posixData.end_month == 0)) {
    posixData.end_month = (posixData.start_month + 6);
    if (posixData.end_month > 12) {
      posixData.end_month -= 12;
    }
  }
  if (posixData.end_week == 0) {
    //set to start
    posixData.end_week = posixData.start_week; // already cleaned up
  }
  posixData.end_dow = cleanDay(posixData.end_dow);// 0 is Sunday
  posixData.end_time_hr = cleanHrStart(posixData.end_time_hr); //default 2 if not specified
  posixData.end_time_min = cleanTimeMin(posixData.end_time_min); // correct to start_month+6 if missing
 // int8_t sgn = 1; uint8_t hr = 0; uint8_t mm = 0;
  if (posixData.tzname[0] == '\0') { // no tz name
    // use <...> name NO does not work  https://github.com/espressif/newlib-esp32/issues/8 need to use GMT dstoffset
    // seems to be fixed now
    if  (posixData.offset_min == 0) {
      // special GMT case
      strlcpy(posixData.tzname, "GMT", sizeof(posixData.tzname));
    } else {
      String tzName;
      tzName += '<';
      GMThhmmOffset(posixData.offset_min, tzName);
      tzName += '>';
      strlcpy(posixData.tzname, tzName.c_str(), sizeof(posixData.tzname));
    }
  }
  if (posixData.dsttzname[0] == '\0') {
    // ESP8266 need dst name if have dst
    if (posixData.start_month) { // have dst
      String dsttzName;
      dsttzName += '<';
      GMThhmmOffset(posixData.dst_offset_min, dsttzName);
      dsttzName += '>';
      strlcpy(posixData.dsttzname, dsttzName.c_str(), sizeof(posixData.dsttzname));
    } else {
      posixData.dsttzname[0] = '\0'; // clear it if not dst
    }
  }
}

static char weekNames[6][5] = {
  "---", "1st", "2nd", "3rd", "4th", "last"
};

void convertWeektoStr(String & result, uint8_t week) {
  if (week > 5) {
    week = 0;
  }
  result += weekNames[week];
}

static char dayNames[7][4] = {
  "Sun", "Mon", "Tue", "Wed", "Thr", "Fri", "Sat"
};
void convertDaytoStr(String & result, uint8_t dayOfWk) {
  if (dayOfWk >= 7) {
    dayOfWk = 0;
  }
  result += dayNames[dayOfWk];
}

static char monthNames[13][4] = {
  "---", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jly", "Aug", "Sep", "Oct", "Nov", "Dec"
};
void convertMonthtoStr(String & result, uint8_t month) {
  if (month > 12) {
    month = 0;
  }
  result += monthNames[month];
}

// human readable description
void getTZDescription(String& posixTZstr, String &result) {
  struct posix_tz_data_struct posixDataLocal;
  posixTZDataFromStr(posixTZstr, posixDataLocal);
  buildPOSIXdescription(posixDataLocal, result);
}

void buildPOSIXdescription(struct posix_tz_data_struct & posixData, String & result) {
  cleanUpPosixData(posixData);
  result = ""; // clear result
  String tznameStr = posixData.tzname;
  if (tznameStr == "UTC") {
    result = "UTC";
    return;
  }
  // else create name GMT +/- offset but change sign
  result = "GMT";
  GMThhmmOffset(posixData.offset_min, result);

  //  result += "\n(Note: POSIX TZ is the -ve of the GMT offset)";
  // finished name.
  if (!posixData.start_month) {
    // no dst rules
    result += "\n No daylight saving rules";
    return;
  }
  result += "\n Daylight Saving ";
  result += "GMT";
  GMThhmmOffset(posixData.dst_offset_min, result);
  result += " starts in the";
  result += "\n"; // 1st .. last week
  convertWeektoStr(result, posixData.start_week);
  result += " week of ";
  convertMonthtoStr(result, posixData.start_month);
  result += " on ";
  convertDaytoStr(result, posixData.start_dow);
  result += " at ";
  print2digits(result, posixData.start_time_hr);
  result += ':';
  print2digits(result, posixData.start_time_min);

  result += "\n Daylight Saving ends in the";
  result += "\n"; // 1st .. last week
  convertWeektoStr(result, posixData.end_week);
  result += " week of ";
  convertMonthtoStr(result, posixData.end_month);
  result += " on ";
  convertDaytoStr(result, posixData.end_dow);
  result += " at ";
  print2digits(result, posixData.end_time_hr);
  result += ':';
  print2digits(result, posixData.end_time_min);
}

// this also cleans up out of range values
void buildPOSIXstr(struct posix_tz_data_struct & posixData, String & result) {
  cleanUpPosixData(posixData);
  result = ""; // clear result
  result += posixData.tzname;
  if (result == "UTC") {
    return;
  }
  POSIXTohhmmOffset(posixData.offset_min, result); // add [-]hr[:mm]
  if (!posixData.start_month) {
    // clear dsttzname
    posixData.dsttzname[0] = '\0';
    return;
  }
  //else  // have dst
  result += posixData.dsttzname;
  POSIXTohhmmOffset(posixData.dst_offset_min, result); // add [-]hr[:mm]

  result += ',';
  result += 'M';
  result +=  posixData.start_month;
  result += '.';
  result +=  posixData.start_week;
  result += '.';
  result +=  posixData.start_dow;
  // add default /2 for clarity
  result += '/';
  result += posixData.start_time_hr;
  if ( posixData.start_time_min) {
    result += ':';
    print2digits(result, posixData.start_time_min);
  }
  result += ',';
  result += 'M';
  result +=  posixData.end_month;
  result += '.';
  result +=  posixData.end_week;
  result += '.';
  result +=  posixData.end_dow;
  // add default /2 for clarity
  result += '/';
  result += posixData.end_time_hr;
  if ( posixData.end_time_min) {
    result += ':';
    print2digits(result, posixData.end_time_min);
  }
}

//time_t Timezone::tzTime(time_t t, ezLocalOrUTC_t local_or_utc, String &tzname, bool &is_dst, int16_t &offset) {

// convert (int)hrOffset : (ont)mmOffset into signed offset_min
int getMinsFromhhmm(int hhOffset, int mmOffset) {
  //  if (debugPtr) {
  //    debugPtr->print("getMinsFromhhmm hhOffset:"); debugPtr->print(hhOffset); debugPtr->print(" mmOffset:"); debugPtr->println(mmOffset);
  //  }
  int rtn = 0;
  if (hhOffset == 0) {
    rtn = mmOffset; // with sign if any
  } else { // take sign from hhOffset
    int sgn = 1;
    if (hhOffset < 0) {
      sgn = -1;
      hhOffset = -hhOffset;
    }
    rtn = hhOffset * 60;
    if (mmOffset < 0) {
      // ignore sign here
      mmOffset = -mmOffset;
    }
    rtn += mmOffset;
    rtn *= sgn; //set sign
  }
  return rtn;
}

void posixTZDataFromStr(String & posixTZstr) { // parses POSIX tz str into its components updates static global posixTZ_Data
  posixTZDataFromStr(posixTZstr, posixTZ_Data);
}

// cleans up data at end
void posixTZDataFromStr(String & posixTZstr, struct posix_tz_data_struct & posixTZData) {
  if (debugPtr) {
    debugPtr->print("posixTZDataFromStr("); debugPtr->print(posixTZstr); debugPtr->println(")");
  }
  String _posix = posixTZstr.c_str();
  _posix.trim();
  String tzname;
 // bool is_dst = false;
 // int16_t offset = 0;

  zero_posix_tz_data_struct(posixTZData);
  if (_posix.length() == 0) {
    return; // use zero data
  }

  if ((_posix[0] == '+') || (_posix[0] == '-') || isDigit(_posix[0]) ) {
    // missing tz name just add space that will be trimmed later
    String tmp = " ";
    tmp += _posix;
    _posix = tmp;
  }

  enum posix_state_e {STD_NAME, OFFSET_HR, OFFSET_MIN, DST_NAME, DST_SHIFT_HR, DST_SHIFT_MIN, START_MONTH, START_WEEK, START_DOW, START_TIME_HR, START_TIME_MIN, END_MONTH, END_WEEK, END_DOW, END_TIME_HR, END_TIME_MIN};
  posix_state_e state = STD_NAME;


  bool ignore_nums = false;
  char c = 1; // Dummy value to get while(newchar) started
  uint8_t strpos = 0;
  uint8_t stdname_end = _posix.length() - 1;
  uint8_t dstname_begin = _posix.length();
  uint8_t dstname_end = _posix.length();
  bool haveDSTname = false;
  int hhOffset = 0;
  int mmOffset = 0;
  bool foundDstOffset = false;
  int hhDstOffset = 0;
  int mmDstOffset = 0;

  while (strpos < _posix.length()) {
    c = (char)_posix[strpos];

    // Do not replace the code below with switch statement: evaluation of state that
    // changes while this runs. (Only works because this state can only go forward.)

    if (c && state == STD_NAME) {
      if (c == '<') {
        ignore_nums = true;
      }
      if (c == '>') {
        ignore_nums = false;
      }
      if (!ignore_nums && (isDigit(c) || c == '-'  || c == '+')) {
        state = OFFSET_HR;
        stdname_end = strpos - 1;
      }
    }
    if (c && state == OFFSET_HR) {
      if (c == '+') {
        // Ignore the plus
      } else if (c == ':') {
        state = OFFSET_MIN;
        c = 0;
      } else if (c != '-' && !isDigit(c)) {
        state = DST_NAME;
        dstname_begin = strpos;
      } else {
        if (hhOffset == 0) {
          hhOffset = atoi(_posix.c_str() + strpos);
          //          if (debugPtr) {
          //            debugPtr->print("hhOffset:"); debugPtr->println(hhOffset);
          //            debugPtr->println(_posix.c_str() + strpos);
          //          }
        }
      }
    }
    if (c && state == OFFSET_MIN) {
      if (!isDigit(c)) {
        state = DST_NAME;
        dstname_begin = strpos;
        ignore_nums = false;
      } else {
        if (mmOffset == 0) {
          mmOffset = atoi(_posix.c_str() + strpos);
          //          if (debugPtr) {
          //            debugPtr->print("mmOffset:"); debugPtr->println(mmOffset);
          //            debugPtr->println(_posix.c_str() + strpos);
          //          }
        }
      }
    }
    if (c && state == DST_NAME) {
      if (c == '<') ignore_nums = true;
      if (c == '>') ignore_nums = false;
      if (c == ',') {
        state = START_MONTH;
        c = 0;
        dstname_end = strpos - 1;
      } else if (!ignore_nums && (c == '-' || isDigit(c))) {
        state = DST_SHIFT_HR;
        dstname_end = strpos - 1;
      }
      haveDSTname = true;
    }
    if (c && state == DST_SHIFT_HR) {
      if (c == ':') {
        state = DST_SHIFT_MIN;
        c = 0;
      } else if (c == ',') {
        state = START_MONTH;
        c = 0;
      } else {
        if (hhDstOffset == 0) {
          foundDstOffset = true;
          hhDstOffset = atoi(_posix.c_str() + strpos);
          //          if (debugPtr) {
          //            debugPtr->print("hhDstOffset:"); debugPtr->println(hhDstOffset);
          //          }
        }
      }
    }
    if (c && state == DST_SHIFT_MIN) {
      if (c == ',') {
        state = START_MONTH;
        c = 0;
      } else {
        if (mmDstOffset == 0) {
          foundDstOffset = true;
          mmDstOffset = atoi(_posix.c_str() + strpos);
          //          if (debugPtr) {
          //            debugPtr->print("mmDstOffset:"); debugPtr->println(mmDstOffset);
          //          }
        }
      }
    }
    if (c && state == START_MONTH) {
      if (c == '.') {
        state = START_WEEK;
        c = 0;
      } else if (c != 'M' && !posixTZData.start_month) posixTZData.start_month = atoi(_posix.c_str() + strpos);
    }
    if (c && state == START_WEEK) {
      if (c == '.') {
        state = START_DOW;
        c = 0;
      } else posixTZData.start_week = c - '0';
    }
    if (c && state == START_DOW) {
      if (c == '/') {
        state = START_TIME_HR;
        c = 0;
      } else if (c == ',') {
        state = END_MONTH;
        c = 0;
      } else posixTZData.start_dow = c - '0';
    }
    if (c && state == START_TIME_HR) {
      if (c == ':') {
        state = START_TIME_MIN;
        c = 0;
      } else if (c == ',') {
        state = END_MONTH;
        c = 0;
      } else if (posixTZData.start_time_hr == 2) posixTZData.start_time_hr = atoi(_posix.c_str() + strpos);
    }
    if (c && state == START_TIME_MIN) {
      if (c == ',') {
        state = END_MONTH;
        c = 0;
      } else if (!posixTZData.start_time_min) posixTZData.start_time_min = atoi(_posix.c_str() + strpos);
    }
    if (c && state == END_MONTH) {
      if (c == '.') {
        state = END_WEEK;
        c = 0;
      } else if (c != 'M') if (!posixTZData.end_month) posixTZData.end_month = atoi(_posix.c_str() + strpos);
    }
    if (c && state == END_WEEK) {
      if (c == '.') {
        state = END_DOW;
        c = 0;
      } else posixTZData.end_week = c - '0';
    }
    if (c && state == END_DOW) {
      if (c == '/') {
        state = END_TIME_HR;
        c = 0;
      } else posixTZData.end_dow = c - '0';
    }
    if (c && state == END_TIME_HR) {
      if (c == ':') {
        state = END_TIME_MIN;
        c = 0;
      }  else if (posixTZData.end_time_hr == 2) posixTZData.end_time_hr = atoi(_posix.c_str() + strpos);
    }
    if (c && state == END_TIME_MIN) {
      if (!posixTZData.end_time_min) posixTZData.end_time_min = atoi(_posix.c_str() + strpos);
    }
    strpos++;
  }
  //  if (debugPtr) {
  //    printPosixData(posixTZData, *debugPtr);
  //  }

  // now fill in offset_min and dst_offset_min
  // take the sign from the hr if non-zero else use mm sign
  posixTZData.offset_min = getMinsFromhhmm(hhOffset, mmOffset); // with sign if any
  if (foundDstOffset) {
    posixTZData.dst_offset_min = getMinsFromhhmm(hhDstOffset, mmDstOffset); // with sign if any
  } // else leave as INT_MAX

  tzname = _posix.substring(0, stdname_end + 1);  // Overwritten with dstname later if needed
  //      tzname = _posix.substring(dstname_begin, dstname_end + 1);
  strlcpy(posixTZData.tzname, tzname.c_str(), sizeof(posixTZData.tzname)); // truncate if >19chars
  if (haveDSTname) {
    String dsttzname = _posix.substring(dstname_begin, dstname_end + 1);
    strlcpy(posixTZData.dsttzname, dsttzname.c_str(), sizeof(posixTZData.dsttzname)); // truncate if >19chars
  }
  if (debugPtr) {
    printPosixData(posixTZData, *debugPtr);
  }
  cleanUpPosixData(posixTZData);
  if (debugPtr) {
    printPosixData(posixTZData, *debugPtr);
  }
}

static bool testParser(String input) {
  String result;
  posixTZDataFromStr(input);
  if (debugPtr) {
    printPosixData(posixTZ_Data, *debugPtr);
  }
  buildPOSIXstr(posixTZ_Data, result);
  if (result != input) {
    if (debugPtr) {
      debugPtr->print(" >>> >>> >>> > missmatch  ");
      debugPtr->println(result);
    }
    return false;
  }
  return true;
}

void testPosix() {
  String result;
  testParser("GMT0");

  testParser("<+01>-1");

  testParser("CET-1CEST,M3.5.0,M10.5.0/3");

  testParser("CST6CDT,M3.2.0,M11.1.0");

  testParser("NST03:30NDT,M3.2.0/0:01,M11.1.0/0:01");

  testParser("<+00>0<+02>-2,M3.5.0/1,M10.5.0/3");
}
