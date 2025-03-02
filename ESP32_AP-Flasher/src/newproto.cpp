#include "newproto.h"

#include <Arduino.h>
#include <FS.h>
#include <HTTPClient.h>
#include <MD5Builder.h>
#include <time.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "serialap.h"
#include "settings.h"
#include "storage.h"
#include "system.h"
#include "tag_db.h"
#include "tagdata.h"
#include "udp.h"
#include "util.h"
#include "web.h"

extern uint16_t sendBlock(const void* data, const uint16_t len);
extern UDPcomm udpsync;
std::vector<PendingItem> pendingQueue;
std::mutex queueMutex;

void addCRC(void* p, uint8_t len) {
    uint8_t total = 0;
    for (uint8_t c = 1; c < len; c++) {
        total += ((uint8_t*)p)[c];
    }
    ((uint8_t*)p)[0] = total;
}
bool checkCRC(void* p, uint8_t len) {
    uint8_t total = 0;
    for (uint8_t c = 1; c < len; c++) {
        total += ((uint8_t*)p)[c];
    }
    return ((uint8_t*)p)[0] == total;
}

uint8_t* getDataForFile(fs::File& file) {
    const size_t fileSize = file.size();
    uint8_t* ret = (uint8_t*)malloc(fileSize);
    if (ret) {
        file.seek(0);
        file.readBytes((char*)ret, fileSize);
    } else {
        Serial.printf("malloc failed for file with size %d\r\n", fileSize);
        wsErr("malloc failed while reading file");
        util::printHeap();
    }
    return ret;
}

void prepareCancelPending(const uint8_t dst[8]) {
    struct pendingData pending = {0};
    memcpy(pending.targetMac, dst, 8);
    sendCancelPending(&pending);

    tagRecord* taginfo = tagRecord::findByMAC(dst);
    if (taginfo == nullptr) {
        if (config.lock) return;
        wsErr("Tag not found, this shouldn't happen.");
        return;
    }
    clearPending(taginfo);
    while (dequeueItem(dst)) {
    };
    taginfo->pendingCount = countQueueItem(dst);
    wsSendTaginfo(dst, SYNC_TAGSTATUS);
}

void prepareIdleReq(const uint8_t* dst, uint16_t nextCheckin) {
    if (nextCheckin > 0 && countQueueItem(dst) == 0) {
        struct pendingData pending = {0};
        memcpy(pending.targetMac, dst, 8);
        pending.availdatainfo.dataType = DATATYPE_NOUPDATE;
        pending.availdatainfo.nextCheckIn = nextCheckin;
        pending.attemptsLeft = 10 + config.maxsleep;

        Serial.printf(">SDA %02X%02X%02X%02X%02X%02X%02X%02X sleeping %d minutes\r\n", dst[7], dst[6], dst[5], dst[4], dst[3], dst[2], dst[1], dst[0], nextCheckin);
        sendDataAvail(&pending);
    }
}

void prepareDataAvail(const uint8_t* dst) {
    // resend
    tagRecord* taginfo = tagRecord::findByMAC(dst);
    if (taginfo == nullptr) {
        if (config.lock) return;
        wsErr("Tag not found, this shouldn't happen.");
        return;
    }

    taginfo->pendingCount++;
    taginfo->pendingIdle = 0;

    struct pendingData pending = {0};
    memcpy(pending.targetMac, dst, 8);
    pending.availdatainfo.dataSize = taginfo->len;
    pending.availdatainfo.dataType = taginfo->dataType;
    pending.availdatainfo.nextCheckIn = 0;
    memcpy(&pending.availdatainfo.dataVer, taginfo->md5, sizeof(uint64_t));
    pending.attemptsLeft = 10;

    queueDataAvail(&pending, !taginfo->isExternal);
    if (taginfo->isExternal) {
        udpsync.netSendDataAvail(&pending);
    }

    wsSendTaginfo(dst, SYNC_TAGSTATUS);
}

void prepareDataAvail(uint8_t* data, uint16_t len, uint8_t dataType, const uint8_t* dst) {
    tagRecord* taginfo = tagRecord::findByMAC(dst);
    if (taginfo == nullptr) {
        if (config.lock) return;
        wsErr("Tag not found, this shouldn't happen.");
        return;
    }

    clearPending(taginfo);
    taginfo->data = (uint8_t*)malloc(len);
    if (taginfo->data == nullptr) {
        wsErr("no memory allocation for data");
        return;
    }

    uint8_t md5bytes[16];
    {
        MD5Builder md5;
        md5.begin();
        md5.add(data, len);
        md5.calculate();
        md5.getBytes(md5bytes);
    }

    memcpy(taginfo->data, data, len);
    taginfo->pendingCount++;
    taginfo->len = len;
    taginfo->pendingIdle = 0;
    taginfo->filename = String();
    taginfo->dataType = dataType;

    struct pendingData pending = {0};
    memcpy(pending.targetMac, dst, 8);
    pending.availdatainfo.dataSize = len;
    pending.availdatainfo.dataType = dataType;
    pending.availdatainfo.nextCheckIn = 5 | 0x8000;  // 5 seconds
    pending.availdatainfo.dataVer = *((uint64_t*)md5bytes);
    pending.attemptsLeft = 10;

    queueDataAvail(&pending, !taginfo->isExternal);
    if (taginfo->isExternal) {
        udpsync.netSendDataAvail(&pending);
    }

    wsSendTaginfo(dst, SYNC_TAGSTATUS);
}

