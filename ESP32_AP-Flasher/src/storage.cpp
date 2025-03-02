#include "storage.h"

#ifdef HAS_SDCARD
#include "FS.h"
#ifdef SD_CARD_SDMMC
#include "SD_MMC.h"
#define SDCARD SD_MMC
#else
#include "SD.h"
#include "SPI.h"
#define SDCARD SD
#endif
#endif

#ifndef SD_CARD_ONLY
#include "LittleFS.h"
#endif

DynStorage::DynStorage() : isInited(0) {}

SemaphoreHandle_t fsMutex = NULL;

#ifndef SD_CARD_ONLY
static void initLittleFS() {
    LittleFS.begin();
    contentFS = &LittleFS;
}
#endif

#ifdef HAS_SDCARD
static bool sd_init_done = false;
#ifdef SD_CARD_SDMMC
static void initSDCard() {
    if(!SD_MMC.begin("/sdcard", true, true, BOARD_MAX_SDMMC_FREQ, 5)){
        Serial.println("Card Mount Failed");
        return;
    }
    uint8_t cardType = SD_MMC.cardType();

    if(cardType == CARD_NONE){
        Serial.println("No SD_MMC card attached");
        return;
    }

    Serial.print("SD_MMC Card Type: ");
    if(cardType == CARD_MMC){
        Serial.println("MMC");
    } else if(cardType == CARD_SD){
        Serial.println("SDSC");
    } else if(cardType == CARD_SDHC){
        Serial.println("SDHC");
    } else {
        Serial.println("UNKNOWN");
    }

    uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
    Serial.printf("SD_MMC Card Size: %lluMB\n", cardSize);

    contentFS = &SD_MMC;
}
#else
static SPIClass* spi;

static void initSDCard() {
    uint8_t spi_bus = VSPI;

    // SD.begin and spi.begin are allocating memory so we dont want to do that
    if(!spi) { 
        spi = new SPIClass(spi_bus);
        spi->begin(SD_CARD_CLK, SD_CARD_MISO, SD_CARD_MOSI, SD_CARD_SS);

        bool res = SD.begin(SD_CARD_SS, *spi, 40000000);
        if (!res) {
            Serial.println("Card Mount Failed");
            return;
        }
    }

    uint8_t cardType = SD.cardType();

    if (cardType == CARD_NONE) {
        Serial.println("No SD card attached");
        return;
    }

    contentFS = &SD;
}
#endif
#endif

uint64_t DynStorage::freeSpace(){
    this->begin();
#ifdef HAS_SDCARD
    return SDCARD.totalBytes() - SDCARD.usedBytes();
#endif
#ifndef SD_CARD_ONLY
    return LittleFS.totalBytes() - LittleFS.usedBytes();
#endif
}

#ifndef SD_CARD_ONLY
void copyFile(File in, File out) {
    Serial.print("Copying ");
    Serial.print(in.path());
    Serial.print(" to ");
    Serial.println(out.path());

    size_t n;
    uint8_t buf[64];
    while ((n = in.read(buf, sizeof(buf))) > 0) {
        out.write(buf, n);
    }
}

#ifdef HAS_SDCARD

void copyBetweenFS(FS& sourceFS, const char* source_path, FS& targetFS) {
    File root = sourceFS.open(source_path);
    char next_path[128];

    if (root.isDirectory()) {
        if (!contentFS->exists(root.path())) {
            if (!contentFS->mkdir(root.path())) {
                Serial.print("Failed to create directory ");
                Serial.println(root.path());
                return;
            }
        }
        File file = root.openNextFile();
        while (file) {
            if (file.isDirectory()) {
                sprintf(next_path, "%s/%s\0", root.path(), file.path());

                copyBetweenFS(sourceFS, file.path(), targetFS);
            } else {
                xSemaphoreTake(fsMutex, portMAX_DELAY);
                File target = contentFS->open(file.path(), "w");
                if (target) {
                    copyFile(file, target);
                    target.close();
                    file.close();
                    xSemaphoreGive(fsMutex);
                } else {
                    xSemaphoreGive(fsMutex);
                    Serial.print("Couldn't create high target file");
                    Serial.println(file.path());
                    return;
                }
            }
            file = root.openNextFile();
        }
    } else {
        xSemaphoreTake(fsMutex, portMAX_DELAY);
        File target = contentFS->open(root.path(), "w");
        if (target) {
            copyFile(root, target);
            target.close();
            xSemaphoreGive(fsMutex);
        } else {
            xSemaphoreGive(fsMutex);
            Serial.print("Couldn't create target file ");
            Serial.println(root.path());
            return;
        }
    }
}

void copyIfNeeded(const char* path) {
    if (!contentFS->exists(path) && LittleFS.exists(path)) {
        Serial.printf("SDCard does not contain %s, littleFS does, copying\r\n", path);
        copyBetweenFS(LittleFS, path, *contentFS);
    }
}
#endif
#endif

void DynStorage::begin() {
    if(fsMutex == NULL) {
        fsMutex = xSemaphoreCreateMutex();
    }

#ifndef SD_CARD_ONLY
    initLittleFS();
#endif

#ifdef HAS_SDCARD
    if(!sd_init_done) {
        xSemaphoreTake(fsMutex, portMAX_DELAY);
        initSDCard();
        xSemaphoreGive(fsMutex);
        sd_init_done = true;
    }
#ifndef SD_CARD_ONLY
    copyIfNeeded("/index.html");
    copyIfNeeded("/fonts");
    copyIfNeeded("/www");
    copyIfNeeded("/tagtypes");
    copyIfNeeded("/AP_FW_Pack.bin");
    copyIfNeeded("/tag_md5_db.json");
    copyIfNeeded("/update_actions.json");
    copyIfNeeded("/content_template.json");
#endif
#endif

    if (!contentFS->exists("/current")) {
        contentFS->mkdir("/current");
    }
    if (!contentFS->exists("/temp")) {
        contentFS->mkdir("/temp");
    }
}

void DynStorage::end() {
#ifdef HAS_SDCARD
#ifndef SD_CARD_ONLY
    initLittleFS();
#endif
#ifdef SD_CARD_SDMMC
#ifndef SD_CARD_ONLY
    contentFS = &LittleFS;
#endif
    SD_MMC.end();
    sd_init_done = false;
#else
#ifndef SD_CARD_ONLY
    if (SD_CARD_CLK == FLASHER_AP_CLK ||
        SD_CARD_MISO == FLASHER_AP_MISO ||
        SD_CARD_MOSI == FLASHER_AP_MOSI) {
        Serial.println("Tearing down SD card connection");

        copyBetweenFS(*contentFS, "/tag_md5_db.json", LittleFS);
        copyBetweenFS(*contentFS, "/AP_FW_Pack.bin", LittleFS);
        if (contentFS->exists("/AP_force_flash.bin")) {
            copyBetweenFS(*contentFS, "/AP_force_flash.bin", LittleFS);
            contentFS->remove("/AP_force_flash.bin");
        }
        Serial.println("Swapping to LittleFS");

        contentFS = &LittleFS;
    }
#endif
#endif
#endif
}

fs::FS* contentFS;
DynStorage Storage;
