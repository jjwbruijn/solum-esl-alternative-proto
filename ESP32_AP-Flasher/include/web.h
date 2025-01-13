
#include <Arduino.h>
#include <AsyncTCP.h>
#ifdef W5500_ETH
  #include <AsyncWebServer_ESP32_SC_W5500.h>
#else
  #include <ESPAsyncWebServer.h>
#endif

void init_web();
void doImageUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
void doJsonUpload(AsyncWebServerRequest *request);
void dotagDBUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);
void wsLog(const String &text);
void wsErr(const String &text);
void wsSendTaginfo(const uint8_t *mac, uint8_t syncMode);
void wsSendSysteminfo();
void wsSendAPitem(struct APlist *apitem);
void wsSerial(const String &text);
void wsSerial(const String &text, const String &color);
uint8_t wsClientCount();

extern AsyncWebSocket ws;
extern SemaphoreHandle_t wsMutex;