bool prepareDataAvail(String& filename, uint8_t dataType, uint8_t dataTypeArgument, const uint8_t* dst, uint16_t nextCheckin, bool resend) {
    if ((nextCheckin & 0x8000) == 0 && nextCheckin > config.maxsleep) nextCheckin = config.maxsleep;
    if ((nextCheckin & 0x8000) == 0 && wsClientCount() && (config.stopsleep == 1)) nextCheckin = 0;
#ifdef HAS_TFT
    if (filename == "direct") {
        char dst_path[64];
        sprintf(dst_path, "/current/%02X%02X%02X%02X%02X%02X%02X%02X.raw\0", dst[7], dst[6], dst[5], dst[4], dst[3], dst[2], dst[1], dst[0]);
        contentFS->remove(dst_path);
        return true;
    }
#endif

    tagRecord* taginfo = tagRecord::findByMAC(dst);
    if (taginfo == nullptr) {
        if (config.lock) return true;
        wsErr("Tag not found, this shouldn't happen.");
        return true;
    }

    if (!filename.startsWith("/")) {
        filename = "/" + filename;
    }

    if (!contentFS->exists(filename)) {
        wsErr("File not found. " + filename);
        return false;
    }

    fs::File file = contentFS->open(filename);
    uint32_t filesize = file.size();
    if (filesize == 0) {
        file.close();
        wsErr("File has size 0. " + filename);
        return false;
    }

    uint8_t md5bytes[16];
    {
        MD5Builder md5;
        md5.begin();
        md5.addStream(file, filesize);
        md5.calculate();
        md5.getBytes(md5bytes);
    }

    file.close();

    if (memcmp(md5bytes, taginfo->md5, 8) == 0) {
        wsLog("new image is the same as current image. not updating tag.");
        wsSendTaginfo(dst, SYNC_TAGSTATUS);
        if (contentFS->exists(filename) && resend == false) {
            contentFS->remove(filename);
        }
        return true;
    }

    if (dataType != DATATYPE_FW_UPDATE) {
        char dst_path[64];
        sprintf(dst_path, "/current/%02X%02X%02X%02X%02X%02X%02X%02X_%lu.pending", dst[7], dst[6], dst[5], dst[4], dst[3], dst[2], dst[1], dst[0], millis() % 1000000);
        if (contentFS->exists(dst_path)) {
            contentFS->remove(dst_path);
        }
        if (resend == false) {
            contentFS->rename(filename, dst_path);
            filename = String(dst_path);
            wsLog("new image: " + filename);
        }

        time_t now;
        time(&now);

        taginfo->pendingIdle = (nextCheckin & 0x8000) ? (nextCheckin & 0x7FFF) + 5 : (nextCheckin * 60) + 60;
        clearPending(taginfo);
    } else {
        wsLog("firmware upload pending");
        clearPending(taginfo);
    }
    taginfo->filename = filename;
    taginfo->len = filesize;
    taginfo->dataType = dataType;
    taginfo->pendingCount++;

    struct pendingData pending = {0};
    memcpy(pending.targetMac, dst, 8);
    pending.availdatainfo.dataType = dataType;
    pending.availdatainfo.dataVer = *((uint64_t*)md5bytes);
    pending.availdatainfo.dataSize = filesize;
    pending.availdatainfo.dataTypeArgument = dataTypeArgument;
    pending.availdatainfo.nextCheckIn = nextCheckin;
    pending.attemptsLeft = MAX_XFER_ATTEMPTS;
    checkMirror(taginfo, &pending);
    queueDataAvail(&pending, !taginfo->isExternal);
    if (taginfo->isExternal == false) {
        Serial.printf(">SDA %02X%02X%02X%02X%02X%02X%02X%02X TYPE 0x%02X\r\n", dst[7], dst[6], dst[5], dst[4], dst[3], dst[2], dst[1], dst[0], pending.availdatainfo.dataType);
    } else {
        udpsync.netSendDataAvail(&pending);
    }

    wsSendTaginfo(dst, SYNC_TAGSTATUS);
    return true;
}

