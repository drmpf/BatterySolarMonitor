/*
   ntpSupport.cpp
   by Matthew Ford,  2021/12/06
   (c)2021 Forward Computing and Control Pty. Ltd.
   NSW, Australia  www.forward.com.au
   This code may be freely used for both private and commerical use.
   Provide this copyright is maintained.
*/

// this code based on https://www.arduino.cc/en/Tutorial/LibraryExamples/UdpNtpClient

//   Note: carefully ESP8266 TZ env uses -ve tz offset, i.e. Sydney EST is +10 but TZ str is EST-10....

// timezone offsets range from  UTC−12:00 to UTC+14:00 in down to 15min segments
// so just use current time to set offset in range -12 to +12 this will be a day off for those tz that are +13 and +14
// +hhmm are SUBTRACTED!! fromo UTC to get local time so take UTC and subtract user's Current Time to get TZ envirmental variable tzoffset rounded to 15mins
// offsets in range -12 < offset <= +12  i.e. -11:45 is the smallest offset and +12:00 is the largest
// e.g. UTC  = 14:00,  LC (localTime) UTC=04:00  tzoffset = +10:00
//      UTC = 08:00  LC = 22:00  tzoffset =  -14 => <=-12 so add 24,  -14+24 = +10
//      UTC = 14:00  LC = 22:00  tzoffset = -8:00
//      UTC = 20:00  LC = 4:00   tzoffset = 16:00 => >12 so subtract 24,  16-24 = -8:00

#include "ntpSupport.h"
#include "tzPosix.h"
#include <millisDelay.h>
#include <time.h>                       // time() ctime()
#include <sys/time.h>                   // struct timeval
#include "millisDelay.h"
#include <ESP_PicoW_LittleFSsupport.h>

#if defined (ESP32)
#include <WiFi.h>
#elif defined (ESP8266)
#include <ESP8266WiFi.h>
#else
#endif

#include <WiFiUdp.h>
// A UDP instance to let us send and receive packets over UDP
static WiFiUDP udp;
static bool udpRunning = false;
static unsigned int localPort = 8888;       // local port to listen for UDP packets
static const char timeServer[] = "pool.ntp.org";//"time.nist.gov"; // time.nist.gov NTP server
static const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
static byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
static millisDelay updateTimer; // time between update requests initially 20sec and then 60min get time from ESP_sntp_update_delay_MS_rfc_not_less_than_15000()
static millisDelay responseTimer; // wait upto 10sec for a response if this is running then a request has been made.
static const unsigned long UDP_ResponseTime = 10ul * 1000; // 10sec
static const unsigned long responseTimer_ms = 500; // 0.5sec
static const unsigned int MAX_ResponseCounter = UDP_ResponseTime / responseTimer_ms; // count in 0.5sec intervals
static unsigned int responseCounter = 0;

static void sendNTPpacket(const char * address);

// define a weak getDefaultTZ method that can be defined elsewhere if you want to set a default TZ
const char* get_ntpSupport_DefaultTZ() __attribute__((weak));

const char* get_ntpSupport_DefaultTZ() {
  return "AEST-10AEDT,M10.1.0,M4.1.0/3"; // see https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv 
}

static bool ntpSupportInitialized = false;

static Stream* debugPtr = NULL;  // local to this file

void setNtpSupportDebug(Stream* debugOutPtr) {
  debugPtr = debugOutPtr;
}


//struct timeval {
//  long    tv_sec;         /* seconds */
//  long    tv_usec;        /* and microseconds */
//};
// time_t is an intergal type that holds number of seconds elapsed since 00:00 hours, Jan 1, 1970 UTC (i.e., a unix timestamp).
// nullptr is a C++ null pointer literal which you can use anywhere you need to pass a null pointer.

