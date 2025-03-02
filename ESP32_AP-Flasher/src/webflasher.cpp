#include "webflasher.h"

#ifdef HAS_EXT_FLASHER

#include <ArduinoJson.h>
#include <AsyncTCP.h>
// #include <freertos/FreeRTOS.h>
// #include <freertos/task.h>

#include "flasher.h"
#include "ips_display.h"
#include "settings.h"
// #include "storage.h"

#include "powermgt.h"
#include "swd.h"
#include "usbflasher.h"
#include "util.h"
#include "web.h"
#include "zbs_interface.h"

#define FLASHMODE_OFF 0
#define FLASHMODE_AUTO_BACKGROUND 1
#define FLASHMODE_AUTO_FOCUS 2

#define AUTOFLASH_STEP_IDLE 0
#define AUTOFLASH_STEP_CONNECT 1
#define AUTOFLASH_STEP_STARTUP 2
#define AUTOFLASH_STEP_WAITDISCONNECT 3
#define AUTOFLASH_STEP_FINISHED 4
#define AUTOFLASH_STEP_ABORT 5
#define AUTOFLASH_STEP_COUNTDOWN 6
#define AUTOFLASH_STEP_COUNTING 8
#define AUTOFLASH_STEP_WRITE 7
#define AUTOFLASH_START_USBFLASHER 9
#define AUTOFLASH_USBFLASHER_RUNNING 10
#define AUTOFLASH_END_USBFLASHER 11

#define Seriallog logger

uint8_t webFlashMode = FLASHMODE_OFF;
uint8_t autoFlashStep = AUTOFLASH_STEP_IDLE;
bool serialPassthroughState = false;
// TaskHandle_t usbFlasherTaskHandle;

AsyncServer TCPserver(243);
AsyncClient* connectedClient = NULL;

Logger::Logger() {
}

Logger::~Logger() {
}

size_t Logger::write(uint8_t c) {
    wsSerial(String((char)c));
    return 1;
}

size_t Logger::write(const uint8_t* buffer, size_t size) {
    wsSerial(String((const char*)buffer, size));
    return size;
}

size_t Logger::printf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    size_t len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    wsSerial(String(buffer));

#ifdef HAS_TFT
    if (len >= 6 && strncmp(buffer, "\r[", 2) == 0) {
        char content[50];
        strncpy(content, buffer + 2, len - 7);
        content[len - 7] = '\0';
        tft2.fillRect(0, 50, tft2.width(), 40, TFT_BLUE);
        tft2.setCursor(10, 57, 2);
        tft2.setTextColor(TFT_YELLOW);
        tft2.print(String(content));
    }
#endif

    return len;
}

size_t Logger::println(const char* text) {
    size_t len = print(text);
    print('\n');
    return len + 1;
}

Logger logger;
util::Timer tagConnectTimer(seconds(2));
extern flasher* zbsflasherp;
uint8_t errors = 0;

void infoDisplay(String actionName, uint8_t icon) {
    wsSerial("  " + actionName, "white");
#ifdef HAS_TFT
    tftOverride = true;
    tft2.fillScreen(TFT_PURPLE);
    tft2.setCursor(0, 0, 4);
    tft2.setTextColor(TFT_WHITE);
    tft2.print("Progress:");
    tft2.setCursor(18, 30, 2);
    tft2.setTextColor(TFT_WHITE);
    tft2.print(actionName);
#endif
}

bool report(bool result) {
    if (result) {
        wsSerial("<✔", "green");
#ifdef HAS_TFT
        tft2.setTextColor(TFT_GREEN);
        tft2.setCursor(5, 30, 2);
        tft2.print("v");
        tft2.setCursor(6, 30, 2);
        tft2.print("v");
        vTaskDelay(100 / portTICK_PERIOD_MS);
#endif
    } else {
        errors++;
        autoFlashStep = AUTOFLASH_STEP_ABORT;
        wsSerial("<✘", "red");
#ifdef HAS_TFT
        tft2.setTextColor(TFT_RED);
        tft2.setCursor(5, 30, 2);
        tft2.print("x");
        tft2.setCursor(6, 30, 2);
        tft2.print("x");
        vTaskDelay(500 / portTICK_PERIOD_MS);
#endif
    }
    return result;
}

