// SDCardSupport.h  file  =================
// Mounts the SD card that carries the solar-forecast shading-adjustment
// data (shade_grid.csv, morning_floor.csv -- see
// dailyPrediction/solar_shading_model.py's write_shade_grid()/
// build_morning_floor()) -- wired to this board's DEFAULT VSPI pins:
// SCK=GPIO18, MOSI("MO")=GPIO23, MISO("MI")=GPIO19, CS=D7 (GPIO13), all
// four confirmed in the FireBeetle 2 ESP32-E variant's pins_arduino.h.
// Because these already ARE the default VSPI pins, no separate
// SPI.begin(sck, miso, mosi, cs) call is needed -- SD.begin(SD_CS_PIN)
// uses the default SPI bus as-is.
//
// Stage 1 of the SD-card feature: mount + verify only (prints the card
// type/size and a root-directory listing to the shared debug output so a
// wiring/formatting problem shows up immediately in the Serial log).
// Reading shade_grid.csv/morning_floor.csv into the solar forecast is a
// later stage.
#ifndef SD_CARD_SUPPORT_H
#define SD_CARD_SUPPORT_H

// Mounts the SD card. Call once from setup(). Prints the card type/size
// and a listing of the root directory to the shared debug output
// (pfodDebugPtr.h) either way -- success or failure -- so the result is
// always visible in the Serial log. Returns true if the card mounted
// successfully.
bool initializeSDCard();

#endif
// ================= end of SDCardSupport.h  file