//struct tm;
//Defined in header <time.h>
//Structure holding a calendar date and time broken down into its components.
//Member objects
//int tm_sec  seconds after the minute – [0, 61] (until C99)[0, 60] (since C99)[for leap second]
//int tm_min minutes after the hour – [0, 59]
//int tm_hour hours since midnight – [0, 23]
//int tm_mday day of the month – [1, 31]
//int tm_mon months since January – [0, 11]
//int tm_year years since 1900
//int tm_wday days since Sunday – [0, 6]
//int tm_yday days since January 1 – [0, 365]
//int tm_isdst Daylight Saving Time flag. The value is positive if DST is in effect, zero if not and negative if no information is available
//
//The Standard mandates only the presence of the aforementioned members in either order.
//The implementations usually add more data-members to this structure.

//static timeval tv;
//static timespec tp;
//static time_t now;

static millisDelay ntpUpdateCheck;
static const unsigned long NORMAL_NTP_UPDATE_INTERVAL = (60UL * 60) * 1000; // 60mins (60 * 60 sec * 1000 ms)
static const unsigned long NTP_NOT_UPDATED_MS = NORMAL_NTP_UPDATE_INTERVAL + (10ul * 60 * 1000); //70mins total
static bool needToSaveConfigFlag = false;

static void showTimeDebug();

static const char timeZoneConfigFileName[] = "/timeZoneCfg.bin";  // binary file

extern "C" int clock_gettime(clockid_t unused, struct timespec *tp);
static bool saveTimeZoneConfig(struct timeZoneConfig_struct& timeZoneConfig);

