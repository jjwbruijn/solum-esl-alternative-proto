#ifdef HAS_BLE_WRITER
#include <Arduino.h>
#include <FS.h>

#include "BLEDevice.h"
#include "newproto.h"
#include "serialap.h"
#include "settings.h"
#include "storage.h"
#include "system.h"
#include "tag_db.h"
#include "tagdata.h"
#include "udp.h"
#include "util.h"
#include "web.h"

uint8_t* Mirrorbuffer;

uint8_t gicToOEPLtype(uint8_t gicType) {
    switch (gicType) {
        case 0xA0:
            return GICI_BLE_TFT_21_BW;
            break;
        case 0x08:
            return GICI_BLE_EPD_21_BW;
            break;
        case 0x0B:
            return GICI_BLE_EPD_21_BWR;
            break;
        case 0x28:
        case 0x30:
            return GICI_BLE_EPD_29_BW;
            break;
        case 0x2B:
            return GICI_BLE_EPD_29_BWR;
            break;
        case 0x33:
            return GICI_BLE_EPD_29_BWR1;
            break;
        case 0x48:
            return GICI_BLE_EPD_BW_42;
            break;
        case 0x4B:
            return GICI_BLE_EPD_BWR_42;
            break;
        case 0x40:
            return GICI_BLE_TFT_BW_42;
            break;
        case 0x42:
            return GICI_BLE_TFT_BWR_42;
            break;
        case 0x68:
            return GICI_BLE_EPD_BW_74;
            break;
        case 0x6A:
            return GICI_BLE_EPD_BWR_74;
            break;
        case 0xEB:
            return GICI_BLE_EPD_BWR_29_SILABS;
            break;
        default:
            return GICI_BLE_UNKNOWN;
            break;
    }
}

struct BleAdvDataStruct {
    uint16_t manu_id;  // 0x1337 for us
    uint8_t version;
    uint16_t hw_type;
    uint16_t fw_version;
    uint16_t capabilities;
    uint16_t battery_mv;
    uint8_t counter;
} __packed;