void prepareExternalDataAvail(struct pendingData* pending, IPAddress remoteIP) {
    tagRecord* taginfo = tagRecord::findByMAC(pending->targetMac);
    if (taginfo == nullptr) {
        return;
    }
    if (taginfo->isExternal == false) {
        switch (pending->availdatainfo.dataType) {
            case DATATYPE_IMG_DIFF:
            case DATATYPE_IMG_ZLIB:
            case DATATYPE_IMG_RAW_1BPP:
            case DATATYPE_IMG_RAW_2BPP:
            case DATATYPE_IMG_G5:
            case DATATYPE_IMG_RAW_3BPP:
            case DATATYPE_IMG_RAW_4BPP: {
                char hexmac[17];
                mac2hex(pending->targetMac, hexmac);
                String filename = "/current/" + String(hexmac) + "_" + String(millis() % 1000000) + ".pending";
                char md5[17];
                mac2hex(reinterpret_cast<uint8_t*>(&pending->availdatainfo.dataVer), md5);
                char imageUrl[80];
                snprintf(imageUrl, sizeof(imageUrl), "http://%s/getdata?mac=%s&md5=%s", remoteIP.toString().c_str(), hexmac, md5);
                wsLog("prepareExternalDataAvail GET " + String(imageUrl));
                HTTPClient http;
                http.begin(imageUrl);
                int httpCode = http.GET();
                if (httpCode == 200) {
                    xSemaphoreTake(fsMutex, portMAX_DELAY);
                    File file = contentFS->open(filename, "w");
                    http.writeToStream(&file);
                    file.close();
                    xSemaphoreGive(fsMutex);
                } else if (httpCode == 404) {
                    snprintf(imageUrl, sizeof(imageUrl), "http://%s/current/%s.raw", remoteIP.toString().c_str(), hexmac);
                    // imageUrl = "http://" + remoteIP.toString() + "/current/" + String(hexmac) + ".raw";
                    http.end();
                    http.begin(imageUrl);
                    httpCode = http.GET();
                    if (httpCode == 200) {
                        xSemaphoreTake(fsMutex, portMAX_DELAY);
                        File file = contentFS->open(filename, "w");
                        http.writeToStream(&file);
                        file.close();
                        xSemaphoreGive(fsMutex);
                    }
                } else {
                    logLine("prepareExternalDataAvail " + String(imageUrl) + " error " + String(httpCode));
                    wsLog("error " + String(httpCode));
                }
                http.end();

                fs::File file = contentFS->open(filename);
                uint32_t filesize = file.size();
                if (filesize == 0) {
                    file.close();
                    wsErr("Remote file not found. " + filename);
                    return;
                }

                uint8_t md5bytes[16];
                {
                    MD5Builder md5;
                    md5.begin();
                    md5.addStream(file, filesize);
                    md5.calculate();
                    md5.getBytes(md5bytes);
                }

                file.close();
                clearPending(taginfo);
                taginfo->filename = filename;
                taginfo->len = filesize;
                taginfo->dataType = pending->availdatainfo.dataType;
                taginfo->pendingCount++;
                break;
            }
            case DATATYPE_NFC_RAW_CONTENT:
            case DATATYPE_NFC_URL_DIRECT: {
                char hexmac[17];
                mac2hex(pending->targetMac, hexmac);
                char dataUrl[80];
                char md5[17];
                mac2hex(reinterpret_cast<uint8_t*>(&pending->availdatainfo.dataVer), md5);
                snprintf(dataUrl, sizeof(dataUrl), "http://%s/getdata?mac=%s&md5=%s", remoteIP.toString().c_str(), hexmac, md5);
                wsLog("GET " + String(dataUrl));
                HTTPClient http;
                logLine("http DATATYPE_NFC_* " + String(dataUrl));
                http.begin(dataUrl);
                int httpCode = http.GET();
                if (httpCode == 200) {
                    size_t len = http.getSize();
                    if (len > 0) {
                        clearPending(taginfo);
                        taginfo->data = new uint8_t[len];
                        WiFiClient* stream = http.getStreamPtr();
                        stream->readBytes(taginfo->data, len);
                        taginfo->dataType = pending->availdatainfo.dataType;
                        taginfo->pendingCount++;
                        taginfo->len = len;
                    }
                }
                http.end();
                break;
            }
            case DATATYPE_FW_UPDATE: {
                return;
            }
        }
        checkMirror(taginfo, pending);
        queueDataAvail(pending, !taginfo->isExternal);

        wsSendTaginfo(pending->targetMac, SYNC_NOSYNC);
    }
}

void processBlockRequest(struct espBlockRequest* br) {
    uint32_t t = millis();
    if (config.runStatus == RUNSTATUS_STOP) {
        return;
    }
    if (!checkCRC(br, sizeof(struct espBlockRequest))) {
        Serial.print("Failed CRC on a blockrequest received by the AP");
        return;
    }

    PendingItem* queueItem = getQueueItem(br->src, br->ver);
    if (queueItem == nullptr) {
        prepareCancelPending(br->src);
        Serial.printf("blockrequest: couldn't find taginfo %02X%02X%02X%02X%02X%02X%02X%02X\r\n", br->src[7], br->src[6], br->src[5], br->src[4], br->src[3], br->src[2], br->src[1], br->src[0]);
        return;
    }
    if (queueItem->data == nullptr) {
        fs::File file = contentFS->open(queueItem->filename);
        if (!file) {
            Serial.print("No current file. " + String(queueItem->filename) + " Canceling request\r\n");
            prepareCancelPending(br->src);
            return;
        }
        queueItem->data = getDataForFile(file);
        Serial.println("Reading file " + String(queueItem->filename) + " in  " + String(millis() - t) + "ms");
        file.close();
    }

    // check if we're not exceeding max blocks (to prevent sendBlock from exceeding its boundary)
    uint8_t totalblocks = (queueItem->len / BLOCK_DATA_SIZE);
    if (queueItem->len % BLOCK_DATA_SIZE) totalblocks++;
    if (br->blockId >= totalblocks) {
        br->blockId = totalblocks - 1;
    }
    uint32_t len = queueItem->len - (BLOCK_DATA_SIZE * br->blockId);
    if (len > BLOCK_DATA_SIZE) len = BLOCK_DATA_SIZE;
    uint16_t checksum = sendBlock(queueItem->data + (br->blockId * BLOCK_DATA_SIZE), len);
    char buffer[150];
    sprintf(buffer, "%02X%02X%02X%02X%02X%02X%02X%02X block request %s block %d, len %d checksum %u\0", br->src[7], br->src[6], br->src[5], br->src[4], br->src[3], br->src[2], br->src[1], br->src[0], queueItem->filename, br->blockId, len, checksum);
    wsLog((String)buffer);
    Serial.printf("<RQB file %s block %d, len %d checksum %u\r\n\0", queueItem->filename, br->blockId, len, checksum);
}

