#include "system.h"

#include <Arduino.h>
#include <FS.h>
#include <Preferences.h>
#include <esp_sntp.h>

#include "storage.h"
#include "tag_db.h"
#ifndef W5500_ETH
  #include "wifimanager.h"
#endif

void timeSyncCallback(struct timeval* tv) {
    Serial.println("time succesfully synced");
}

void initTime(void* parameter) {
    #ifndef W5500_ETH
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    #endif
    sntp_set_time_sync_notification_cb(timeSyncCallback);
    sntp_set_sync_interval(300 * 1000);
    configTzTime(config.timeZone, "time.cloudflare.com", "pool.ntp.org", "time.nist.gov");
    logStartUp();
    struct tm timeinfo;
    while (millis() < 30000) {
        if (!getLocalTime(&timeinfo)) {
            Serial.println("Waiting for valid time from NTP-server");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        } else {
            break;
        }
    }
    if (config.runStatus == RUNSTATUS_INIT) {
        config.runStatus = RUNSTATUS_RUN;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
    vTaskDelete(NULL);
}

void logLine(const char* buffer) {
    logLine(String(buffer));
}

void logLine(const String& text) {
    time_t now;
    time(&now);

    char timeStr[24];
    const char* format = (now < (time_t)1672531200) ? "           %H:%M:%S " : "%Y-%m-%d %H:%M:%S ";
    strftime(timeStr, sizeof(timeStr), format, localtime(&now));

    xSemaphoreTake(fsMutex, portMAX_DELAY);
    File logFile = contentFS->open("/log.txt", "a");
    if (logFile) {
        if (logFile.size() >= 10 * 1024) {
            logFile.close();
            contentFS->remove("/logold.txt");
            contentFS->rename("/log.txt", "/logold.txt");
            logFile = contentFS->open("/log.txt", "a");
            if (!logFile) {
                xSemaphoreGive(fsMutex);
                return;
            }
        }

        logFile.print(timeStr);
        logFile.println(text);
        logFile.close();
    }
    xSemaphoreGive(fsMutex);
}

void logStartUp() {
    esp_reset_reason_t resetReason = esp_reset_reason();

    String logEntry = "Reboot. Reason: ";
    switch (resetReason) {
        case ESP_RST_POWERON:
            logEntry += "Power-on";
            break;
        case ESP_RST_EXT:
            logEntry += "External";
            break;
        case ESP_RST_SW:
            logEntry += "Software";
            break;
        case ESP_RST_PANIC:
            logEntry += "Panic";
            break;
        case ESP_RST_INT_WDT:
            logEntry += "Watchdog";
            break;
        case ESP_RST_TASK_WDT:
            logEntry += "Task Watchdog";
            break;
        case ESP_RST_WDT:
            logEntry += "Other Watchdog";
            break;
        case ESP_RST_DEEPSLEEP:
            logEntry += "Deep Sleep";
            break;
        case ESP_RST_BROWNOUT:
            logEntry += "Brownout";
            break;
        case ESP_RST_SDIO:
            logEntry += "SDIO";
            break;
        default:
            logEntry += "Unknown";
            break;
    }

    logLine(logEntry);
}