bool BLE_filter_add_device(BLEAdvertisedDevice advertisedDevice) {
    Serial.print("BLE Advertised Device found: ");
    Serial.println(advertisedDevice.toString().c_str());

    uint8_t payloadData[100];
    int payloadDatalen = advertisedDevice.getPayloadLength();
    memcpy(&payloadData, (uint8_t*)advertisedDevice.getPayload(), payloadDatalen);
    Serial.printf(" Payload data: ");
    for (int i = 0; i < payloadDatalen; i++)
        Serial.printf("%02X", payloadData[i]);
    Serial.printf("\r\n");

    if (advertisedDevice.haveManufacturerData()) {
        int manuDatalen = advertisedDevice.getManufacturerData().length();
        uint8_t manuData[100];
        if (manuDatalen > sizeof(manuData))
            return false;  // Manu data too big, could never happen but better make sure here
        Serial.printf(" Address type: %02X Manu data: ", advertisedDevice.getAddressType());
        for (int i = 0; i < advertisedDevice.getManufacturerData().length(); i++)
            Serial.printf("%02X", manuData[i]);
        Serial.printf("\r\n");
#if ESP_ARDUINO_VERSION_MAJOR == 2
        memcpy(&manuData, (uint8_t*)advertisedDevice.getManufacturerData().data(), manuDatalen);
#else
        // [Nic] suggested fix for arduino 3.x by copilot, but I cannot test it 
        memcpy(&manuData, (uint8_t*)advertisedDevice.getManufacturerData().c_str(), manuDatalen);
#endif
        if (manuDatalen == 7 && manuData[0] == 0x53 && manuData[1] == 0x50) {  // Lets check for a Gicisky E-Paper display

            struct espAvailDataReq theAdvData;
            memset((uint8_t*)&theAdvData, 0x00, sizeof(espAvailDataReq));

            uint8_t macReversed[6];
            memcpy(&macReversed, (uint8_t*)advertisedDevice.getAddress().getNative(), 6);
            theAdvData.src[0] = macReversed[5];
            theAdvData.src[1] = macReversed[4];
            theAdvData.src[2] = macReversed[3];
            theAdvData.src[3] = macReversed[2];
            theAdvData.src[4] = macReversed[1];
            theAdvData.src[5] = macReversed[0];
            theAdvData.src[6] = manuData[2];  // We use this do find out what type of display we got for compression^^
            theAdvData.src[7] = manuData[6];
            theAdvData.adr.batteryMv = manuData[3] * 100;
            theAdvData.adr.lastPacketRSSI = advertisedDevice.getRSSI();
            theAdvData.adr.lastPacketLQI = advertisedDevice.getRSSI();
            theAdvData.adr.hwType = gicToOEPLtype(manuData[2]);
            theAdvData.adr.tagSoftwareVersion = manuData[4] << 8 | manuData[5];
            theAdvData.adr.capabilities = 0x00;

            processDataReq(&theAdvData, true);
            return true;
        } else if (manuDatalen >= sizeof(BleAdvDataStruct) && manuData[0] == 0x37 && manuData[1] == 0x13) {  // Lets check for a Gicisky E-Paper display
            Serial.printf("ATC BLE OEPL Detected\r\n");
            struct espAvailDataReq theAdvData;
            struct BleAdvDataStruct inAdvData;

            memset((uint8_t*)&theAdvData, 0x00, sizeof(espAvailDataReq));
            memcpy(&inAdvData, manuData, sizeof(BleAdvDataStruct));
            /*Serial.printf("manu_id %04X\r\n", inAdvData.manu_id);
            Serial.printf("version %04X\r\n", inAdvData.version);
            Serial.printf("hw_type %04X\r\n", inAdvData.hw_type);
            Serial.printf("fw_version %04X\r\n", inAdvData.fw_version);
            Serial.printf("capabilities %04X\r\n", inAdvData.capabilities);
            Serial.printf("battery_mv %u\r\n", inAdvData.battery_mv);
            Serial.printf("counter %u\r\n", inAdvData.counter);*/
            if (inAdvData.version != 1) {
                printf("Version currently not supported!\r\n");
                return false;
            }
            uint8_t macReversed[6];
            memcpy(&macReversed, (uint8_t*)advertisedDevice.getAddress().getNative(), 6);
            theAdvData.src[0] = macReversed[5];
            theAdvData.src[1] = macReversed[4];
            theAdvData.src[2] = macReversed[3];
            theAdvData.src[3] = macReversed[2];
            theAdvData.src[4] = macReversed[1];
            theAdvData.src[5] = macReversed[0];
            theAdvData.src[6] = manuData[0];  // We use this do find out what type of display we got for compression^^
            theAdvData.src[7] = manuData[1];
            theAdvData.adr.batteryMv = inAdvData.battery_mv;
            theAdvData.adr.lastPacketRSSI = advertisedDevice.getRSSI();
            theAdvData.adr.hwType = inAdvData.hw_type & 0xff;
            theAdvData.adr.tagSoftwareVersion = inAdvData.fw_version;
            theAdvData.adr.capabilities = inAdvData.capabilities & 0xff;
            processDataReq(&theAdvData, true);
            return true;
        }
    }
    if (payloadDatalen >= 17) {  // Lets check for an ATC Mi Thermometer
        uint8_t macReversed[6];
        memcpy(&macReversed, (uint8_t*)advertisedDevice.getAddress().getNative(), 6);
        if (payloadData[9] == macReversed[5] && payloadData[8] == macReversed[4] && payloadData[7] == macReversed[3]) {  // Here we found an ATC Mi Thermometer
            struct espAvailDataReq theAdvData;
            memset((uint8_t*)&theAdvData, 0x00, sizeof(espAvailDataReq));

            theAdvData.src[0] = macReversed[5];
            theAdvData.src[1] = macReversed[4];
            theAdvData.src[2] = macReversed[3];
            theAdvData.src[3] = macReversed[2];
            theAdvData.src[4] = macReversed[1];
            theAdvData.src[5] = macReversed[0];
            theAdvData.src[6] = 0x00;
            theAdvData.src[7] = 0x00;
            theAdvData.adr.batteryMv = payloadData[14] << 8 | payloadData[15];
            theAdvData.adr.temperature = (payloadData[10] << 8 | payloadData[11]) / 10;
            theAdvData.adr.lastPacketLQI = payloadData[12];
            theAdvData.adr.lastPacketRSSI = advertisedDevice.getRSSI();
            theAdvData.adr.hwType = ATC_MI_THERMOMETER;
            theAdvData.adr.tagSoftwareVersion = 0x00;
            theAdvData.adr.capabilities = 0x00;
            processDataReq(&theAdvData, true);
            Serial.printf("We got an ATC_MiThermometer via BLE\r\n");
            return true;
        }
    }
    return false;
}

bool BLE_is_image_pending(uint8_t address[8]) {
    for (int16_t c = 0; c < tagDB.size(); c++) {
        tagRecord* taginfo = tagDB.at(c);
        if (taginfo->pendingCount > 0 && taginfo->version == 0 && ((taginfo->hwType & 0xB0) == 0xB0)) {
            memcpy(address, taginfo->mac, 8);
            return true;
        }
    }
    for (int16_t c = 0; c < tagDB.size(); c++) {
        tagRecord* taginfo = tagDB.at(c);
        if (taginfo->pendingCount > 0 && taginfo->version == 0 && (taginfo->mac[7] == 0x13) && (taginfo->mac[6] == 0x37)) {
            memcpy(address, taginfo->mac, 8);
            Serial.printf("ATC BLE OEPL data Waiting\r\n");
            return true;
        }
    }
    return false;
}