void processXferComplete(struct espXferComplete* xfc, bool local) {
    if (config.runStatus == RUNSTATUS_STOP) {
        return;
    }
    char buffer[64];
    sprintf(buffer, "%02X%02X%02X%02X%02X%02X%02X%02X reports xfer complete\r\n\0", xfc->src[7], xfc->src[6], xfc->src[5], xfc->src[4], xfc->src[3], xfc->src[2], xfc->src[1], xfc->src[0]);
    wsLog((String)buffer);

    if (local) {
        Serial.printf("<XFC %02X%02X%02X%02X%02X%02X%02X%02X\r\n", xfc->src[7], xfc->src[6], xfc->src[5], xfc->src[4], xfc->src[3], xfc->src[2], xfc->src[1], xfc->src[0]);
    } else {
        Serial.printf("<REMOTE XFC %02X%02X%02X%02X%02X%02X%02X%02X\r\n", xfc->src[7], xfc->src[6], xfc->src[5], xfc->src[4], xfc->src[3], xfc->src[2], xfc->src[1], xfc->src[0]);
    }

    time_t now;
    time(&now);

    char dst_path[64];
    sprintf(dst_path, "/current/%02X%02X%02X%02X%02X%02X%02X%02X.raw\0", xfc->src[7], xfc->src[6], xfc->src[5], xfc->src[4], xfc->src[3], xfc->src[2], xfc->src[1], xfc->src[0]);

    uint8_t md5bytes[16];
    PendingItem* queueItem = getQueueItem(xfc->src);
    if (queueItem != nullptr) {
        if (contentFS->exists(dst_path) && contentFS->exists(queueItem->filename)) {
            contentFS->remove(dst_path);
        }
        if (contentFS->exists(queueItem->filename)) {
            uint8_t dataType = queueItem->pendingdata.availdatainfo.dataType;
            if (config.preview && dataType != DATATYPE_FW_UPDATE && dataType != DATATYPE_NOUPDATE) {
                contentFS->rename(queueItem->filename, String(dst_path));
                }
            else {
                if (queueItem->pendingdata.availdatainfo.dataType != DATATYPE_FW_UPDATE) contentFS->remove(queueItem->filename);
            }
        }
        memcpy(md5bytes, &queueItem->pendingdata.availdatainfo.dataVer, sizeof(uint64_t));
        memset(md5bytes + sizeof(uint64_t), 0, 16 - sizeof(uint64_t));
        dequeueItem(xfc->src);
    }

    tagRecord* taginfo = tagRecord::findByMAC(xfc->src);
    if (taginfo != nullptr) {
        clearPending(taginfo);
        memcpy(taginfo->md5, md5bytes, sizeof(md5bytes));
        taginfo->updateCount++;
        taginfo->updateLast = now;
        taginfo->pendingCount = countQueueItem(xfc->src);
        taginfo->wakeupReason = 0;
        if (taginfo->contentMode == 12 && local == false) {
            if (contentFS->exists(dst_path)) {
                contentFS->remove(dst_path);
            }
        }
        if (taginfo->contentMode == 5 || taginfo->contentMode == 17 || taginfo->contentMode == 18) {
            popTagInfo(xfc->src);
            taginfo = tagRecord::findByMAC(xfc->src);
            taginfo->nextupdate = now;
        }
    }

    // more in the queue?
    if (local) checkQueue(xfc->src);

    wsSendTaginfo(xfc->src, SYNC_TAGSTATUS);
    if (local) udpsync.netProcessXferComplete(xfc);
}

void processXferTimeout(struct espXferComplete* xfc, bool local) {
    if (config.runStatus == RUNSTATUS_STOP) {
        return;
    }
    char buffer[64];
    sprintf(buffer, "< %02X%02X%02X%02X%02X%02X%02X%02X xfer timeout\r\n\0", xfc->src[7], xfc->src[6], xfc->src[5], xfc->src[4], xfc->src[3], xfc->src[2], xfc->src[1], xfc->src[0]);
    wsErr((String)buffer);

    if (local) {
        Serial.printf("<XTO %02X%02X%02X%02X%02X%02X%02X%02X\r\n", xfc->src[7], xfc->src[6], xfc->src[5], xfc->src[4], xfc->src[3], xfc->src[2], xfc->src[1], xfc->src[0]);
    } else {
        Serial.printf("<REMOTE XTO %02X%02X%02X%02X%02X%02X%02X%02X\r\n", xfc->src[7], xfc->src[6], xfc->src[5], xfc->src[4], xfc->src[3], xfc->src[2], xfc->src[1], xfc->src[0]);
    }

    time_t now;
    time(&now);
    tagRecord* taginfo = tagRecord::findByMAC(xfc->src);
    if (taginfo != nullptr) {
        taginfo->pendingIdle = 60;
        clearPending(taginfo);
        while (dequeueItem(xfc->src)) {
        };
    }

    checkQueue(xfc->src);

    wsSendTaginfo(xfc->src, SYNC_TAGSTATUS);
    if (local) udpsync.netProcessXferTimeout(xfc);
}