void onDataReceived(void* arg, AsyncClient* client, void* data, size_t len) {
    flasherDataHandler((uint8_t*)data, len, TRANSPORT_TCP);
}

void onClientConnect(void* arg, AsyncClient* client) {
    Serial.println("New client connected");
    if (autoFlashStep == AUTOFLASH_USBFLASHER_RUNNING) {
#ifdef HAS_TFT
        if (tftOverride == true) {
            tft2.fillRect(0, 62, tft2.width(), 18, TFT_BLUE);
            tft2.setCursor(10, 63, 2);
            tft2.setTextColor(TFT_GREEN);
            tft2.print("TCP connected");
        }
#endif
    } else {
        autoFlashStep = AUTOFLASH_START_USBFLASHER;
    }
    webFlashMode = FLASHMODE_OFF;
    if (connectedClient == NULL) {
        connectedClient = client;
        client->onData(&onDataReceived, NULL);
        client->onDisconnect([](void* arg, AsyncClient* c) {
            Serial.println("Client disconnected");

            if (zbsflasherp != nullptr) {
                zbsflasherp->zbs->reset();
                delete zbsflasherp;
                zbsflasherp = nullptr;
            }

            pinMode(FLASHER_EXT_MISO, INPUT);
            pinMode(FLASHER_EXT_CLK, INPUT);

#ifdef HAS_TFT
            if (autoFlashStep == AUTOFLASH_USBFLASHER_RUNNING && tftOverride == true) {
                tft2.fillRect(0, 62, tft2.width(), 18, TFT_BLUE);
                tft2.setCursor(10, 63, 2);
                tft2.setTextColor(TFT_YELLOW);
                tft2.print("TCP disconnected");
            }
#endif
            connectedClient = NULL;
        });
    } else {
        client->close(true);
    }
}

void sendDataToClient(const uint8_t* data, size_t len) {
    if (connectedClient != NULL) {
        connectedClient->write(reinterpret_cast<const char*>(data), len);
    } else {
        Serial.println("No client connected");
    }
}