static const char* daysOfWeek_short[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *monthOfYear_short[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char* daysOfWeek[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *monthOfYear[12] = {"January", "Febuary", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

struct timeZoneConfig_struct {
  time_t utcTime; // sec since 1/1/1970  = if not yet set by SNTP or timezonedb.com
  // POSIX tz str
  char tzStr[50]; // eg AEST-10AEDT,M10.1.0,M4.1.0/3  if empty then skip setting tzStr and just use user set local time and sntp utc to calculate tz offset
};
static struct timeZoneConfig_struct timeZoneConfig;

static bool haveSNTPresponse = false; // set true on first response
static bool haveSecondSNTPresponse = false; // got second one
static bool haveSNTPupdate = false;

//static unsigned int getLocalTime_mins(); // hh:mm in mins

/** returns true if config saved */
bool saveTZconfigIfNeeded() { // saves any TZ config changes
  bool rtn = false;
  if (needToSaveConfigFlag) {
    saveTimeZoneConfig(timeZoneConfig);
    rtn = true;
    needToSaveConfigFlag = false;
  }
  return rtn;
}


static void setUTCconfigTime() {
  // avoid var not referenced warnings
  (void)(daysOfWeek_short);
  (void)(monthOfYear_short);
  (void)(daysOfWeek);
  (void)(monthOfYear);
  time_t now = time(nullptr);
  timeZoneConfig.utcTime = now;
};

#if defined (ESP32)
extern "C" void tzset(); // esp32
// for esp32 add this
static void ESP_setTZ(const char* tz_str) {
  setenv("TZ", tz_str, 1);
  tzset();
}
#elif defined (ESP8266)
static void ESP_setTZ(const char* tz_str) {
  setTZ(tz_str);
}
#else
#endif

void setTime(long epochSecs, int us) {
  struct timeval tv;
  tv.tv_sec = epochSecs;  // epoch time (seconds)
  tv.tv_usec = us;    // microseconds
  settimeofday(&tv, NULL);
}


// send an NTP request to the time server at the given address
static void sendNTPpacket(const char * address) {
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  udp.beginPacket(address, 123); // NTP requests are to port 123
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
  if (debugPtr) {
    debugPtr->println("Sent NTP UDP request");
  }
}


// call cleanUpfirst
void setTZfromPOSIXstr(const char* tz_str) {
  time_t now = time(nullptr);
  timeZoneConfig.utcTime = now;
  strlcpy(timeZoneConfig.tzStr, tz_str, sizeof(timeZoneConfig.tzStr));
  if (debugPtr) {
    debugPtr->print("setTZfromPOSIXstr:"); debugPtr->println(timeZoneConfig.tzStr);
  }
  ESP_setTZ(tz_str); // ESP8266 has this method
  needToSaveConfigFlag = true;
}



static uint32_t ESP_sntp_update_delay_MS_rfc_not_less_than_15000() {
  if (haveSecondSNTPresponse) {
    return NORMAL_NTP_UPDATE_INTERVAL; // 60mins
  } else {
    return 20000; // 20 sec
  }
}

// used when timeZoneConfigFileName file does not exist or is invalid
static void setInitialTimeZoneConfig() {
  timeZoneConfig.utcTime = 0;
  timeZoneConfig.tzStr[0] = '\0'; // => GMT0 after cleanup
  strlcpy(timeZoneConfig.tzStr, get_ntpSupport_DefaultTZ(), sizeof(timeZoneConfig.tzStr));
  // clean up
  cleanUpPosixTZStr(timeZoneConfig.tzStr, sizeof(timeZoneConfig.tzStr));
}

void resetDefaultTZstr() {
  char tzStr[sizeof(timeZoneConfig.tzStr)];
  tzStr[0] = '\0';
  strlcpy(tzStr, get_ntpSupport_DefaultTZ(), sizeof(tzStr));
  setTZfromPOSIXstr(tzStr); // cleans up and set save flag as well
  saveTZconfigIfNeeded(); // force save
}

String getTZstr() {
  return String(timeZoneConfig.tzStr);
}

static void printTimeZoneConfig(struct timeZoneConfig_struct & timeZoneConfig, Stream & out) {
  out.print("utcTime:");
  out.println(timeZoneConfig.utcTime);
  out.print("tzStr:");
  out.println(timeZoneConfig.tzStr);
}


// load the last time saved before shutdown/reboot
// returns pointer to timeZoneConfig
static struct timeZoneConfig_struct* loadTimeZoneConfig() {
  setInitialTimeZoneConfig();
  if (!initializeFS()) {
    if (debugPtr) {
      debugPtr->println("FS failed to initialize");
    }
    return &timeZoneConfig; // returns default if cannot open FS
  }
  if (!LittleFS.exists(timeZoneConfigFileName)) {
    if (debugPtr) {
      debugPtr->print(timeZoneConfigFileName); debugPtr->println(" missing.");
    }
    saveTimeZoneConfig(timeZoneConfig);
    return &timeZoneConfig; // returns default if missing
  }
  // else load config
  File f = LittleFS.open(timeZoneConfigFileName, "r");
  if (!f) {
    if (debugPtr) {
      debugPtr->print(timeZoneConfigFileName); debugPtr->println(" did not open for read.");
    }
    LittleFS.remove(timeZoneConfigFileName);
    saveTimeZoneConfig(timeZoneConfig);
    return &timeZoneConfig; // returns default wrong size
  }
  if (f.size() != sizeof(timeZoneConfig)) {
    if (debugPtr) {
      debugPtr->print(timeZoneConfigFileName); debugPtr->println(" wrong size.");
    }
    f.close();
    saveTimeZoneConfig(timeZoneConfig);
    return &timeZoneConfig; // returns default wrong size
  }
  int bytesIn = f.read((uint8_t*)(&timeZoneConfig), sizeof(timeZoneConfig));
  if (bytesIn != sizeof(timeZoneConfig)) {
    if (debugPtr) {
      debugPtr->print(timeZoneConfigFileName); debugPtr->println(" wrong size read in.");
    }
    setInitialTimeZoneConfig(); // again
    f.close();
    saveTimeZoneConfig(timeZoneConfig);
    return &timeZoneConfig;
  }
  f.close();
  // else return settings
  // clean up tz and return
  cleanUpPosixTZStr(timeZoneConfig.tzStr, sizeof(timeZoneConfig.tzStr));
  if (debugPtr) {
    debugPtr->println("Loaded config");
    printTimeZoneConfig(timeZoneConfig, *debugPtr);
  }
  if (debugPtr) {
    String desc = timeZoneConfig.tzStr;
    struct posix_tz_data_struct posixTz;
    posixTZDataFromStr(desc, posixTz);
    buildPOSIXdescription(posixTz, desc);
    debugPtr->println("TZ description");
    debugPtr->println(desc);
  }

  return &timeZoneConfig;
}

// load the last time saved before shutdown/reboot
static bool saveTimeZoneConfig(struct timeZoneConfig_struct & timeZoneConfig) {
  if (!initializeFS()) {
    if (debugPtr) {
      debugPtr->println("FS failed to initialize");
    }
    return false;
  }
  // else save config
  File f = LittleFS.open(timeZoneConfigFileName, "w"); // create/overwrite
  if (!f) {
    if (debugPtr) {
      debugPtr->print(timeZoneConfigFileName); debugPtr->println(" did not open for write.");
    }
    return false; // returns default wrong size
  }
  setUTCconfigTime(); // update utc time
  int bytesOut = f.write((uint8_t*)(&timeZoneConfig), sizeof(struct timeZoneConfig_struct));
  if (bytesOut != sizeof(struct timeZoneConfig_struct)) {
    if (debugPtr) {
      debugPtr->print(timeZoneConfigFileName); debugPtr->println(" write failed.");
    }
    return false;
  }
  // else return settings
  f.close(); // no rturn
  if (debugPtr) {
    debugPtr->print(timeZoneConfigFileName); debugPtr->println(" config saved.");
    printTimeZoneConfig(timeZoneConfig, *debugPtr);
  }
  return true;
}

//static void printLocalTime(Stream &out) {
//  struct tm timeinfo;
//  if (!getLocalTime(&timeinfo)) {
//    out.println("No time available (yet)");
//    return;
//  }
//  out.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
//}

/**
   return false if missed sntp update and timer timed out.
*/
bool missedSNTPupdate() {
  if (ntpUpdateCheck.justFinished()) {
    haveSNTPupdate = false;
  }
  return !haveSNTPupdate;
}

// start sntp server and updates time and then stops server
void initializeNtpSupport() {
  if (ntpSupportInitialized) {
    return;
  }
  ntpSupportInitialized = true;
  if (debugPtr) {
    debugPtr->print("initializeNtpSupport"); debugPtr->println();
  }

  // Settle delay: calling settimeofday()/opening a UDP socket immediately
  // after WiFi.status()==WL_CONNECTED -- while lwIP's own connection-event
  // handling may still be mid-flight -- has been observed to trigger
  // "assert failed: udp_remove ... Required to lock TCPIP core
  // functionality!" on this core version. A short delay here lets that
  // settle before this file touches the network stack or the system clock.
  delay(500);

  loadTimeZoneConfig(); // load timeZoneConfig global and cleans up tzStr

  setTime(timeZoneConfig.utcTime, 0); //ignore us
  udpRunning = udp.begin(localPort); // returns 1 for Ok else 0
  if (udpRunning) {
    ntpUpdateCheck.start(NTP_NOT_UPDATED_MS); // start monitor
    setTZfromPOSIXstr(timeZoneConfig.tzStr);
    updateTimer.start(1); // force update in 1ms i.e. on first loop
  }
}

void forceNTPupdate() {
  if (!udpRunning) {
    return;
  }
  if (responseTimer.isRunning()) {
    // already waiting for a response
    return;
  }
  ntpUpdateCheck.start(NTP_NOT_UPDATED_MS); // start monitor
  updateTimer.start(1); // force update in 1ms
}

void processNTP() {
  if (!udpRunning) {
    return; // udp not started
  }
  missedSNTPupdate(); // update haveSNTPupdate

  if (updateTimer.justFinished()) {  // send next request
    sendNTPpacket(timeServer); // send an NTP packet to a time server
    responseCounter = 0;
    responseTimer.start(responseTimer_ms); // start first timer
    return;
  }

  if (updateTimer.isRunning()) { // waiting to send next request
    return; // waiting for next update to start
  }

  if (responseTimer.justFinished()) {  // check response
    responseCounter++;
    if (debugPtr) {
      debugPtr->print("responseCounter:"); debugPtr->println(responseCounter);
    }
    if (udp.parsePacket()) {
      // We've received a packet, read the data from it
      udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

      // the timestamp starts at byte 40 of the received packet and is four bytes,
      // or two words, long. First, extract the two words:

      unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
      unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
      // combine the four bytes (two words) into a long integer
      // this is NTP time (seconds since Jan 1 1900):
      unsigned long secsSince1900 = highWord << 16 | lowWord;
      //      if (debugPtr) {
      //        debugPtr->print("Seconds since Jan 1 1900 = ");
      //        debugPtr->println(secsSince1900);
      //      }
      // now convert NTP time into everyday time:
      // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
      const unsigned long seventyYears = 2208988800UL;
      // subtract seventy years:
      unsigned long epoch = secsSince1900 - seventyYears;
      // print Unix time:
      if (debugPtr) {
        debugPtr->print("Unix time = ");
        debugPtr->println(epoch);
      }
      setTime(epoch, 0);

      showTimeDebug();
      if (haveSNTPresponse) {
        haveSecondSNTPresponse = true;
      }
      haveSNTPresponse = true;
      haveSNTPupdate = true;
      ntpUpdateCheck.start(NTP_NOT_UPDATED_MS); // start monitor again
      updateTimer.start(ESP_sntp_update_delay_MS_rfc_not_less_than_15000()); // 20sec until haveSecondSNTPresponse

    } else {
      // no response yet
      if (responseCounter >= MAX_ResponseCounter) {
        if (debugPtr) {
          debugPtr->println(" No NTP response in 10sec");
        }
        haveSNTPresponse = true; // update failed
        // request again in 20sec
        updateTimer.start(20ul * 1000);
      } else {
        responseTimer.start(responseTimer_ms); // check response again in 1sec
      }
    }
  }
}

// only handles +v numbers
static void print2digits(String & result, uint num) {
  if (num < 10) {
    result += '0';
  }
  result += num;
}

static String getHHMMss(struct tm * tmPtr) {
  String result; // hh:mm:ss
  print2digits(result, tmPtr->tm_hour);
  result += ':';
  print2digits(result, tmPtr->tm_min);
  result += ':';
  print2digits(result, tmPtr->tm_sec);
  return result;
}

static String getHHMM(struct tm * tmPtr) {
  String result; // hh:mm:ss
  print2digits(result, tmPtr->tm_hour);
  result += ':';
  print2digits(result, tmPtr->tm_min);
  return result;
}

static String getHHMM_AM_PM(struct tm * tmPtr) {
  String result; // hh:mm:ss
  bool pm = false;
  int hr = tmPtr->tm_hour;
  if (hr >= 12) {
    hr -= 12;
    pm = true;
  }
  if (hr == 0) {
    hr = 12;
  }
  //hr = 12; // testing
  result += hr; // only 1 digit no leading 0 print2digits(result, hr);
  result += ':';
  print2digits(result, tmPtr->tm_min);
  if (pm) {
    result += " PM";
  } else {
    result += " AM";
  }
  return result;
}
//// local time HH:MM:ss in min
//static unsigned int getLocalTime_mins() {
//  time_t now = time(nullptr);
//  struct tm* tmPtr = localtime(&now);
//  unsigned int rtn = (tmPtr->tm_hour);
//  rtn = rtn * 60 + tmPtr->tm_min;
//  return rtn;
//}


// local time HH:MM:ss in sec
String getLocalTime_s() {
  time_t now = time(nullptr);
  struct tm* tmPtr = localtime(&now);
  uint32_t rtn = (tmPtr->tm_hour);
  rtn = rtn * 60 + tmPtr->tm_min;
  rtn = rtn * 60 + tmPtr->tm_sec;
  return String(rtn);
}

// small String <=10char) in ESP8266/ESP32 use built in char[]
String getCurrentTime_hhmm() {
  //  gettimeofday(&tv, nullptr);
  //  clock_gettime(0, &tp);
  time_t now = time(nullptr);
  struct tm* tmPtr = localtime(&now);
  return getHHMM(tmPtr);
}
// small String <=10char) in ESP8266/ESP32 use built in char[]
String getCurrentTime_hhmmAM_PM() {
  //  gettimeofday(&tv, nullptr);
  //  clock_gettime(0, &tp);
  time_t now = time(nullptr);
  struct tm* tmPtr = localtime(&now);
  return getHHMM_AM_PM(tmPtr);
}

String getCurrentDayOfWeek() {
  time_t now = time(nullptr);
  struct tm* tmPtr = localtime(&now);
  return daysOfWeek[tmPtr->tm_wday];
}

String getCurrentDayOfMonth() {
  time_t now = time(nullptr);
  struct tm* tmPtr = localtime(&now);
  int mday = tmPtr->tm_mday;
  String rtn(mday);
  if ((mday == 1) || (mday == 21) || (mday == 31)) {
    rtn += "st";
  } else if ((mday == 2) || (mday == 22)) {
    rtn += "nd";
  } else if ((mday == 3) || (mday == 23)) {
    rtn += "rd";
  } else {
    rtn += "th";
  }
  return rtn;
}
String getCurrentMonth() {
  time_t now = time(nullptr);
  struct tm* tmPtr = localtime(&now);
  return monthOfYear[tmPtr->tm_mon];
}

String getCurrentYear() {
  time_t now = time(nullptr);
  struct tm* tmPtr = localtime(&now);
  String rtn(tmPtr->tm_year + 1900);
  return rtn;
}

String getUTCTime() {
  //  gettimeofday(&tv, nullptr);
  //  clock_gettime(0, &tp);
  time_t now = time(nullptr);
  struct tm* tmPtr = gmtime(&now);
  return getHHMMss(tmPtr);
}

#define PTM(w) \
  debugPtr->print(" " #w "="); \
  debugPtr->print(tm->tm_##w);

static void printTm(const char* what, const tm * tm) {
  if (debugPtr) {
    debugPtr->print(what);
    PTM(isdst); PTM(yday); PTM(wday);
    PTM(year);  PTM(mon);  PTM(mday);
    PTM(hour);  PTM(min);  PTM(sec);
    debugPtr->println();
  }
}

String getCurrentTZ() {
  char* tz_str = getenv("TZ");
  if (!tz_str) {
    return "none";
  } // else
  return tz_str;
}

String getCurrentTZdescription() {
  char* tz_str = getenv("TZ");
  String tzStr(tz_str);
  String result;
  getTZDescription(tzStr, result);
  return result;
}

static void showTimeDebug() {
  if (!debugPtr) {
    return;
  }
  debugPtr->println("ShowTimeDebug");
  timeval tv;
  timespec tp;
  time_t now;
  //uint32_t now_ms, now_us;

  gettimeofday(&tv, nullptr);
  clock_gettime(0, &tp);
  now = time(nullptr);
  //now_ms = millis();
  //now_us = micros();

  debugPtr->println();
  printTm("localtime:", localtime(&now));
  debugPtr->println();
  printTm("gmtime:   ", gmtime(&now));
  debugPtr->println();

  // EPOCH+tz+dst
  debugPtr->print("time:      ");
  debugPtr->println((uint32_t)now);

  char* tz_str = getenv("TZ");
  if (!tz_str) { //not set yet
    debugPtr->printf("timezone:  %s\n", "(none)");
  } else {
    // human readable
    debugPtr->printf("timezone:  %s\n", tz_str);
    String tzStr(tz_str);
    String desc;
    getTZDescription(tzStr, desc);
    debugPtr->println(desc);
  }

  debugPtr->print("ctime:     ");
  debugPtr->print(ctime(&now));

  debugPtr->println();
}
