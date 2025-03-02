#include "ota.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <HTTPClient.h>
#include <MD5Builder.h>
#include <Update.h>

#include "flasher.h"
#include "espflasher.h"
#include "leds.h"
#include "serialap.h"
#include "storage.h"
#include "tag_db.h"
#include "util.h"
#include "web.h"


#ifndef BUILD_ENV_NAME
#define BUILD_ENV_NAME unknown
#endif
#ifndef BUILD_TIME
#define BUILD_TIME 0
#endif
#ifndef BUILD_VERSION
#define BUILD_VERSION custom
#endif
#ifndef SHA
#define SHA 0
#endif

#define STR_IMPL(x) #x
#define STR(x) STR_IMPL(x)
#define LOG(format, ... ) Serial.printf(format,## __VA_ARGS__)


void handleSysinfoRequest(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["alias"] = config.alias;
    doc["env"] = STR(BUILD_ENV_NAME);
    doc["buildtime"] = STR(BUILD_TIME);
    doc["buildversion"] = STR(BUILD_VERSION);
    doc["sha"] = STR(SHA);
    doc["psramsize"] = ESP.getPsramSize();
    doc["flashsize"] = ESP.getFlashChipSize();
    doc["rollback"] = Update.canRollBack();
    doc["ap_version"] = apInfo.version;

    doc["hasC6"] = 0;
    doc["hasH2"] = 0;
    doc["hasTslr"] = 0;

#if defined HAS_H2
    doc["hasH2"] = 1;
#elif defined HAS_TSLR
    doc["hasTslr"] = 1;
#elif defined C6_OTA_FLASHING
    doc["hasC6"] = 1;
#endif

#ifdef HAS_EXT_FLASHER
    doc["hasFlasher"] = 1;
#else
    doc["hasFlasher"] = 0;
#endif
    const size_t bufferSize = measureJson(doc) + 1;
    AsyncResponseStream* response = request->beginResponseStream("application/json", bufferSize);
    serializeJson(doc, *response);
    request->send(response);
};

void handleCheckFile(AsyncWebServerRequest* request) {
    if (!request->hasParam("path")) {
        request->send(400);
        return;
    }

    const String filePath = request->getParam("path")->value();
    File file = contentFS->open(filePath, "r");
    if (!file) {
        JsonDocument doc;
        doc["filesize"] = 0;
        doc["md5"] = "";
        String jsonResponse;
        serializeJson(doc, jsonResponse);
        request->send(200, "application/json", jsonResponse);
        return;
    }

    const size_t fileSize = file.size();

    MD5Builder md5;
    md5.begin();
    md5.addStream(file, fileSize);
    md5.calculate();
    const String md5Hash = md5.toString();

    file.close();

    JsonDocument doc;
    doc["filesize"] = fileSize;
    doc["md5"] = md5Hash;
    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse);
}

#define UPLOAD_BUFFER_SIZE 32768

struct UploadInfo {
    String filename;
    uint8_t buffer[UPLOAD_BUFFER_SIZE];
    size_t bufferSize;
};

void handleLittleFSUpload(AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
    String uploadfilename;
    bool error = false;
    if (!index) {
        String path;
        if (!request->hasParam("path", true)) {
            final = true;
            error = true;
        } else {
            uploadfilename = request->getParam("path", true)->value();
            Serial.println("update " + uploadfilename);
            File file = contentFS->open(uploadfilename, "w");
            file.close();
            UploadInfo* uploadInfo = new UploadInfo{uploadfilename, {}, 0};
            request->_tempObject = (void*)uploadInfo;
        }
    }

    UploadInfo* uploadInfo = static_cast<UploadInfo*>(request->_tempObject);

    if (uploadInfo != nullptr) {
        uploadfilename = uploadInfo->filename;

        if (len) {
            if (uploadInfo->bufferSize + len <= UPLOAD_BUFFER_SIZE) {
                memcpy(&uploadInfo->buffer[uploadInfo->bufferSize], data, len);
                uploadInfo->bufferSize += len;
            } else {
                xSemaphoreTake(fsMutex, portMAX_DELAY);
                File file = contentFS->open(uploadfilename, "a");
                if (file) {
                    file.write(uploadInfo->buffer, uploadInfo->bufferSize);
                    file.close();
                    uploadInfo->bufferSize = 0;
                } else {
                    logLine("Failed to open file for appending: " + uploadfilename);
                    final = true;
                    error = true;
                }
                xSemaphoreGive(fsMutex);

                memcpy(uploadInfo->buffer, data, len);
                uploadInfo->bufferSize = len;
            }
        }
        if (final) {
            if (uploadInfo->bufferSize > 0) {
                xSemaphoreTake(fsMutex, portMAX_DELAY);
                File file = contentFS->open(uploadfilename, "a");
                if (file) {
                    file.write(uploadInfo->buffer, uploadInfo->bufferSize);
                    file.close();
                } else {
                    logLine("Failed to open file for appending: " + uploadfilename);
                    error = true;
                }
                xSemaphoreGive(fsMutex);
                request->_tempObject = nullptr;
                delete uploadInfo;
            }

            if (error) {
                request->send(507, "text/plain", "Error. Disk full?");
            } else {
                request->send(200, "text/plain", "Ok, file written");
            }
        }
    }
}