void webFlasherTask(void* parameter) {
    TCPserver.begin();
    TCPserver.onClient(&onClientConnect, NULL);

    static int32_t countDownTimer = 0;
    while (1) {
        switch (autoFlashStep) {
            case AUTOFLASH_STEP_IDLE: {
                if (webFlashMode != FLASHMODE_OFF && (tagConnectTimer.doRun() || webFlashMode == FLASHMODE_AUTO_FOCUS)) {
                    Serial.println("check pins");
                    if (extTagConnected()) autoFlashStep = AUTOFLASH_STEP_CONNECT;
                }
                break;
            }

            case AUTOFLASH_STEP_CONNECT: {
                wsSerial("-", "clear");
                infoDisplay("Connecting", 0);
                vTaskDelay(2000 / portTICK_PERIOD_MS);

                // pinMode(FLASHER_EXT_TEST, OUTPUT);
                // digitalWrite(FLASHER_EXT_TEST, LOW);

                zbsflasherp = new flasher();
                errors = 0;

                if (!report(zbsflasherp->connectTag(FLASHER_EXT_PORT))) {
                    Seriallog.printf("Sorry, failed to connect to this tag...\r\n");
                    break;

                } else {
                    infoDisplay("Get firmware md5", 0);
                    report(zbsflasherp->getFirmwareMD5());

                    infoDisplay("Find tag", 0);
                    if (zbsflasherp->findTagByMD5()) {
                        // this tag currently contains original firmware, found its fingerprint
                        Seriallog.printf("Found original firmware tag (fingerprint %s)\r\n", zbsflasherp->md5char);

                        infoDisplay("Read info block", 0);
                        report(zbsflasherp->readInfoBlock());
                        if (errors) break;

                        infoDisplay("Get firmware mac", 0);
                        report(zbsflasherp->getFirmwareMac());
                        if (errors) break;

                        infoDisplay("Prepare info block", 0);
                        report(zbsflasherp->prepareInfoBlock());
                        if (errors) break;

                        zbsflasherp->includeInfoBlock = true;
                        autoFlashStep = AUTOFLASH_STEP_COUNTDOWN;
                        break;

                    } else if (report(zbsflasherp->getInfoBlockMD5())) {
                        infoDisplay("Find by md5", 0);
                        // did find an infoblock MD5 that looks valid
                        if (report(zbsflasherp->findTagByMD5())) {
                            // did find the md5 in the database

                            infoDisplay("Found tag", 0);
                            Seriallog.printf("Found an already-flashed tag (fingerprint %s)\r\n", zbsflasherp->md5char);

                            infoDisplay("Get infoblock mac", 0);
                            report(zbsflasherp->getInfoBlockMac());
                            if (errors) break;

                            infoDisplay("Get infoblock type", 0);
                            report(zbsflasherp->getInfoBlockType());
                            if (errors) break;

                            infoDisplay("Read infoblock", 0);
                            report(zbsflasherp->readInfoBlock());
                            if (errors) break;

                            zbsflasherp->includeInfoBlock = false;
                            autoFlashStep = AUTOFLASH_STEP_COUNTDOWN;
                            break;

                        } else {
                            infoDisplay("Unknown fingerprint", 0);
                            report(false);
                            // couldn't find the md5 from the infoblock
                            Seriallog.printf("Found an already-flashed tag, but we couldn't find its fingerprint (%s) in the database\r\n", zbsflasherp->md5char);
                            break;
                        }
                    } else {
                        // We couldn't recognize the tag from its fingerprint...
                        Seriallog.printf("Found a tag but didn't recognize its fingerprint (%s)\r\n", zbsflasherp->md5char);

                        infoDisplay("Backup firmware", 0);
                        report(zbsflasherp->backupFlash());

                        Seriallog.printf("Saved this MD5 binary to filesystem\r\n");

                        infoDisplay("Unknown tag", 0);
                        report(false);
                        break;
                    }
                }

                zbsflasherp->zbs->reset(false);

                delete zbsflasherp;
                zbsflasherp = nullptr;

                autoFlashStep = AUTOFLASH_STEP_FINISHED;
                break;
            }

            case AUTOFLASH_STEP_COUNTDOWN: {
                infoDisplay("Final Countdown", 0);

                countDownTimer = millis() + 3000;
                autoFlashStep = AUTOFLASH_STEP_COUNTING;
                break;
            }

            case AUTOFLASH_STEP_COUNTING: {
                uint32_t timenow = millis();
                if (timenow >= countDownTimer) {
                    autoFlashStep = AUTOFLASH_STEP_WRITE;
                    timenow = countDownTimer;
                }

                float timeDifference = countDownTimer - timenow;
                char timeString[10];
                snprintf(timeString, sizeof(timeString), "%.1fs", timeDifference / 1000.0);

#ifdef HAS_TFT
                tftOverride = true;
                tft2.fillRect(0, 50, tft2.width(), 40, TFT_BLUE);
                tft2.setCursor(10, 55, 4);
                tft2.setTextColor(TFT_YELLOW);
                tft2.print(timeString);
#endif
                wsSerial("\r" + String(timeString), "yellow");
                break;
            }

            case AUTOFLASH_STEP_WRITE: {
                if (zbsflasherp->includeInfoBlock) {
                    infoDisplay("Write info block", 0);
                    report(zbsflasherp->writeInfoBlock());
                    if (errors) break;
                }

                infoDisplay("Write flash", 0);
                report(zbsflasherp->writeFlashFromPack("/Tag_FW_Pack.bin", zbsflasherp->tagtype));
                if (errors) break;

                zbsflasherp->zbs->reset(false);
                delete zbsflasherp;
                zbsflasherp = nullptr;

                infoDisplay("Write successful", 0);
                report(true);

                wsSerial("Flashing succeeded", "green");
#ifdef HAS_TFT
                tftOverride = true;
                tft2.fillRect(0, 50, tft2.width(), 40, TFT_DARKGREEN);
                tft2.setCursor(0, 55, 4);
                tft2.setTextColor(TFT_WHITE);
                tft2.print("OK, Done!");
                vTaskDelay(500 / portTICK_PERIOD_MS);
#endif

                autoFlashStep = AUTOFLASH_STEP_FINISHED;
                break;
            }

            case AUTOFLASH_STEP_ABORT: {
                wsSerial("Flashing failed", "red");
#ifdef HAS_TFT
                tftOverride = true;
                tft2.fillRect(0, 50, tft2.width(), 40, TFT_RED);
                tft2.setCursor(0, 55, 4);
                tft2.setTextColor(TFT_WHITE);
                tft2.print("Flash failed");
                vTaskDelay(500 / portTICK_PERIOD_MS);
#endif

                zbsflasherp->zbs->reset(false);
                delete zbsflasherp;
                zbsflasherp = nullptr;

                vTaskDelay(100 / portTICK_PERIOD_MS);
                pinMode(FLASHER_EXT_TEST, INPUT_PULLDOWN);
                vTaskDelay(100 / portTICK_PERIOD_MS);
                autoFlashStep = AUTOFLASH_STEP_WAITDISCONNECT;
                break;
            }

            case AUTOFLASH_STEP_FINISHED: {
                Seriallog.printf("You can safely disconnect the tag");

                vTaskDelay(100 / portTICK_PERIOD_MS);
                pinMode(FLASHER_EXT_TEST, INPUT_PULLDOWN);
                vTaskDelay(100 / portTICK_PERIOD_MS);
                autoFlashStep = AUTOFLASH_STEP_WAITDISCONNECT;
                break;
            }

            case AUTOFLASH_STEP_WAITDISCONNECT: {
                if (!extTagConnected()) autoFlashStep = AUTOFLASH_STEP_STARTUP;
                vTaskDelay(500 / portTICK_PERIOD_MS);
                break;
            }

            case AUTOFLASH_STEP_STARTUP: {
                int8_t powerPins2[] = FLASHER_EXT_POWER;
                uint8_t numPowerPins = sizeof(powerPins2);
                powerControl(false, (uint8_t*)powerPins2, numPowerPins);

#ifdef HAS_TFT
                tftOverride = true;
                tft2.fillScreen(TFT_PURPLE);
                tft2.setCursor(0, 0, 4);
                tft2.setTextColor(TFT_WHITE);
                tft2.print("Flasher mode");
                tft2.setCursor(0, 25, 2);
                tft2.setTextColor(TFT_WHITE);
                tft2.print("Ready to connect a tag.");
                tft2.print("When recognized, it will be flashed automatically");
                if (webFlashMode == FLASHMODE_AUTO_BACKGROUND) tftOverride = false;
#endif
                wsSerial("Ready to connect a tag. It will be flashed automatically", "silver");
                autoFlashStep = AUTOFLASH_STEP_IDLE;
                vTaskDelay(100 / portTICK_PERIOD_MS);
                break;
            }

            case AUTOFLASH_START_USBFLASHER: {
#ifdef HAS_TFT
                tftOverride = true;
                tft2.fillScreen(TFT_PURPLE);
                tft2.setCursor(0, 0, 4);
                tft2.setTextColor(TFT_WHITE);
                tft2.print("CMD mode");
                tft2.setCursor(0, 25, 2);
                tft2.setTextColor(TFT_WHITE);
                tft2.print("Use OEPL-flasher.py to  flash a tag");
                if (webFlashMode == FLASHMODE_AUTO_BACKGROUND) tftOverride = false;
#endif
                autoFlashStep = AUTOFLASH_USBFLASHER_RUNNING;
                break;
            }

            case AUTOFLASH_USBFLASHER_RUNNING: {
                break;
            }

            case AUTOFLASH_END_USBFLASHER: {
                autoFlashStep = AUTOFLASH_STEP_FINISHED;
                break;
            }
        }

        /* work in progress
        if (serialPassthroughState) {
            tagDebugPassthrough();
            vTaskDelay(1 / portTICK_PERIOD_MS);
        } else {
        */
            vTaskDelay(100 / portTICK_PERIOD_MS);
        // }
    }
}