uint8_t swapBits(uint8_t num) {
    uint8_t result = 0;
    for (int i = 0; i < 8; ++i) {
        result |= ((num >> i) & 0x01) << (7 - i);
    }
    return result;
}

uint32_t compress_image(uint8_t address[8], uint8_t* buffer, uint32_t max_len) {
    uint32_t t = millis();
    PendingItem* queueItem = getQueueItem(address, 0);
    if (queueItem == nullptr) {
        prepareCancelPending(address);
        Serial.printf("blockrequest: couldn't find taginfo %02X%02X%02X%02X%02X%02X%02X%02X\r\n", address[7], address[6], address[5], address[4], address[3], address[2], address[1], address[0]);
        return 0;
    }
    if (queueItem->data == nullptr) {
        fs::File file = contentFS->open(queueItem->filename);
        if (!file) {
            Serial.print("No current file. " + String(queueItem->filename) + " Canceling request\r\n");
            prepareCancelPending(address);
            return 0;
        }
        queueItem->data = getDataForFile(file);
        Serial.println("Reading file " + String(queueItem->filename) + " in  " + String(millis() - t) + "ms");
        file.close();
    }

    uint16_t giciType = (address[7] << 8) | address[6];  // here we "extract" the display info again

    uint8_t screenResolution = (giciType >> 5) & 63;
    uint8_t dispPtype = (giciType >> 3) & 3;
    uint8_t availColors = ((giciType >> 1) & 3);
    uint8_t special_color = ((giciType >> 10) & 12);
    uint8_t singleDoubleMirror = giciType & 1;
    uint8_t canDoCompression = (giciType & 0x4000) ? 0 : 1;

    bool extra_color = false;
    bool extra_tft_bitshifting = false;
    bool mirror_width = false;
    uint16_t width_display = 104;
    uint16_t height_display = 212;

    switch (screenResolution) {
        case 0:
            width_display = 216;
            height_display = 104;
            break;
        case 1:
            width_display = 296;
            height_display = 128;
            break;
        case 2:
            width_display = 300;
            height_display = 400;
            break;
        case 3:
            width_display = 640;
            height_display = 384;
            break;
        case 4:
            width_display = 960;
            height_display = 640;
            break;
        case 5:
            width_display = 250;
            height_display = 136;
            break;
        case 6:
            width_display = 196;
            height_display = 96;
            break;
        case 7:
            width_display = 640;
            height_display = 480;
            break;
        case 8:
            width_display = 250;
            height_display = 128;
            break;
        case 9:
            width_display = 800;
            height_display = 480;
            break;
        case 10:
            width_display = 280;
            height_display = 480;
            break;
    }

    switch (dispPtype) {
        case 0:  // TFT
            mirror_width = false;
            extra_tft_bitshifting = true;  // Special case for the TFT Type
            break;
        case 1:  // EPA
            mirror_width = false;
            break;
        case 2:  // EPA1
            mirror_width = false;
            break;
        case 3:  // EPA2
            mirror_width = false;
            break;
    }

    if (giciType & 0x100)  // Some special case, needs to be tested if always correct
        mirror_width = true;

    switch (availColors) {
        case 0:  // BW
            extra_color = false;
            break;
        case 1:  // BWR
            extra_color = true;
            break;
        case 2:  // BWY
            extra_color = true;
            break;
        case 3:  // BWRY
            extra_color = true;
            break;
        case 4:  // BWRGBYO
            extra_color = true;
            break;
    }
    switch (singleDoubleMirror) {
        case 0:  // Single image
            break;
        case 1:  // 2 Images
            break;
    }

    uint32_t len_compressed = 0;
    if (canDoCompression)
        len_compressed = 4;
    uint32_t curr_input_posi = 0;
    uint32_t byte_per_line = (height_display / 8);
    if (height_display % 8 != 0)
        byte_per_line++;
    Mirrorbuffer = (uint8_t*)malloc(byte_per_line + 1);
    if (Mirrorbuffer == nullptr) {
        Serial.println("BLE Could not create Mirrorbuffer!");
        return 0;
    }
    Serial.printf("BLE Filter options:\r\n");
    Serial.printf("screenResolution %d\r\n", screenResolution);
    Serial.printf("dispPtype %d\r\n", dispPtype);
    Serial.printf("availColors %d\r\n", availColors);
    Serial.printf("special_color %d\r\n", special_color);
    Serial.printf("singleDoubleMirror %d\r\n", singleDoubleMirror);
    Serial.printf("canDoCompression %d\r\n", canDoCompression);
    Serial.printf("byte_per_line %d\r\n", byte_per_line);
    Serial.printf("width_display %d\r\n", width_display);
    Serial.printf("height_display %d\r\n", height_display);
    Serial.printf("mirror_width %d\r\n", mirror_width);
    for (int i = 0; i < width_display; i++) {
        if (canDoCompression) {
            buffer[len_compressed++] = 0x75;
            buffer[len_compressed++] = byte_per_line + 7;
            buffer[len_compressed++] = byte_per_line;
            buffer[len_compressed++] = 0x00;
            buffer[len_compressed++] = 0x00;
            buffer[len_compressed++] = 0x00;
            buffer[len_compressed++] = 0x00;
        }
        if (mirror_width) {
            for (int b = 0; b < byte_per_line; b++) {
                Mirrorbuffer[b] = ~queueItem->data[curr_input_posi++];
            }
            for (int b = byte_per_line - 1; b >= 0; b--) {
                buffer[len_compressed++] = swapBits(Mirrorbuffer[b]);
            }
        } else {
            for (int b = 0; b < byte_per_line; b++) {
                buffer[len_compressed++] = ~queueItem->data[curr_input_posi++];
            }
        }
    }
    if (extra_color) {
        for (int i = 0; i < width_display; i++) {
            if (canDoCompression) {
                buffer[len_compressed++] = 0x75;
                buffer[len_compressed++] = byte_per_line + 7;
                buffer[len_compressed++] = byte_per_line;
                buffer[len_compressed++] = 0x00;
                buffer[len_compressed++] = 0x00;
                buffer[len_compressed++] = 0x00;
                buffer[len_compressed++] = 0x00;
            }
            if (mirror_width) {
                for (int b = 0; b < byte_per_line; b++) {
                    if (queueItem->len <= curr_input_posi)
                        Mirrorbuffer[b] = 0x00;  // Do not anything outside of the buffer!
                    else
                        Mirrorbuffer[b] = queueItem->data[curr_input_posi++];
                }
                for (int b = byte_per_line - 1; b >= 0; b--) {
                    buffer[len_compressed++] = swapBits(Mirrorbuffer[b]);
                }
            } else {
                for (int b = 0; b < byte_per_line; b++) {
                    if (queueItem->len <= curr_input_posi) {
                        buffer[len_compressed++] = 0x00;  // Do not anything outside of the buffer!
                    } else {
                        buffer[len_compressed++] = queueItem->data[curr_input_posi++];
                    }
                }
            }
        }
    }
    if (canDoCompression) {
        buffer[0] = len_compressed & 0xff;
        buffer[1] = (len_compressed >> 8) & 0xff;
        buffer[2] = (len_compressed >> 16) & 0xff;
        buffer[3] = (len_compressed >> 24) & 0xff;
    }
    free(Mirrorbuffer);
    return len_compressed;
}