struct FirmwareUpdateParams {
    String url;
    String md5;
    size_t size;
};

void handleUpdateOTA(AsyncWebServerRequest* request) {
    if (request->hasParam("url", true) && request->hasParam("md5", true) && request->hasParam("size", true)) {
        FirmwareUpdateParams* params = new FirmwareUpdateParams;
        params->url = request->getParam("url", true)->value();
        params->md5 = request->getParam("md5", true)->value();
        params->size = request->getParam("size", true)->value().toInt();
        xTaskCreate(firmwareUpdateTask, "OTAUpdateTask", 6144, params, 10, NULL);

        request->send(200, "text/plain", "In progress");
    } else {
        request->send(400, "Bad request");
    }
}

void firmwareUpdateTask(void* parameter) {
    FirmwareUpdateParams* params = reinterpret_cast<FirmwareUpdateParams*>(parameter);

    if (ESP.getMaxAllocHeap() < 22000) {
        wsSerial("Error: Not enough memory left. Restart the esp32 and try updating again.");
        wsSerial("[reboot]");
    } else {
        const char* url = params->url.c_str();
        const char* md5 = params->md5.c_str();
        const size_t size = params->size;
        updateFirmware(url, md5, size);
    }

    delete params;
    vTaskDelete(NULL);
}

void updateFirmware(const char* url, const char* expectedMd5, const size_t size) {
    util::printHeap();

    config.runStatus = RUNSTATUS_STOP;
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    saveDB("/current/tagDB.json");
    // destroyDB();

    HTTPClient httpClient;

    wsSerial("start downloading");
    wsSerial(url);

    httpClient.begin(url);
    httpClient.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    util::printHeap();
    const int httpCode = httpClient.GET();
    util::printHeap();

    if (httpCode == HTTP_CODE_OK) {
        if (Update.begin(size)) {
            Update.setMD5(expectedMd5);

            unsigned long progressTimer = millis();
            Update.onProgress([&progressTimer](size_t progress, size_t total) {
                if (millis() - progressTimer > 500 || progress == total) {
                    char buffer[50];
                    sprintf(buffer, "Progress: %u%% %d %d", progress * 100 / total, progress, total);
                    wsSerial(String(buffer));
                    progressTimer = millis();
                    vTaskDelay(1 / portTICK_PERIOD_MS);
                }
            });

            const size_t written = Update.writeStream(httpClient.getStream());
            if (written == httpClient.getSize()) {
                if (Update.end(true)) {
                    wsSerial("Firmware update successful");
                    wsSerial("Reboot system now");
                    wsSerial("[reboot]");
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                } else {
                    wsSerial("Error updating firmware:");
                    wsSerial(Update.errorString());
                }
            } else {
                wsSerial("Error writing firmware data:");
                wsSerial(Update.errorString());
            }
        } else {
            wsSerial("Failed to begin firmware update");
            wsSerial(Update.errorString());
        }
    } else {
        wsSerial("Failed to download firmware file (HTTP code " + String(httpCode) + ")");
        wsSerial(httpClient.errorToString(httpCode));
    }

    httpClient.end();
    config.runStatus = RUNSTATUS_RUN;
}