void processDataReq(struct espAvailDataReq* eadr, bool local, IPAddress remoteIP) {
    if (config.runStatus == RUNSTATUS_STOP) {
        return;
    }
    char buffer[64];

    char hexmac[17];
    mac2hex(eadr->src, hexmac);

    tagRecord* taginfo = tagRecord::findByMAC(eadr->src);
    if (taginfo == nullptr) {
        if (config.lock == 1 || (config.lock == 2 && eadr->adr.wakeupReason != WAKEUP_REASON_FIRSTBOOT)) return;
#ifdef HAS_SUBGHZ
        if (apInfo.hasSubGhz && eadr->adr.currentChannel > 0 && eadr->adr.currentChannel == apInfo.SubGhzChannel) {
            // Empty intentionally
        } else
#endif
            if (local == true && eadr->adr.currentChannel > 0 && eadr->adr.currentChannel != apInfo.channel) {
            Serial.printf("Tag %s reports illegal channel %d\r\n", hexmac, eadr->adr.currentChannel);
            return;
        }
        taginfo = new tagRecord;
        memcpy(taginfo->mac, eadr->src, sizeof(taginfo->mac));
        taginfo->pendingCount = 0;
        tagDB.push_back(taginfo);
    }
    time_t now;
    time(&now);

    if (!local) {
        if (taginfo->isExternal == false) {
            wsLog("moved AP from local to external " + String(hexmac));
            taginfo->isExternal = true;
        }
        taginfo->apIp = remoteIP;
    } else {
        if (taginfo->isExternal == true) {
            wsLog("moved AP from external to local " + String(hexmac));
            taginfo->isExternal = false;
        }
        taginfo->apIp = IPAddress(0, 0, 0, 0);
    }

    if (taginfo->pendingIdle == 0) {
        taginfo->expectedNextCheckin = now + 60;
    } else if (taginfo->pendingIdle == 9999) {
        taginfo->expectedNextCheckin = 3216153600;
        taginfo->pendingIdle = 0;
    } else {
        taginfo->expectedNextCheckin = now + taginfo->pendingIdle;
        taginfo->pendingIdle = 0;
    }
    taginfo->lastseen = now;

    if (eadr->adr.lastPacketRSSI != 0) {
        if (eadr->adr.wakeupReason >= 0xE0) {
            if (taginfo->pendingCount == 0) {
                taginfo->nextupdate = 0;
                memset(taginfo->md5, 0, sizeof(taginfo->md5));
            }

            if (local) {
                const char* reason = "";
                if (eadr->adr.wakeupReason == WAKEUP_REASON_FIRSTBOOT)
                    reason = "Booting";
                else if (eadr->adr.wakeupReason == WAKEUP_REASON_NETWORK_SCAN)
                    reason = "Network scan";
                else if (eadr->adr.wakeupReason == WAKEUP_REASON_WDT_RESET)
                    reason = "Watchdog reset";
                else if (eadr->adr.wakeupReason == WAKEUP_REASON_FAILED_OTA_FW)
                    reason = "Firmware update rejected";
                sprintf(buffer, "%02X%02X%02X%02X%02X%02X%02X%02X %s", eadr->src[7], eadr->src[6], eadr->src[5], eadr->src[4], eadr->src[3], eadr->src[2], eadr->src[1], eadr->src[0], reason);
                logLine(buffer);
            }
        }

        taginfo->LQI = eadr->adr.lastPacketLQI;
        taginfo->hwType = eadr->adr.hwType;
        taginfo->RSSI = eadr->adr.lastPacketRSSI;
        taginfo->temperature = eadr->adr.temperature;
        taginfo->batteryMv = eadr->adr.batteryMv;
        taginfo->hwType = eadr->adr.hwType;
        taginfo->wakeupReason = eadr->adr.wakeupReason;
        taginfo->capabilities = eadr->adr.capabilities;
        taginfo->currentChannel = eadr->adr.currentChannel;
        taginfo->tagSoftwareVersion = eadr->adr.tagSoftwareVersion;
    }
    if (local) {
        sprintf(buffer, "<ADR %02X%02X%02X%02X%02X%02X%02X%02X\r\n\0", eadr->src[7], eadr->src[6], eadr->src[5], eadr->src[4], eadr->src[3], eadr->src[2], eadr->src[1], eadr->src[0]);
        Serial.print(buffer);
    }

    if (local) {
        wsSendTaginfo(eadr->src, SYNC_TAGSTATUS);
        udpsync.netProcessDataReq(eadr);
    } else {
        wsSendTaginfo(eadr->src, SYNC_NOSYNC);
    }
}

void processTagReturnData(struct espTagReturnData* trd, uint8_t len, bool local) {
    if (!checkCRC(trd, len)) {
        return;
    }

    const uint8_t payloadLength = trd->len - 11;

    // Replace this stuff with something that handles the data coming from the tag. This is here for demo purposes!
    char buffer[64];
    sprintf(buffer, "<TRD %02X%02X%02X%02X%02X%02X%02X%02X\r\n", trd->src[7], trd->src[6], trd->src[5], trd->src[4], trd->src[3], trd->src[2], trd->src[1], trd->src[0]);
    wsLog((String)buffer);
    sprintf(buffer, "TRD Data: len=%d, type=%d, ver=0x%08X\r\n", payloadLength, trd->returnData.dataType, trd->returnData.dataVer);
    wsLog((String)buffer);

#ifndef SAVE_SPACE
    TagData::parse(trd->src, trd->returnData.dataType, trd->returnData.data, payloadLength);
#endif
}

void refreshAllPending() {
    for (int16_t c = 0; c < tagDB.size(); c++) {
        tagRecord* taginfo = tagDB.at(c);
        if (taginfo->pendingCount > 0 && taginfo->version == 0) {
            clearPending(taginfo);
            taginfo->nextupdate = 0;
            wsSendTaginfo(taginfo->mac, SYNC_TAGSTATUS);
        }
    }
};

