// SDCardSupport.cpp  file  =================
#include "SDCardSupport.h"
#include <SD.h>
#include <SPI.h>
#include <pfodDebugPtr.h>

#define DEBUG  // this file's debug output is on by default -- explicitly requested
static Print *debugPtr = NULL;

// D7 on the FireBeetle 2 ESP32-E is GPIO13 (pins_arduino.h) -- the card's
// CS line. SCK/MOSI("MO")/MISO("MI") are that same board's DEFAULT VSPI
// pins (18/23/19), which is why SD.begin(SD_CS_PIN, ...) below needs no
// custom SPIClass/pins -- it already uses the default SPI bus.
static const uint8_t SD_CS_PIN = 13;

// TEMPORARY diagnostic: SD.begin()'s own default is 4MHz, which is often
// too fast for clean signal integrity on breadboard/jumper-wire SPI runs.
// Confirmed via Core Debug Level=Error output that CMD0 (GO_IDLE_STATE --
// the very first command, before any filesystem is even involved) was
// getting no valid response ("sdCommand(): Card Failed! cmd: 0x00",
// "f_mount failed: (3) The physical drive cannot work") -- i.e. this was
// never a filesystem/formatting problem, it's SPI-layer. Dropped to a
// conservative 400kHz to test whether that's signal integrity; raise it
// back up once CMD0 is confirmed responding, re-tuning for reliability
// vs. read speed.
static const uint32_t SD_SPI_HZ = 400000;

static void printCardType() {
  if (!debugPtr) {
    return;
  }
  sdcard_type_t cardType = SD.cardType();
  debugPtr->print(F("SDCardSupport: card type "));
  switch (cardType) {
    case CARD_MMC: debugPtr->println(F("MMC")); break;
    case CARD_SD: debugPtr->println(F("SDSC")); break;
    case CARD_SDHC: debugPtr->println(F("SDHC")); break;
    case CARD_NONE: debugPtr->println(F("NONE")); break;
    default: debugPtr->println(F("UNKNOWN")); break;
  }
  uint64_t cardSizeMB = SD.cardSize() / (1024 * 1024);
  debugPtr->print(F("SDCardSupport: size "));
  debugPtr->print((uint32_t)cardSizeMB);
  debugPtr->println(F("MB"));
}

// Lists the root directory to debugPtr -- confirms both that the card
// mounted AND that the expected shade_grid.csv/morning_floor.csv files
// are actually present on it, in one piece of debug output, per explicit
// instruction.
static void printRootDir() {
  if (!debugPtr) {
    return;
  }
  File root = SD.open("/");
  if (!root) {
    debugPtr->println(F("SDCardSupport: could not open root directory"));
    return;
  }
  if (!root.isDirectory()) {
    debugPtr->println(F("SDCardSupport: \"/\" is not a directory"));
    root.close();
    return;
  }
  debugPtr->println(F("SDCardSupport: root directory:"));
  int count = 0;
  File entry = root.openNextFile();
  while (entry) {
    debugPtr->print(F("  "));
    debugPtr->print(entry.name());
    if (!entry.isDirectory()) {
      debugPtr->print(F("  ("));
      debugPtr->print((uint32_t)entry.size());
      debugPtr->print(F(" bytes)"));
    }
    debugPtr->println();
    count++;
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  debugPtr->print(F("SDCardSupport: "));
  debugPtr->print(count);
  debugPtr->println(F(" entrie(s)"));
}

bool initializeSDCard() {
  if (!debugPtr) {
#ifdef DEBUG
    debugPtr = getDebugPtr();
#endif
  }

  if (!SD.begin(SD_CS_PIN, SPI, SD_SPI_HZ)) {
    if (debugPtr) {
      debugPtr->println(F("SDCardSupport: SD.begin() failed -- card not mounted (check wiring/formatting)"));
    }
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    if (debugPtr) {
      debugPtr->println(F("SDCardSupport: no SD card detected"));
    }
    return false;
  }

  if (debugPtr) {
    printCardType();
    printRootDir();
  }
  return true;
}
// ================= end of SDCardSupport.cpp  file