void handleRollback(AsyncWebServerRequest* request) {
    if (Update.canRollBack()) {
        const bool rollbackSuccess = Update.rollBack();
        if (rollbackSuccess) {
            request->send(200, "Rollback successful");
            wsSerial("Rollback successful");
            wsSerial("Reboot system now");
            wsSerial("[reboot]");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        } else {
            wsSerial("Rollback failed");
            request->send(400, "Rollback failed");
        }
    } else {
        wsSerial("Rollback not allowed");
        request->send(400, "Rollback not allowed");
    }
}

#ifdef C6_OTA_FLASHING
void C6firmwareUpdateTask(void* parameter) {
   String *Url = reinterpret_cast<String *>(parameter);
   LOG("C6firmwareUpdateTask: url '%s'\n",Url->c_str());
    wsSerial("Stopping AP service");

    setAPstate(false, AP_STATE_FLASHING);
    config.runStatus = RUNSTATUS_STOP;
#ifndef FLASHER_DEBUG_SHARED
    extern bool rxSerialStopTask2;
    rxSerialStopTask2 = true;
#endif
    vTaskDelay(500 / portTICK_PERIOD_MS);
    Serial1.end();

    wsSerial(SHORT_CHIP_NAME " flash starting");

    bool result = FlashC6_H2(Url->c_str());

    wsSerial(SHORT_CHIP_NAME " flash end");

    if (result) {
        setAPstate(false, AP_STATE_OFFLINE);

        wsSerial("Finishing config...");
        vTaskDelay(3000 / portTICK_PERIOD_MS);

        wsSerial("starting monitor");
        Serial1.begin(115200, SERIAL_8N1, FLASHER_AP_RXD, FLASHER_AP_TXD);
#ifndef FLASHER_DEBUG_SHARED
        rxSerialStopTask2 = false;
        xTaskCreate(rxSerialTask2, "rxSerialTask2", 1750, NULL, 2, NULL);
#endif
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        apInfo.version = 0;
        wsSerial("resetting AP");
        APTagReset();
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        wsSerial("bringing AP online");
        if (bringAPOnline()) config.runStatus = RUNSTATUS_RUN;

     // Wait for version info to arrive 
        vTaskDelay(50 / portTICK_PERIOD_MS);
        if(apInfo.version == 0) {
           result = false;
        }
    } 

    if (result) {
       wsSerial("Finished!");
       char buffer[50];
       snprintf(buffer,sizeof(buffer),
                "ESP32-" SHORT_CHIP_NAME " version is now %04x",apInfo.version);
       wsSerial(String(buffer));
    }
    else if(apInfo.version == 0) {
       wsSerial("AP failed failed to come online. :-(");
    }
    else {
       wsSerial("Flashing failed. :-(");
    }
    delete Url;
    vTaskDelete(NULL);
}
#endif


void handleUpdateC6(AsyncWebServerRequest* request) {
#if defined C6_OTA_FLASHING
    if (request->hasParam("url",true)) {
       String *Url = new String(request->getParam("url",true)->value());
       xTaskCreate(C6firmwareUpdateTask, "OTAUpdateTask", 6400, Url, 10, NULL);
       request->send(200, "Ok");
    }
    else {
       LOG("Sending bad request");
       request->send(400, "Bad request");
    }
#elif defined(SHORT_CHIP_NAME)
    request->send(400, SHORT_CHIP_NAME " flashing not implemented");
#else
    request->send(400, "C6/H2 flashing not implemented");
#endif
}

void handleUpdateActions(AsyncWebServerRequest* request) {
    wsSerial("Performing cleanup");
    File file = contentFS->open("/update_actions.json", "r");
    if (!file) {
        wsSerial("No update_actions.json present");
        request->send(200, "No update actions needed");
        return;
    }
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    const JsonArray deleteFiles = doc["deletefile"].as<JsonArray>();
    for (const auto& filePath : deleteFiles) {
        if (contentFS->remove(filePath.as<const char*>())) {
            wsSerial("deleted file: " + filePath.as<String>());
        }
    }
    file.close();
    wsSerial("Cleanup finished");
    request->send(200, "Clean up finished");
    contentFS->remove("/update_actions.json");
}