void updateContent(const uint8_t* dst) {
    tagRecord* taginfo = tagRecord::findByMAC(dst);
    if (taginfo != nullptr) {
        clearPending(taginfo);
        memset(taginfo->md5, 0, sizeof(taginfo->md5));
        taginfo->nextupdate = 0;
        wsSendTaginfo(taginfo->mac, SYNC_TAGSTATUS);
    }
}

void setAPchannel() {
    bool bSendRadioLayer = false;
    struct espSetChannelPower tmp;
    if (config.channel == 0) {
        // trigger channel autoselect
        UDPcomm udpsync;
        udpsync.getAPList();
    } else {
        if (curChannel.channel != config.channel) {
            curChannel.channel = config.channel;
            bSendRadioLayer = true;
        }
    }
#ifdef HAS_SUBGHZ
    if (curChannel.subghzchannel != config.subghzchannel) {
        curChannel.subghzchannel = config.subghzchannel;
        apInfo.SubGhzChannel = config.subghzchannel;
        bSendRadioLayer = true;
    }
#endif
    if (bSendRadioLayer) {
        tmp = curChannel;
        if (config.channel == 0) {
            tmp.channel = 0;  // don't set the 802.15.4 channel
        }
        sendChannelPower(&tmp);
    }
}

bool sendAPSegmentedData(const uint8_t* dst, String data, uint16_t icons, bool inverted, bool local) {
    struct pendingData pending = {0};
    memcpy(pending.targetMac, dst, 8);
    pending.availdatainfo.dataType = DATATYPE_UK_SEGMENTED;
    pending.availdatainfo.dataSize = icons << 16;
    memcpy((void*)&(pending.availdatainfo.dataVer), data.c_str(), 10);
    pending.availdatainfo.dataTypeArgument = inverted;
    pending.availdatainfo.nextCheckIn = 0;
    pending.attemptsLeft = MAX_XFER_ATTEMPTS;
    Serial.printf(">AP Segmented Data %02X%02X%02X%02X%02X%02X%02X%02X\r\n\0", dst[7], dst[6], dst[5], dst[4], dst[3], dst[2], dst[1], dst[0]);
    if (local) {
        return queueDataAvail(&pending, true);
    } else {
        queueDataAvail(&pending, false);
        udpsync.netSendDataAvail(&pending);
        return true;
    }
}

bool showAPSegmentedInfo(const uint8_t* dst, bool local) {
    struct pendingData pending = {0};
    memcpy(pending.targetMac, dst, 8);
    pending.availdatainfo.dataType = DATATYPE_UK_SEGMENTED;
    pending.availdatainfo.dataSize = 0x00;
    pending.availdatainfo.dataVer = 0x00;
    pending.availdatainfo.dataTypeArgument = 0;
    pending.availdatainfo.nextCheckIn = 0;
    pending.attemptsLeft = MAX_XFER_ATTEMPTS;
    Serial.printf(">SDA %02X%02X%02X%02X%02X%02X%02X%02X\r\n\0", dst[7], dst[6], dst[5], dst[4], dst[3], dst[2], dst[1], dst[0]);
    if (local) {
        return queueDataAvail(&pending, true);
    } else {
        queueDataAvail(&pending, false);
        udpsync.netSendDataAvail(&pending);
        return true;
    }
}

bool sendTagCommand(const uint8_t* dst, uint8_t cmd, bool local, const uint8_t* payload) {
    struct pendingData pending = {0};
    memcpy(pending.targetMac, dst, 8);
    pending.availdatainfo.dataType = DATATYPE_COMMAND_DATA;
    pending.availdatainfo.dataTypeArgument = cmd;
    pending.availdatainfo.nextCheckIn = 0;
    if (payload != nullptr) {
        memcpy(&pending.availdatainfo.dataVer, payload, sizeof(uint64_t));
        memcpy(&pending.availdatainfo.dataSize, payload + sizeof(uint64_t), sizeof(uint32_t));
    }
    pending.attemptsLeft = MAX_XFER_ATTEMPTS;
    Serial.printf(">Tag CMD %02X%02X%02X%02X%02X%02X%02X%02X\r\n\0", dst[7], dst[6], dst[5], dst[4], dst[3], dst[2], dst[1], dst[0]);

    tagRecord* taginfo = tagRecord::findByMAC(dst);
    if (taginfo != nullptr) {
        taginfo->pendingCount++;
        wsSendTaginfo(taginfo->mac, SYNC_TAGSTATUS);
    }

    if (local) {
        return queueDataAvail(&pending, true);
    } else {
        queueDataAvail(&pending, false);
        udpsync.netSendDataAvail(&pending);
        return true;
    }
}

bool sendTagMac(const uint8_t* dst, const uint64_t newmac, bool local) {
    struct pendingData pending = {0};
    memcpy(pending.targetMac, dst, 8);
    pending.availdatainfo.dataType = DATATYPE_COMMAND_DATA;
    pending.availdatainfo.dataTypeArgument = 0x23;
    pending.availdatainfo.nextCheckIn = 0;
    

    pending.availdatainfo.dataVer = newmac;
    pending.availdatainfo.dataSize = 0;

    pending.attemptsLeft = MAX_XFER_ATTEMPTS;
    Serial.printf(">Tag %02X%02X%02X%02X%02X%02X%02X%02X Mac set\r\n\0", dst[7], dst[6], dst[5], dst[4], dst[3], dst[2], dst[1], dst[0]);

    tagRecord* taginfo = tagRecord::findByMAC(dst);
    if (taginfo != nullptr) {
        taginfo->pendingCount++;
        wsSendTaginfo(taginfo->mac, SYNC_TAGSTATUS);
    }

    if (local) {
        return queueDataAvail(&pending, true);
    } else {
        queueDataAvail(&pending, false);
        udpsync.netSendDataAvail(&pending);
        return true;
    }
}

