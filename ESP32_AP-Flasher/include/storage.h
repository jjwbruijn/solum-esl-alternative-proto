#ifndef _DYN_STORAGE_H_
#define _DYN_STORAGE_H_

#include "FS.h"

#ifdef HAS_SDCARD
#ifndef SD_CARD_SDMMC

#ifndef SD_CARD_SS
#error SD_CARD_SS UNDEFINED
#endif

#ifndef SD_CARD_CLK
#define SD_CARD_CLK 18
#endif

#ifndef SD_CARD_MISO
#define SD_CARD_MISO 19
#endif

#ifndef SD_CARD_MOSI
#define SD_CARD_MOSI 23
#endif

#endif
#endif

class DynStorage {
   public:
    DynStorage();
    void begin();
    void end();
    void listFiles();
    uint64_t freeSpace();

   private:
    bool isInited;
};

extern SemaphoreHandle_t fsMutex;
extern DynStorage Storage;
extern fs::FS *contentFS;
#ifndef SD_CARD_ONLY
extern void copyFile(File in, File out);
#endif

#endif


