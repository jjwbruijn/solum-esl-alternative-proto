
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "contentmanager.h"
#include "flasher.h"
#include "serialap.h"
#include "settings.h"
#include "storage.h"
#include "system.h"
#include "tag_db.h"
#include "tagdata.h"
#ifndef W5500_ETH
  #include "wifimanager.h"
#endif

#ifdef HAS_EXT_FLASHER
#include "webflasher.h"
#endif

#if defined HAS_USB || defined HAS_EXT_FLASHER
#include "usbflasher.h"
#endif

#include "language.h"
#include "leds.h"
#include "udp.h"
#include "util.h"
#include "web.h"
#ifdef HAS_BLE_WRITER
#include "ble_writer.h"
#endif

util::Timer intervalContentRunner(seconds(1));
util::Timer intervalSysinfo(seconds(5));
util::Timer intervalVars(seconds(10));
util::Timer intervalSaveDB(minutes(5));

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void delayedStart(void* parameter) {
    vTaskDelay(30000 / portTICK_PERIOD_MS);
    if (config.runStatus != RUNSTATUS_RUN) {
        wsLog("starting content generation");
        config.runStatus = RUNSTATUS_RUN;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
    vTaskDelete(NULL);
}

void setup() {
    Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT == 1
    Serial.setTxTimeoutMs(0); // workaround bug in USB CDC that slows down serial output when no usb connected
#endif
    Serial.print(">\r\n");
#ifdef HAS_TFT
    extern void yellow_ap_display_init(void);
    yellow_ap_display_init();
#endif

    xTaskCreate(ledTask, "ledhandler", 2000, NULL, 2, NULL);
    vTaskDelay(10 / portTICK_PERIOD_MS);

#if defined(OPENEPAPERLINK_MINI_AP_PCB) || defined(OPENEPAPERLINK_NANO_AP_PCB)
    APEnterEarlyReset();
    // this allows us to view the booting process. After the device showing up, you have 3 seconds to open a terminal on the COM port
    vTaskDelay(3000 / portTICK_PERIOD_MS);
#ifdef DEBUG_VERSION
    // Specifically for the Mini-version (using an ESP32-S2), use another serial port for debug output. Makes it possible to see core dumps
    Serial0.begin(115200, SERIAL_8N1, 38, 37);
    Serial0.printf("Started debug output...\r\n");
    Serial0.setDebugOutput(true);
#endif
#endif

#ifdef BOARD_HAS_PSRAM
    if (!psramInit()) {
        Serial.printf("This build of the AP expects PSRAM, but we couldn't find/init any. Something is terribly wrong here! System halted.");
#ifdef HAS_RGB_LED
        showColorPattern(CRGB::Yellow, CRGB::Red, CRGB::Red);
#endif
        while (1) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    };
    heap_caps_malloc_extmem_enable(64);
#endif

    Storage.begin();

    /*
    Serial.println("\n\n##################################");
    Serial.printf("Internal Total heap %d, internal Free Heap %d\n", ESP.getHeapSize(), ESP.getFreeHeap());
    Serial.printf("SPIRam Total heap %d, SPIRam Free Heap %d\n", ESP.getPsramSize(), ESP.getFreePsram());
    Serial.printf("ChipRevision %d, Cpu Freq %d, SDK Version %s\n", ESP.getChipRevision(), ESP.getCpuFreqMHz(), ESP.getSdkVersion());
    Serial.printf("Flash Size %d, Flash Speed %d\n", ESP.getFlashChipSize(), ESP.getFlashChipSpeed());
    Serial.println("##################################\n\n");

    Serial.printf("Total heap: %d\n", ESP.getHeapSize());
    Serial.printf("Free heap: %d\n", ESP.getFreeHeap());
    Serial.printf("Total PSRAM: %d\n", ESP.getPsramSize());
    Serial.printf("Free PSRAM: %d\n\n", ESP.getFreePsram());

    Serial.printf("ESP32 Partition table:\n");
    Serial.printf("| Type | Sub |  Offset  |   Size   |       Label      |\n");
    Serial.printf("| ---- | --- | -------- | -------- | ---------------- |\n");
    esp_partition_iterator_t pi = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (pi != NULL) {
        do {
            const esp_partition_t* p = esp_partition_get(pi);
            Serial.printf("|  %02x  | %02x  | 0x%06X | 0x%06X | %-16s |\r\n",
                          p->type, p->subtype, p->address, p->size, p->label);
        } while (pi = (esp_partition_next(pi)));
    }
    */

    initAPconfig();

    updateLanguageFromConfig();
    updateBrightnessFromConfig();

    config.runStatus = RUNSTATUS_INIT;
    init_web();
    xTaskCreate(initTime, "init time", 5000, NULL, 2, NULL);

#ifdef HAS_RGB_LED
    rgbIdle();
#endif

#ifndef SAVE_SPACE
    TagData::loadParsers("/parsers.json");
#endif

    if (!loadDB("/current/tagDB.json")) {
        Serial.println("unable to load tagDB, reverting to backup");
        loadDB("/current/tagDB.json.bak");
    } else {
        cleanupCurrent();
    }
    xTaskCreate(APTask, "AP Process", 6000, NULL, 5, NULL);
    vTaskDelay(10 / portTICK_PERIOD_MS);

#ifdef HAS_BLE_WRITER
    if (config.ble) {
        xTaskCreate(BLETask, "BLE Writer", 12000, NULL, 5, NULL);
    }
#endif

#ifdef HAS_USB
    // We'll need to start the 'usbflasher' task for boards with a second (USB) port. This can be used as a 'flasher' interface, using a python script on the host
    xTaskCreate(usbFlasherTask, "usbflasher", 10000, NULL, 5, NULL);
#else
    pinMode(0, INPUT_PULLUP);
#endif

#ifdef HAS_EXT_FLASHER
    xTaskCreate(webFlasherTask, "webflasher", 8000, NULL, 3, NULL);
#endif

    esp_reset_reason_t resetReason = esp_reset_reason();
    if (resetReason == ESP_RST_PANIC) {
        Serial.println("Panic! Pausing content generation for 30 seconds");
        config.runStatus = RUNSTATUS_PAUSE;
    }

    xTaskCreate(delayedStart, "delaystart", 5000, NULL, 2, NULL);

    wsSendSysteminfo();
    util::printHeap();
}

void loop() {
    ws.cleanupClients();
    #ifndef W5500_ETH
        wm.poll();
    #endif

    if (intervalSysinfo.doRun()) {
        wsSendSysteminfo();
    }
    if (intervalVars.doRun() && config.runStatus != RUNSTATUS_STOP) {
        checkVars();
    }
    if (intervalSaveDB.doRun() && config.runStatus != RUNSTATUS_STOP) {
        saveDB("/current/tagDB.json");
    }
    if (intervalContentRunner.doRun() && (apInfo.state == AP_STATE_ONLINE || apInfo.state == AP_STATE_NORADIO)) {
        contentRunner();
    }

#ifdef HAS_TFT
    extern void yellow_ap_display_loop(void);
    yellow_ap_display_loop();
#endif

    vTaskDelay(100 / portTICK_PERIOD_MS);
}