uint32_t get_ATC_BLE_OEPL_image(uint8_t address[8], uint8_t* buffer, uint32_t max_len, uint8_t* dataType, uint8_t* dataTypeArgument, uint16_t* nextCheckIn) {
    uint32_t t = millis();
    PendingItem* queueItem = getQueueItem(address, 0);
    if (queueItem == nullptr) {
        prepareCancelPending(address);
        Serial.printf("blockrequest: couldn't find taginfo %02X%02X%02X%02X%02X%02X%02X%02X\r\n", address[7], address[6], address[5], address[4], address[3], address[2], address[1], address[0]);
        return 0;
    }
    if (queueItem->data == nullptr) {
        fs::File file = contentFS->open(queueItem->filename);
        if (!file) {
            Serial.print("No current file. " + String(queueItem->filename) + " Canceling request\r\n");
            prepareCancelPending(address);
            return 0;
        }
        queueItem->data = getDataForFile(file);
        Serial.println("Reading file " + String(queueItem->filename) + " in  " + String(millis() - t) + "ms");
        file.close();
    }
    if (queueItem->len > max_len) {
        Serial.print("The upload is too big better cencel it\r\n");
        prepareCancelPending(address);
        return 0;
    }
    *dataType = queueItem->pendingdata.availdatainfo.dataType;
    *dataTypeArgument = queueItem->pendingdata.availdatainfo.dataTypeArgument;
    *nextCheckIn = queueItem->pendingdata.availdatainfo.nextCheckIn;
    uint32_t len_compressed = queueItem->len;
    memcpy(buffer, queueItem->data, queueItem->len);
    Serial.print("Data is prepared Len: " + String(queueItem->len) + "\r\n");
    return queueItem->len;
}

#endif
