#include <Arduino.h>
#include <LittleFS.h>

#if defined HAS_H2
   #define SHORT_CHIP_NAME "H2"
   #define OTA_BIN_DIR     "ESP32-H2"
   #define ESP_CHIP_TYPE   ESP32H2_CHIP
#elif defined HAS_TSLR
   #define SHORT_CHIP_NAME "TSLR"
#elif defined C6_OTA_FLASHING
   #define SHORT_CHIP_NAME "C6"
   #define OTA_BIN_DIR     "ESP32-C6"
   #define ESP_CHIP_TYPE   ESP32C6_CHIP
#endif

bool FlashC6_H2(const char *Url);
