#include "storage.h"

#ifdef HAS_SDCARD
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#endif

#include "LittleFS.h"

DynStorage::DynStorage() : isInited(0) {}

SemaphoreHandle_t fsMutex = NULL;

static void initLittleFS() {
    LittleFS.begin();
    contentFS = &LittleFS;
}

#ifdef HAS_SDCARD
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

uint64_t DynStorage::freeSpace(){
    this->begin();
#ifdef HAS_SDCARD
    return SD.totalBytes() - SD.usedBytes();
#endif
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

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

void DynStorage::begin() {
    if(!fsMutex) {
        fsMutex = xSemaphoreCreateMutex();
    }
    initLittleFS();

#ifdef HAS_SDCARD
    initSDCard();

    copyIfNeeded("/index.html");
    copyIfNeeded("/fonts");
    copyIfNeeded("/www");
    copyIfNeeded("/tagtypes");
    copyIfNeeded("/AP_FW_Pack.bin");
    copyIfNeeded("/tag_md5_db.json");
    copyIfNeeded("/update_actions.json");
    copyIfNeeded("/content_template.json");
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
    initLittleFS();
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
}

fs::FS* contentFS;
DynStorage Storage;