void handleWSdata(uint8_t* data, size_t len, AsyncWebSocketClient* client) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, (const char*)data);
    JsonDocument response;
    response["flashstatus"] = 1;

    if (error) {
        wsSerial("Failed to parse JSON");
        return;
    }

    if (doc["flashcmd"].is<int>()) {
        uint16_t flashcmd = doc["flashcmd"].as<int>();
        switch (flashcmd) {
            case WEBFLASH_ENABLE_AUTOFLASH:
                wsSerial("Switching to autoflash", "yellow");
                webFlashMode = FLASHMODE_AUTO_FOCUS;
                autoFlashStep = AUTOFLASH_STEP_STARTUP;
                break;
            case WEBFLASH_ENABLE_USBFLASHER:
                wsSerial("Switching to usbflasher", "yellow");
                wsSerial("You can now use OEPL-flasher.py to flash your tags", "silver");
                autoFlashStep = AUTOFLASH_START_USBFLASHER;
                webFlashMode = FLASHMODE_OFF;
                break;
            case WEBFLASH_FOCUS:
                if (webFlashMode == FLASHMODE_AUTO_BACKGROUND) webFlashMode = FLASHMODE_AUTO_FOCUS;
                if (webFlashMode == FLASHMODE_OFF) {
                    autoFlashStep = AUTOFLASH_START_USBFLASHER;
                } else {
                    autoFlashStep = AUTOFLASH_STEP_STARTUP;
                }
#ifdef HAS_TFT
                tftOverride = true;
#endif
                break;
            case WEBFLASH_BLUR:
                if (webFlashMode == FLASHMODE_AUTO_FOCUS) webFlashMode = FLASHMODE_AUTO_BACKGROUND;
#ifdef HAS_TFT
                tftOverride = false;
                sendAvail(0xFC);
#endif
                break;
            case WEBFLASH_POWER_ON: {
                wsSerial("Power up", "yellow");

                extern bool rxSerialStopTask2;
                if (rxSerialStopTask2 == false) {
                    rxSerialStopTask2 = true;
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                }

                int8_t powerPins2[] = FLASHER_EXT_POWER;
                uint8_t numPowerPins = sizeof(powerPins2);
                powerControl(true, (uint8_t*)powerPins2, numPowerPins);

                /*  work in progress
                if (Serial2) Serial2.end();
                Serial2.begin(115200, SERIAL_8N1, FLASHER_EXT_RXD, FLASHER_EXT_TXD);
                Serial.println(">>>");
                serialPassthroughState = true;
                */
                break;
            }
            case WEBFLASH_POWER_OFF: {
                wsSerial("Power down", "yellow");
                if (serialPassthroughState) {
                    serialPassthroughState = false;
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                }
                extern bool rxSerialStopTask2;
                if (rxSerialStopTask2 == false) {
                    rxSerialStopTask2 = true;
                    vTaskDelay(500 / portTICK_PERIOD_MS);
                }

                if (Serial2) Serial2.end();
                pinMode(FLASHER_EXT_MISO, INPUT);
                pinMode(FLASHER_EXT_CLK, INPUT);
                pinMode(FLASHER_EXT_TEST, INPUT);
                pinMode(FLASHER_EXT_RXD, INPUT);
                pinMode(FLASHER_EXT_TXD, INPUT);
                pinMode(FLASHER_EXT_RESET, INPUT);
                int8_t powerPins2[] = FLASHER_EXT_POWER;
                uint8_t numPowerPins = sizeof(powerPins2);
                powerControl(false, (uint8_t*)powerPins2, numPowerPins);
                break;
            }
        }
    }

    String jsonResponse;
    serializeJson(response, jsonResponse);
    client->text(jsonResponse);
}

#endif