void updateTaginfoitem(struct TagInfo* taginfoitem, IPAddress remoteIP) {
    tagRecord* taginfo = tagRecord::findByMAC(taginfoitem->mac);

    if (taginfo == nullptr) {
        if (config.lock) return;
        taginfo = new tagRecord;
        memcpy(taginfo->mac, taginfoitem->mac, sizeof(taginfo->mac));
        taginfo->pendingCount = 0;
        tagDB.push_back(taginfo);
    }
    tagRecord initialTagInfo = *taginfo;

    switch (taginfoitem->syncMode) {
        case SYNC_USERCFG:
            taginfo->alias = String(taginfoitem->alias);
            taginfo->nextupdate = taginfoitem->nextupdate;
            break;
        case SYNC_TAGSTATUS:
            taginfo->lastseen = taginfoitem->lastseen;
            taginfo->nextupdate = taginfoitem->nextupdate;
            taginfo->pendingCount = taginfoitem->pendingCount;
            taginfo->expectedNextCheckin = taginfoitem->expectedNextCheckin;
            taginfo->hwType = taginfoitem->hwType;
            taginfo->wakeupReason = taginfoitem->wakeupReason;
            taginfo->capabilities = taginfoitem->capabilities;
            taginfo->pendingIdle = taginfoitem->pendingIdle;
            break;
    }

    char hexmac[17];
    mac2hex(taginfo->mac, hexmac);
    if (taginfo->contentMode != 12 && taginfoitem->contentMode != 12 && taginfoitem->contentMode != 0) {
        wsLog("Remote AP at " + remoteIP.toString() + " takes control over tag " + String(hexmac));
        taginfo->contentMode = 12;
    }

    if (taginfoitem->syncMode == SYNC_DELETE) {
        taginfo->contentMode = 255;
        wsSendTaginfo(taginfo->mac, SYNC_NOSYNC);
        deleteRecord(taginfoitem->mac);
    } else {
        bool hasChanges = (memcmp(&initialTagInfo, taginfo, sizeof(tagRecord)) != 0);
        if (hasChanges) {
            wsSendTaginfo(taginfo->mac, SYNC_NOSYNC);
        }
    }
}

bool checkMirror(struct tagRecord* taginfo, struct pendingData* pending) {
    for (int16_t c = 0; c < tagDB.size(); c++) {
        tagRecord* taginfo2 = tagDB.at(c);
        if (taginfo2->contentMode == 20 && taginfo2->version == 0) {
            JsonDocument doc;
            deserializeJson(doc, taginfo2->modeConfigJson);
            JsonObject cfgobj = doc.as<JsonObject>();
            uint8_t mac[8] = {0};
            if (hex2mac(cfgobj["mac"], mac) && memcmp(mac, taginfo->mac, sizeof(mac)) == 0) {
                if (taginfo->data == nullptr) {
                    fs::File file = contentFS->open(taginfo->filename);
                    if (!file) {
                        return false;
                    }
                    taginfo->data = getDataForFile(file);
                    file.close();
                }

                clearPending(taginfo2);
                taginfo2->expectedNextCheckin = taginfo->expectedNextCheckin;
                taginfo2->filename = taginfo->filename;
                taginfo2->len = taginfo->len;
                taginfo2->data = taginfo->data;  // copy buffer pointer
                taginfo2->dataType = taginfo->dataType;
                taginfo2->pendingCount++;
                taginfo2->nextupdate = 3216153600;

                struct pendingData pending2 = {0};
                memcpy(pending2.targetMac, taginfo2->mac, 8);
                pending2.availdatainfo.dataType = taginfo2->dataType;
                pending2.availdatainfo.dataVer = pending->availdatainfo.dataVer;
                pending2.availdatainfo.dataSize = taginfo2->len;
                pending2.availdatainfo.dataTypeArgument = pending->availdatainfo.dataTypeArgument;
                pending2.availdatainfo.nextCheckIn = pending->availdatainfo.nextCheckIn;
                pending2.attemptsLeft = pending->attemptsLeft;

                if (taginfo2->isExternal == false) {
                    queueDataAvail(&pending2, true);
                } else {
                    char dst_path[64];
                    sprintf(dst_path, "/current/%02X%02X%02X%02X%02X%02X%02X%02X_%lu.pending", taginfo2->mac[7], taginfo2->mac[6], taginfo2->mac[5], taginfo2->mac[4], taginfo2->mac[3], taginfo2->mac[2], taginfo2->mac[1], taginfo2->mac[0], millis() % 1000000);
                    xSemaphoreTake(fsMutex, portMAX_DELAY);
                    File file = contentFS->open(dst_path, "w");
                    if (file) {
                        file.write(taginfo2->data, taginfo2->len);
                        file.close();
                        xSemaphoreGive(fsMutex);
                        queueDataAvail(&pending2, false);
                        udpsync.netSendDataAvail(&pending2);
                    } else {
                        xSemaphoreGive(fsMutex);
                    }
                }

                wsSendTaginfo(taginfo2->mac, SYNC_TAGSTATUS);
            }
        }
    }
    return false;
}

void enqueueItem(struct PendingItem& item) {
    std::lock_guard<std::mutex> lock(queueMutex);
    pendingQueue.push_back(item);
}

bool dequeueItem(const uint8_t* targetMac) {
    return dequeueItem(targetMac, 0);
}

bool dequeueItem(const uint8_t* targetMac, const uint64_t dataVer) {
    std::lock_guard<std::mutex> lock(queueMutex);
    auto it = std::find_if(pendingQueue.begin(), pendingQueue.end(),
                           [targetMac, dataVer](const PendingItem& item) {
                               bool macMatches = memcmp(item.pendingdata.targetMac, targetMac, sizeof(item.pendingdata.targetMac)) == 0;
                               bool dataVerMatches = (dataVer == 0) || (dataVer == item.pendingdata.availdatainfo.dataVer);
                               return macMatches && dataVerMatches;
                           });
    if (it != pendingQueue.end()) {
        if (it->data != nullptr) {
            int datacount = 0;
            for (const PendingItem& item : pendingQueue) {
                if (item.data == it->data) {
                    datacount++;
                }
            }
            if (datacount == 1) {
                free(it->data);
            }
            it->data = nullptr;
        }
        pendingQueue.erase(it);
        return true;
    }
    return false;
}

uint16_t countQueueItem(const uint8_t* targetMac) {
    std::unique_lock<std::mutex> lock(queueMutex);
    int count = std::count_if(pendingQueue.begin(), pendingQueue.end(),
                              [targetMac](const PendingItem& item) {
                                  return memcmp(item.pendingdata.targetMac, targetMac, sizeof(item.pendingdata.targetMac)) == 0;
                              });
    return count;
}

PendingItem* getQueueItem(const uint8_t* targetMac) {
    return getQueueItem(targetMac, 0);
}

PendingItem* getQueueItem(const uint8_t* targetMac, const uint64_t dataVer) {
    auto it = std::find_if(pendingQueue.begin(), pendingQueue.end(),
                           [targetMac, dataVer](const PendingItem& item) {
                               bool macMatches = memcmp(item.pendingdata.targetMac, targetMac, sizeof(item.pendingdata.targetMac)) == 0;
                               bool dataVerMatches = (dataVer == 0) || (dataVer == item.pendingdata.availdatainfo.dataVer);
                               return macMatches && dataVerMatches;
                           });
    if (it != pendingQueue.end()) {
        return &(*it);
    } else {
        return nullptr;
    }
}

void checkQueue(const uint8_t* targetMac) {
    uint16_t queueCount;
    queueCount = countQueueItem(targetMac);
    if (queueCount > 0) {
        Serial.printf("queue: total %d elements\r\n", pendingQueue.size());
        PendingItem* queueItem = getQueueItem(targetMac);
        if (queueItem == nullptr) {
            return;
        }
        if (queueCount > 1) queueItem->pendingdata.availdatainfo.nextCheckIn = 5 | 0x8000;
        sendDataAvail(&queueItem->pendingdata);
    }
}

bool queueDataAvail(struct pendingData* pending, bool local) {
    PendingItem newPending;
    newPending.pendingdata.availdatainfo = pending->availdatainfo;
    newPending.pendingdata.attemptsLeft = pending->attemptsLeft;
    std::copy(pending->targetMac, pending->targetMac + sizeof(pending->targetMac), newPending.pendingdata.targetMac);

    tagRecord* taginfo = tagRecord::findByMAC(pending->targetMac);

    if (taginfo == nullptr) {
        return false;
    }

    std::strcpy(newPending.filename, taginfo->filename.c_str());
    if (taginfo->data != nullptr) {
        // move data pointer
        newPending.data = taginfo->data;
        taginfo->data = nullptr;
    } else {
        newPending.data = nullptr;
        
        if (pendingQueue.size() < 5) {   // maximized to 5 to save some memory
            // optional: read data early, don't wait for block request.
            fs::File file = contentFS->open(newPending.filename);
            if (file) {
                newPending.data = getDataForFile(file);
                Serial.println("Reading file " + String(newPending.filename));
                file.close();
            } else {
                Serial.println("Warning: not found: " + String(newPending.filename));
            }
        }
    }
    newPending.len = taginfo->len;

    uint8_t dataType = pending->availdatainfo.dataType;
    if (dataType != DATATYPE_FW_UPDATE && dataType != DATATYPE_NOUPDATE && pending->availdatainfo.dataTypeArgument & 0xF8 == 0x00) {
        // in case of an image (no preload), remove already queued images
        pendingQueue.erase(std::remove_if(pendingQueue.begin(), pendingQueue.end(),
                                          [pending](const PendingItem& item) {
                                              bool macMatches = memcmp(item.pendingdata.targetMac, pending->targetMac, sizeof(item.pendingdata.targetMac)) == 0;
                                              bool dataTypeArgumentMatches = (pending->availdatainfo.dataType == item.pendingdata.availdatainfo.dataType) && ((item.pendingdata.availdatainfo.dataTypeArgument & 0xF8) == 0x00);
                                              return macMatches && dataTypeArgumentMatches;
                                          }),
                           pendingQueue.end());
    }

    enqueueItem(newPending);
    taginfo->pendingCount = countQueueItem(pending->targetMac);
    if (taginfo->pendingCount == 1) {
        Serial.printf("queue item added, first in line\r\n");
        // if (local) sendDataAvail(pending);
    } else {
        Serial.printf("queue item added, total %d elements\r\n", taginfo->pendingCount);
        // to do: notify C6 to shorten the checkin time for the current SDA
    }

    if (local) checkQueue(pending->targetMac);

    return true;
}
