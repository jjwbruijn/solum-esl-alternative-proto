#include "serialap.h"

#include <Arduino.h>
#include <HardwareSerial.h>

#include "commstructs.h"
#include "contentmanager.h"
#include "flasher.h"
#include "leds.h"
#include "newproto.h"
#include "powermgt.h"
#include "settings.h"
#include "storage.h"
#include "web.h"
#include "zbs_interface.h"

#define LOG(format, ... ) printf(format,## __VA_ARGS__)

QueueHandle_t rxCmdQueue;
SemaphoreHandle_t txActive;

// If a command is sent, it will wait for a reply here
#define CMD_REPLY_WAIT 0x00
#define CMD_REPLY_ACK 0x01
#define CMD_REPLY_NOK 0x02
#define CMD_REPLY_NOQ 0x03
volatile uint8_t cmdReplyValue = CMD_REPLY_WAIT;

#define AP_SERIAL_PORT Serial1
#ifndef FLASHER_DEBUG_SHARED
volatile bool rxSerialStopTask2 = false;
#endif

uint8_t channelList[6];
struct espSetChannelPower curChannel = {0, 11, 10};

#define RX_CMD_RQB 0x01
#define RX_CMD_ADR 0x02
#define RX_CMD_XFC 0x03
#define RX_CMD_XTO 0x04
#define RX_CMD_RDY 0x05
#define RX_CMD_RSET 0x06
#define RX_CMD_TRD 0x07

#define AP_ACTIVITY_MAX_INTERVAL 30 * 1000
volatile uint32_t lastAPActivity = 0;
struct APInfoS apInfo;

enum ApSerialState {
   SERIAL_STATE_NONE,
   SERIAL_STATE_INITIALIZED,
   SERIAL_STATE_STARTING,
   SERIAL_STATE_RUNNING,
   SERIAL_STATE_STOP,
   SERIAL_STATE_STOPPED
};

volatile ApSerialState gSerialTaskState;

struct rxCmd {
    uint8_t* data;
    uint8_t len;
    uint8_t type;
};

#define ZBS_RX_WAIT_HEADER 0
#define ZBS_RX_WAIT_PKT_LEN 1
#define ZBS_RX_WAIT_PKT_RX 2
#define ZBS_RX_WAIT_SEP1 3
#define ZBS_RX_WAIT_SEP2 4
#define ZBS_RX_WAIT_VER 6
#define ZBS_RX_BLOCK_REQUEST 7
#define ZBS_RX_WAIT_XFERCOMPLETE 8
#define ZBS_RX_WAIT_DATA_REQ 9
#define ZBS_RX_WAIT_JOINNETWORK 10
#define ZBS_RX_WAIT_XFERTIMEOUT 11
#define ZBS_RX_WAIT_MAC 12
#define ZBS_RX_WAIT_CHANNEL 13
#define ZBS_RX_WAIT_POWER 14
#define ZBS_RX_WAIT_PENDING 15
#define ZBS_RX_WAIT_NOP 16
#define ZBS_RX_WAIT_TYPE 17
#define ZBS_RX_WAIT_TAG_RETURN_DATA 18
#define ZBS_RX_WAIT_SUBCHANNEL 19

bool txStart() {
    while (1) {
        if (xPortInIsrContext()) {
            if (xSemaphoreTakeFromISR(txActive, NULL) == pdTRUE) return true;
        } else {
            if (xSemaphoreTake(txActive, portTICK_PERIOD_MS)) return true;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
        Serial.println("wait... tx busy");
    }
    // this never happens. Should we make a timeout?
    return false;
}
void txEnd() {
    if (xPortInIsrContext()) {
        xSemaphoreGiveFromISR(txActive, NULL);
    } else {
        xSemaphoreGive(txActive);
    }
}
bool waitCmdReply() {
    uint32_t val = millis();
    while (millis() < val + 200) {
        switch (cmdReplyValue) {
            case CMD_REPLY_WAIT:
                break;
            case CMD_REPLY_ACK:
                lastAPActivity = millis();
                if (apInfo.isOnline == false)
                    setAPstate(true, AP_STATE_ONLINE);
                return true;
                break;
            case CMD_REPLY_NOK:
                lastAPActivity = millis();
                return false;
                break;
            case CMD_REPLY_NOQ:
                lastAPActivity = millis();
                return false;
                break;
        }
        vTaskDelay(1 / portTICK_RATE_MS);
    }
    return false;
}

#if (AP_PROCESS_PORT == FLASHER_AP_PORT)
int8_t APpowerPins[] = FLASHER_AP_POWER;
#define AP_RESET_PIN FLASHER_AP_RESET
#define AP_POWER_PIN FLASHER_AP_POWER
#endif
#ifdef HAS_EXT_FLASHER
#if (AP_PROCESS_PORT == FLASHER_EXT_PORT)
int8_t APpowerPins[] = FLASHER_EXT_POWER;
#define AP_RESET_PIN FLASHER_EXT_RESET
#define AP_POWER_PIN FLASHER_EXT_POWER
#endif
#if (AP_PROCESS_PORT == FLASHER_ALTRADIO_PORT)
int8_t APpowerPins[] = FLASHER_ALT_POWER;
#define AP_RESET_PIN FLASHER_ALT_RESET
#define AP_POWER_PIN FLASHER_ALT_POWER
#endif
#endif

void APEnterEarlyReset() {
    pinMode(AP_RESET_PIN, OUTPUT);
    digitalWrite(AP_RESET_PIN, LOW);
}

void setAPstate(bool isOnline, uint8_t state) {
    apInfo.isOnline = isOnline;
    apInfo.state = state;
#ifdef HAS_RGB_LED
    CRGB colorMap[7] = {
        CRGB::Orange,
        CRGB::Green,
        CRGB::Blue,
        CRGB::Yellow,
        CRGB::Aqua,
        CRGB::Red,
        CRGB::YellowGreen};
    rgbIdleColor = colorMap[state];
    #ifdef BLE_ONLY
        rgbIdleColor = CRGB::Green;
    #endif
    rgbIdlePeriod = (isOnline ? 767 : 255);
    if (isOnline) rgbIdle();
#endif
#ifdef FLASHER_DEBUG_SHARED
// Flasher shares port with AP comms
    if(state == AP_STATE_FLASHING) {
        LOG("Shared COM port, gSerialTaskState %d\n",gSerialTaskState);
        gSerialTaskState = SERIAL_STATE_STOP;
        for(int i = 0; i < 100; i++) {
            vTaskDelay(1 / portTICK_RATE_MS);
            if(gSerialTaskState == SERIAL_STATE_STOPPED) {
                gSerialTaskState = SERIAL_STATE_NONE;
                break;
            }
        }
        LOG("gSerialTaskState %d\n",gSerialTaskState);
    }
#endif
}

// Reset the tag
void APTagReset() {
    Serial.println("Resetting tag");
    uint8_t powerPins = sizeof(APpowerPins);
    if (powerPins > 0 && APpowerPins[0] == -1)
        powerPins = 0;

#ifdef FLASHER_DEBUG_PROG
    pinMode(FLASHER_DEBUG_PROG, OUTPUT);
    digitalWrite(FLASHER_DEBUG_PROG, HIGH);
#endif
    pinMode(AP_RESET_PIN, OUTPUT);
    digitalWrite(AP_RESET_PIN, LOW);
    vTaskDelay(50 / portTICK_PERIOD_MS);
    powerControl(false, (uint8_t*)APpowerPins, powerPins);
    vTaskDelay(300 / portTICK_PERIOD_MS);
    powerControl(true, (uint8_t*)APpowerPins, powerPins);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    digitalWrite(AP_RESET_PIN, HIGH);
    vTaskDelay(100 / portTICK_PERIOD_MS);
}

// Send data to the AP
uint16_t sendBlock(const void* data, const uint16_t len) {
    time_t timeCanary = millis();
    if (apInfo.state == AP_STATE_NORADIO) return true;
    if (!apInfo.isOnline) return false;
    if (!txStart()) return 0;
    // don't retry now, as it collides with communication from the tag
    for (uint8_t attempt = 0; attempt < 1; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print(">D>");
        if (waitCmdReply()) goto blksend;
        Serial.printf("block send failed in try %d\r\n", attempt);
    }
    Serial.print("Failed sending block...\r\n");
    txEnd();
    return 0;
blksend:
    uint8_t blockbuffer[sizeof(struct blockData)];
    struct blockData* bd = (struct blockData*)blockbuffer;
    bd->size = len;
    bd->checksum = 0;

    // calculate checksum
    const uint8_t* dataBytes = reinterpret_cast<const uint8_t*>(data);
    for (uint16_t c = 0; c < len; c++) {
        bd->checksum += dataBytes[c];
    }

    // send blockData header
    dataBytes = reinterpret_cast<const uint8_t*>(&blockbuffer);
    const size_t bufferSize = sizeof(struct blockData);
    uint8_t* modifiedHeader = static_cast<uint8_t*>(malloc(bufferSize));
    if (modifiedHeader != nullptr) {
        for (size_t i = 0; i < bufferSize; i++) {
            modifiedHeader[i] = 0xAA ^ dataBytes[i];
        }
        AP_SERIAL_PORT.write(modifiedHeader, bufferSize);
        free(modifiedHeader);
    }

    // send an entire block of data
    uint16_t c;
    dataBytes = reinterpret_cast<const uint8_t*>(data);
    uint8_t* modifiedBuffer = static_cast<uint8_t*>(malloc(len));
    if (modifiedBuffer != nullptr) {
        for (c = 0; c < len; c++) {
            modifiedBuffer[c] = 0xAA ^ dataBytes[c];
        }
        AP_SERIAL_PORT.write(modifiedBuffer, len);
        free(modifiedBuffer);
    }

    // fill the rest of the block-length filled with something else (will end up as 0xFF in the buffer)
    const size_t remainingBytes = BLOCK_DATA_SIZE - c;
    if (remainingBytes > 0) {
        uint8_t fillBuffer[remainingBytes];
        memset(fillBuffer, 0x55, remainingBytes);
        AP_SERIAL_PORT.write(fillBuffer, remainingBytes);
    }

    // dummy bytes in case some bytes were missed, makes sure the AP gets kicked out of data-loading mode
    uint8_t dummyBuffer[32];
    memset(dummyBuffer, 0xF5, 32);
    AP_SERIAL_PORT.write(dummyBuffer, 32);

    if (apInfo.type != ESP32_C6) delay(10);
    txEnd();
    Serial.println("Sendblock complete, " + String(millis() - timeCanary) + "ms");
    return bd->checksum;
}

bool sendDataAvail(struct pendingData* pending) {
    if (apInfo.state == AP_STATE_NORADIO) return true;
    if (!apInfo.isOnline) return false;
    if (!txStart()) return false;
    addCRC(pending, sizeof(struct pendingData));
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print("SDA>");
        for (uint8_t c = 0; c < sizeof(struct pendingData); c++) {
            AP_SERIAL_PORT.write(((uint8_t*)pending)[c]);
        }
        if (waitCmdReply()) {
            txEnd();
            return true;
        }
        Serial.printf("SDA send failed in try %d\r\n", attempt);
        delay(200);
    }
    Serial.print("SDA failed to send...\r\n");
    txEnd();
    return false;
}
bool sendCancelPending(struct pendingData* pending) {
    if (apInfo.state == AP_STATE_NORADIO) return true;
    if (!apInfo.isOnline) return false;
    if (!txStart()) return false;
    addCRC(pending, sizeof(struct pendingData));
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print("CXD>");
        for (uint8_t c = 0; c < sizeof(struct pendingData); c++) {
            AP_SERIAL_PORT.write(((uint8_t*)pending)[c]);
        }
        if (waitCmdReply()) {
            txEnd();
            return true;
        }
        Serial.printf("CXD send failed in try %d\r\n", attempt);
    }
    Serial.print("CXD failed to send...\r\n");
    txEnd();
    return false;
}
bool sendChannelPower(struct espSetChannelPower* scp) {
    if (apInfo.state == AP_STATE_NORADIO) return true;
    if ((apInfo.state != AP_STATE_ONLINE) && (apInfo.state != AP_STATE_COMING_ONLINE)) return false;
    if (!txStart()) return false;
    addCRC(scp, sizeof(struct espSetChannelPower));
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print("SCP>");
        for (uint8_t c = 0; c < sizeof(struct espSetChannelPower); c++) {
            AP_SERIAL_PORT.write(((uint8_t*)scp)[c]);
        }
        if (waitCmdReply()) {
            txEnd();
            apInfo.channel = scp->channel;
            apInfo.power = scp->power;
            return true;
        }
        Serial.printf("SCP send failed in try %d\r\n", attempt);
    }
    Serial.print("SCP failed to send...\r\n");
    txEnd();
    return false;
}
bool sendPing() {
    if (apInfo.state == AP_STATE_NORADIO) return true;
    if (apInfo.state == AP_STATE_FLASHING) return false;
    Serial.print("ping");
    int t = millis();
    if (!txStart()) return false;
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print("RDY?");
        if (waitCmdReply()) {
            txEnd();
            Serial.printf(" ok, %dms\r\n", millis() - t);
            return true;
        }
    }
    txEnd();
    Serial.println(" failed");
    return false;
}
bool sendGetInfo() {
    if (apInfo.state == AP_STATE_NORADIO) return true;
    if (!txStart()) return false;
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print("NFO?");
        if (waitCmdReply()) {
            txEnd();
            return true;
        }
    }
    txEnd();
    return false;
}
bool sendHighspeed() {
    if (apInfo.state == AP_STATE_NORADIO) return true;
    if (!txStart()) return false;
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        cmdReplyValue = CMD_REPLY_WAIT;
        AP_SERIAL_PORT.print("HSPD");
        if (waitCmdReply()) {
            txEnd();
            return true;
        }
    }
    txEnd();
    return false;
}

// add RX'd request from the AP to the processor queue
void addRXQueue(uint8_t* data, uint8_t len, uint8_t type) {
    struct rxCmd* rxcmd = new struct rxCmd;
    rxcmd->data = data;
    rxcmd->len = len;
    rxcmd->type = type;
    BaseType_t queuestatus = xQueueSend(rxCmdQueue, &rxcmd, 0);
    if (queuestatus == pdFALSE) {
        if (data) free(data);
        free(rxcmd);
    }
}

// Asynchronous command processor
void rxCmdProcessor(void* parameter) {
    rxCmdQueue = xQueueCreate(30, sizeof(struct rxCmd*));
    txActive = xSemaphoreCreateBinary();
    xSemaphoreGive(txActive);
    while (1) {
        struct rxCmd* rxcmd = nullptr;
        BaseType_t q = xQueueReceive(rxCmdQueue, &rxcmd, 10);
        if (q == pdTRUE) {
            switch (rxcmd->type) {
                case RX_CMD_RQB:
                    processBlockRequest((struct espBlockRequest*)rxcmd->data);
#ifdef HAS_RGB_LED
                    // shortBlink(CRGB::Blue);
#endif
                    quickBlink(3);
                    break;
                case RX_CMD_ADR:
                    processDataReq((struct espAvailDataReq*)rxcmd->data, true);
#ifdef HAS_RGB_LED
                    // shortBlink(CRGB::Aqua);
#endif
                    quickBlink(1);
                    break;
                case RX_CMD_XFC:
                    processXferComplete((struct espXferComplete*)rxcmd->data, true);
#ifdef HAS_RGB_LED
                    // shortBlink(CRGB::Purple);
#endif
                    break;
                case RX_CMD_XTO:
                    processXferTimeout((struct espXferComplete*)rxcmd->data, true);
                    break;
                case RX_CMD_RSET:
                    Serial.println("AP did reset, resending pending\r\n");
                    refreshAllPending();
                    sendChannelPower(&curChannel);
                    break;
                case RX_CMD_TRD:
                    // received tag return data
                    processTagReturnData((struct espTagReturnData*)rxcmd->data, rxcmd->len, true);
                    break;
            }
            if (rxcmd->data) free(rxcmd->data);
            if (rxcmd) free(rxcmd);
        }
    }
}
void rxSerialTask(void* parameter) {
    static char cmdbuffer[4] = {0};
    static uint8_t* packetp = nullptr;
    //    static uint8_t pktlen = 0;
    static uint8_t pktindex = 0;  // length of the command
    static uint8_t RXState = ZBS_RX_WAIT_HEADER;
    static char lastchar = 0;
    static uint8_t charindex = 0;

    gSerialTaskState = SERIAL_STATE_RUNNING;
    LOG("rxSerialTask starting\n");
    while (gSerialTaskState == SERIAL_STATE_RUNNING) {
        while (AP_SERIAL_PORT.available()) {
            lastchar = AP_SERIAL_PORT.read();
            switch (RXState) {
                case ZBS_RX_WAIT_HEADER:

                    Serial.write(lastchar);

                    //  shift characters in
                    for (uint8_t c = 0; c < 3; c++) {
                        cmdbuffer[c] = cmdbuffer[c + 1];
                    }
                    cmdbuffer[3] = lastchar;

                    if ((strncmp(cmdbuffer, "ACK>", 4) == 0)) cmdReplyValue = CMD_REPLY_ACK;
                    if ((strncmp(cmdbuffer, "NOK>", 4) == 0)) cmdReplyValue = CMD_REPLY_NOK;
                    if ((strncmp(cmdbuffer, "NOQ>", 4) == 0)) cmdReplyValue = CMD_REPLY_NOQ;

                    if ((strncmp(cmdbuffer, "VER>", 4) == 0)) {
                        pktindex = 0;
                        RXState = ZBS_RX_WAIT_VER;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if ((strncmp(cmdbuffer, "MAC>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_MAC;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if ((strncmp(cmdbuffer, "ZCH>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_CHANNEL;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
#ifdef HAS_SUBGHZ
                    if ((strncmp(cmdbuffer, "SCH>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_SUBCHANNEL;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
#endif
                    if ((strncmp(cmdbuffer, "ZPW>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_POWER;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if ((strncmp(cmdbuffer, "PEN>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_PENDING;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if ((strncmp(cmdbuffer, "NOP>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_NOP;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if ((strncmp(cmdbuffer, "TYP>", 4) == 0)) {
                        RXState = ZBS_RX_WAIT_TYPE;
                        charindex = 0;
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if (strncmp(cmdbuffer, "RES>", 4) == 0) {
                        addRXQueue(NULL, 0, RX_CMD_RSET);
                    }
                    if (strncmp(cmdbuffer, "RQB>", 4) == 0) {
                        RXState = ZBS_RX_BLOCK_REQUEST;
                        charindex = 0;
                        pktindex = 0;
                        packetp = (uint8_t*)calloc(sizeof(struct espBlockRequest) + 8, 1);
                        memset(cmdbuffer, 0x00, 4);
                        lastAPActivity = millis();
                        if (apInfo.isOnline == false)
                            setAPstate(true, AP_STATE_ONLINE);
                    }
                    if (strncmp(cmdbuffer, "ADR>", 4) == 0) {
                        RXState = ZBS_RX_WAIT_DATA_REQ;
                        charindex = 0;
                        pktindex = 0;
                        packetp = (uint8_t*)calloc(sizeof(struct espAvailDataReq) + 8, 1);
                        memset(cmdbuffer, 0x00, 4);
                        lastAPActivity = millis();
                        if (apInfo.isOnline == false)
                            setAPstate(true, AP_STATE_ONLINE);
                    }
                    if (strncmp(cmdbuffer, "XFC>", 4) == 0) {
                        RXState = ZBS_RX_WAIT_XFERCOMPLETE;
                        pktindex = 0;
                        packetp = (uint8_t*)calloc(sizeof(struct espXferComplete) + 8, 1);
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if (strncmp(cmdbuffer, "XTO>", 4) == 0) {
                        RXState = ZBS_RX_WAIT_XFERTIMEOUT;
                        pktindex = 0;
                        packetp = (uint8_t*)calloc(sizeof(struct espXferComplete) + 8, 1);
                        memset(cmdbuffer, 0x00, 4);
                    }
                    if (strncmp(cmdbuffer, "RDY>", 4) == 0) {
                        addRXQueue(NULL, 0, RX_CMD_RDY);
                    }
                    if (strncmp(cmdbuffer, "TRD>", 4) == 0) {
                        RXState = ZBS_RX_WAIT_TAG_RETURN_DATA;
                        pktindex = 0;
                        packetp = (uint8_t*)calloc(sizeof(struct espTagReturnData) + 8, 1);
                        memset(cmdbuffer, 0x00, 4);
                        lastAPActivity = millis();
                        if (apInfo.isOnline == false)
                            setAPstate(true, AP_STATE_ONLINE);
                    }
                    break;
                case ZBS_RX_BLOCK_REQUEST:
                    packetp[pktindex] = lastchar;
                    pktindex++;
                    if (pktindex == sizeof(struct espBlockRequest)) {
                        addRXQueue(packetp, pktindex, RX_CMD_RQB);
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_XFERCOMPLETE:
                    packetp[pktindex] = lastchar;
                    pktindex++;
                    if (pktindex == sizeof(struct espXferComplete)) {
                        addRXQueue(packetp, pktindex, RX_CMD_XFC);
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_XFERTIMEOUT:
                    packetp[pktindex] = lastchar;
                    pktindex++;
                    if (pktindex == sizeof(struct espXferComplete)) {
                        addRXQueue(packetp, pktindex, RX_CMD_XTO);
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_DATA_REQ:
                    packetp[pktindex] = lastchar;
                    pktindex++;
                    if (pktindex == sizeof(struct espAvailDataReq)) {
                        addRXQueue(packetp, pktindex, RX_CMD_ADR);
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_TAG_RETURN_DATA: {
                    packetp[pktindex] = lastchar;
                    pktindex++;
                    if ((pktindex > 10) && (pktindex >= (packetp[9] + 10))) {
                        addRXQueue(packetp, pktindex, RX_CMD_TRD);
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                } break;
                case ZBS_RX_WAIT_VER:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 4) {
                        charindex = 0;
                        apInfo.version = (uint16_t)strtoul(cmdbuffer, NULL, 16);
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_MAC:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        charindex = 0;
                        apInfo.mac[pktindex] = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                        pktindex++;
                    }
                    if (pktindex == 8) {
                        RXState = ZBS_RX_WAIT_HEADER;
                    }
                    break;
                case ZBS_RX_WAIT_CHANNEL:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        RXState = ZBS_RX_WAIT_HEADER;
                        apInfo.channel = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                    }
                    break;
#ifdef HAS_SUBGHZ
                case ZBS_RX_WAIT_SUBCHANNEL:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 3) {
                        RXState = ZBS_RX_WAIT_HEADER;
                        int Channel = atoi(cmdbuffer);
                        if (Channel != NO_SUBGHZ_CHANNEL) {
                            apInfo.hasSubGhz = true;
                            apInfo.SubGhzChannel = Channel;
                        } else {
                            apInfo.hasSubGhz = false;
                            apInfo.SubGhzChannel = 0;
                        }
                    }
                    break;
#endif
                case ZBS_RX_WAIT_POWER:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        RXState = ZBS_RX_WAIT_HEADER;
                        apInfo.power = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                    }
                    break;
                case ZBS_RX_WAIT_PENDING:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        RXState = ZBS_RX_WAIT_HEADER;
                        apInfo.pendingBuffer = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                    }
                    break;
                case ZBS_RX_WAIT_NOP:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        RXState = ZBS_RX_WAIT_HEADER;
                        apInfo.nop = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                    }
                    break;
                case ZBS_RX_WAIT_TYPE:
                    cmdbuffer[charindex] = lastchar;
                    charindex++;
                    if (charindex == 2) {
                        RXState = ZBS_RX_WAIT_HEADER;
                        apInfo.type = (uint8_t)strtoul(cmdbuffer, NULL, 16);
                    }
                    break;
            }
        }
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }  // end of while(1)

    AP_SERIAL_PORT.end();
    gSerialTaskState = SERIAL_STATE_STOPPED;
    LOG("rxSerialTask stopped\n");
    vTaskDelete(NULL);
}

#if defined(FLASHER_DEBUG_RXD) && !defined(FLASHER_DEBUG_SHARED)
void rxSerialTask2(void* parameter) {
    char lastchar = 0;
    time_t startTime = millis();
    int charCount = 0;
    Serial2.begin(115200, SERIAL_8N1, FLASHER_DEBUG_TXD, FLASHER_DEBUG_RXD);
    while (rxSerialStopTask2 == false) {
        while (Serial2.available()) {
            lastchar = Serial2.read();
            charCount++;

            // debug info
            Serial.write(lastchar);
        }
        vTaskDelay(1 / portTICK_PERIOD_MS);

        time_t currentTime = millis();
        if (currentTime - startTime >= 1000) {
            if (charCount > 6000) {
                rxSerialStopTask2 = true;
                Serial.println("Serial monitor stopped because of flooding (" + String(charCount) + " characters per second");
            }
            startTime = currentTime;
            charCount = 0;
        }
    }
    Serial2.end();
    Serial.println("Exiting AP serial monitor");
    vTaskDelete(NULL);
}
#endif

void ShowAPInfo() {
    Serial.printf("\r\n| AP Info - type %02X       |\r\n", apInfo.type);
    Serial.printf("| Ch   |             0x%02X |\r\n", apInfo.channel);
    Serial.printf("| Power|               %02X |\r\n", apInfo.power);
    Serial.printf("| MAC  | %02X%02X%02X%02X%02X%02X%02X%02X |\r\n", apInfo.mac[7], apInfo.mac[6], apInfo.mac[5], apInfo.mac[4], apInfo.mac[3], apInfo.mac[2], apInfo.mac[1], apInfo.mac[0]);
    Serial.printf("| Ver  |           0x%04X |\r\n", apInfo.version);
}

void notifySegmentedFlash() {
    sendAPSegmentedData(apInfo.mac, (String) "Fl     ash", 0x0800, false, true);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
#ifdef POWER_NO_SOFT_POWER
    sendAPSegmentedData(apInfo.mac, (String) "If    done", 0x0800, false, true);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    sendAPSegmentedData(apInfo.mac, (String) "RE    boot", 0x0800, false, true);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
#endif
}
void checkWaitPowerCycle() {
    // check if we should wait for a power cycle. If we do, try to inform the user the best we can, and hang.
#ifdef POWER_NO_SOFT_POWER
    setAPstate(false, AP_STATE_REQUIRED_POWER_CYCLE);
    // If we have no soft power control, we'll now wait until the device is power-cycled
    Serial.printf("Please power-cycle your AP/device\r\n");
#ifdef HAS_RGB_LED
    showColorPattern(CRGB::Aqua, CRGB::Aqua, CRGB::Red);
#endif
    while (1) {
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
#endif
}
void segmentedShowIp() {
    IPAddress IP = WiFi.localIP();
    char temp[12];
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    sendAPSegmentedData(apInfo.mac, (String) "IP    Addr", 0x0200, true, true);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    sprintf(temp, "%03d IP %03d", IP[0], IP[1]);
    sendAPSegmentedData(apInfo.mac, (String)temp, 0x0200, true, true);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    sprintf(temp, "%03d IP %03d", IP[2], IP[3]);
    sendAPSegmentedData(apInfo.mac, (String)temp, 0x0200, true, true);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
}

bool bringAPOnline() {
#ifdef BLE_ONLY
    apInfo.state = AP_STATE_NORADIO;
#endif
    if (apInfo.state == AP_STATE_NORADIO) return true;
    if (apInfo.state == AP_STATE_FLASHING) return false;

    if(gSerialTaskState != SERIAL_STATE_INITIALIZED) {
#if (AP_PROCESS_PORT == FLASHER_AP_PORT)
       AP_SERIAL_PORT.begin(115200, SERIAL_8N1, FLASHER_AP_RXD, FLASHER_AP_TXD);
#elif defined(HAS_EXT_FLASHER)
   #if (AP_PROCESS_PORT == FLASHER_EXT_PORT)
       AP_SERIAL_PORT.begin(115200, SERIAL_8N1, FLASHER_EXT_RXD, FLASHER_EXT_TXD);
   #elif (AP_PROCESS_PORT == FLASHER_ALTRADIO_PORT)
       AP_SERIAL_PORT.begin(115200, SERIAL_8N1, FLASHER_AP_RXD, FLASHER_AP_TXD);
   #endif
#endif
       gSerialTaskState = SERIAL_STATE_INITIALIZED;
    }
    if(gSerialTaskState != SERIAL_STATE_RUNNING) {
       gSerialTaskState = SERIAL_STATE_STARTING;
       xTaskCreate(rxSerialTask, "rxSerialTask", 1750, NULL, 11, NULL);
       vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    setAPstate(false, AP_STATE_OFFLINE);
    // try without rebooting
    AP_SERIAL_PORT.updateBaudRate(115200);
    uint32_t bootTimeout = millis();
    bool APrdy = sendPing();
    if (!APrdy) {
        if (apInfo.state == AP_STATE_FLASHING) return false;
        APTagReset();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        bootTimeout = millis();
        APrdy = false;
        while ((!APrdy) && (millis() - bootTimeout < 10 * 1000) && (apInfo.state != AP_STATE_FLASHING)) {
            APrdy = sendPing();
            vTaskDelay(300 / portTICK_PERIOD_MS);
        }
    }
    if (!APrdy) {
        return false;
    } else {
        setAPstate(false, AP_STATE_COMING_ONLINE);
        sendChannelPower(&curChannel);
        vTaskDelay(200 / portTICK_PERIOD_MS);
        if (!sendGetInfo()) {
            setAPstate(false, AP_STATE_OFFLINE);
            return false;
        }
        if (apInfo.type == ESP32_C6) {
            if (sendHighspeed()) {
                AP_SERIAL_PORT.flush();
                vTaskDelay(10 / portTICK_PERIOD_MS);
                AP_SERIAL_PORT.updateBaudRate(2000000);
                Serial.println("switched to 2000000 baud");
            }
        }

        vTaskDelay(200 / portTICK_PERIOD_MS);
        setAPstate(true, AP_STATE_ONLINE);
        return true;
    }
}

bool checkRadio() {
    #ifdef BLE_ONLY
    return false;
    #endif
    #ifndef C6_OTA_FLASHING
    return true;
    #endif
    // make a short between FLASHER_AP_TXD and FLASHER_AP_RXD to indicate that no radio is present
    // e.g. for flasher only, or just to use the S3 to generate images for smaller AP's
    pinMode(FLASHER_AP_TXD, OUTPUT);
    pinMode(FLASHER_AP_RXD, INPUT_PULLDOWN);
    digitalWrite(FLASHER_AP_TXD, LOW);
    if (digitalRead(FLASHER_AP_RXD) != LOW) return true;
    digitalWrite(FLASHER_AP_TXD, HIGH);
    if (digitalRead(FLASHER_AP_RXD) != HIGH) return true;
    pinMode(FLASHER_AP_TXD, INPUT_PULLDOWN);
    return false;
}

void APTask(void* parameter) {
    if (!checkRadio()) {
        // no radio
        Serial.println("Working without radio.");
        addFadeMono(config.led);
        setAPstate(true, AP_STATE_NORADIO);
        refreshAllPending();
        vTaskDelete(NULL);
        return;
    }


    xTaskCreate(rxCmdProcessor, "rxCmdProcessor", 6000, NULL, 15, NULL);
#if defined(FLASHER_DEBUG_RXD) && !defined(FLASHER_DEBUG_SHARED)
    xTaskCreate(rxSerialTask2, "rxSerialTask2", 1750, NULL, 2, NULL);
    vTaskDelay(500 / portTICK_PERIOD_MS);
#endif
    bringAPOnline();

#ifndef C6_OTA_FLASHING
    if (checkForcedAPFlash() && FLASHER_AP_MOSI != -1) {
        if (apInfo.type == SOLUM_SEG_UK && apInfo.isOnline) {
            notifySegmentedFlash();
        }
        Serial.printf("We're going to try to perform an 'AP forced flash' in\r\n");
        flashCountDown(10);
        Serial.printf("\r\nPerforming force flash of the AP\r\n");
        setAPstate(false, AP_STATE_FLASHING);
        doForcedAPFlash();
        checkWaitPowerCycle();
        bringAPOnline();
    }
#endif

    if (apInfo.isOnline) {
        // AP works!
        ShowAPInfo();

        if (apInfo.type == SOLUM_SEG_UK) {
            setAPstate(true, AP_STATE_COMING_ONLINE);
            segmentedShowIp();
            showAPSegmentedInfo(apInfo.mac, true);
            setAPstate(true, AP_STATE_ONLINE);
            updateContent(apInfo.mac);
        }

        uint16_t fsversion;
#ifndef C6_OTA_FLASHING
        if (FLASHER_AP_MOSI != -1) {
            fsversion = getAPUpdateVersion(apInfo.type);
            if ((fsversion) && (apInfo.version != fsversion)) {
                Serial.printf("Firmware version on LittleFS: %04X\r\n", fsversion);

                Serial.printf("We're going to try to update the AP's FW in\r\n");
                flashCountDown(30);
                Serial.printf("\r\n");
                notifySegmentedFlash();
                setAPstate(false, AP_STATE_FLASHING);
                if (doAPUpdate(apInfo.type)) {
                    checkWaitPowerCycle();
                    Serial.printf("Flash completed, let's try to boot the AP!\r\n");
                    if (bringAPOnline()) {
                        // AP works
                        ShowAPInfo();
                        setAPchannel();
                    } else {
                        Serial.printf("Failed to bring up the AP after flashing seemed successful... That's not supposed to happen!\r\n");
                        Serial.printf("This can be caused by a bad AP firmware, failed or failing hardware, or the inability to fully power-cycle the AP\r\n");
                        setAPstate(false, AP_STATE_FAILED);
#ifdef HAS_RGB_LED
                        showColorPattern(CRGB::Red, CRGB::Yellow, CRGB::Red);
#endif
                    }
                } else {
                    setAPstate(false, AP_STATE_FAILED);
                    checkWaitPowerCycle();
                    Serial.println("Failed to update version on the AP :(\r\n");
#ifdef HAS_RGB_LED
                    showColorPattern(CRGB::Red, CRGB::Red, CRGB::Red);
#endif
                }
            }
        }
#endif

        refreshAllPending();
    } else {
#ifndef FLASH_TIMEOUT
#define FLASH_TIMEOUT 30
#endif

        if (FLASHER_AP_MOSI == -1) {
            Serial.printf("I wasn't able to connect to the AP radio. Did you flash it?\r\n");
            Serial.printf("The build of this firmware expects an AP tag with TXD/RXD on ESP32 pins %d and %d, does this match with your wiring?\r\n", FLASHER_AP_RXD, FLASHER_AP_TXD);
#ifdef HAS_RGB_LED
            showColorPattern(CRGB::Red, CRGB::Yellow, CRGB::Red);
#endif
            if (apInfo.state != AP_STATE_FLASHING)  // In case we are flashing already we do not want to end in a failed AP
                setAPstate(false, AP_STATE_FAILED);
        } else {
#ifndef C6_OTA_FLASHING
            // AP unavailable, maybe time to flash?
            setAPstate(false, AP_STATE_OFFLINE);

            Serial.printf("I wasn't able to connect to a ZBS (AP) tag.\r\n");
            Serial.printf("This could be the first time this AP is booted and the AP-tag may be unflashed.\r\n");
            Serial.printf("If this tag was previously flashed succesfully but this message still shows up, there's probably something wrong with the serial connections.\r\n");
            Serial.printf("The build of this firmware expects an AP tag with TXD/RXD on ESP32 pins %d and %d, does this match with your wiring?\r\n", FLASHER_AP_RXD, FLASHER_AP_TXD);
            Serial.printf("Performing firmware flash in about %d seconds!\r\n", FLASH_TIMEOUT);
            flashCountDown(FLASH_TIMEOUT);
            if (doAPFlash()) {
                checkWaitPowerCycle();
                if (bringAPOnline()) {
                    // AP works
                    ShowAPInfo();
                    if (apInfo.type == SOLUM_SEG_UK) {
                        segmentedShowIp();
                        showAPSegmentedInfo(apInfo.mac, true);
                    }
                    refreshAllPending();
                } else {
                    Serial.printf("Failed to bring up the AP after successful flashing... That's not supposed to happen!\r\n");
                    Serial.printf("This generally means that the flasher connections (MISO/MOSI/CLK/RESET/CS) are okay,\r\n");
                    Serial.printf("but we can't (yet) talk to the AP over serial lines. Verify the pins mentioned above.\r\n\r\n");

#ifndef POWER_NO_SOFT_POWER
                    Serial.printf("The firmware you're using expects soft power control over the AP tag; if it can't\r\n");
                    Serial.printf("power-cycle the AP-tag using GPIO pin %d, this can cause this very same issue.\r\n", APpowerPins[0]);
#endif

#ifdef HAS_RGB_LED
                    showColorPattern(CRGB::Red, CRGB::Yellow, CRGB::Red);
#endif
                    setAPstate(false, AP_STATE_FAILED);
                }
            } else {
                // failed to flash
#ifdef HAS_RGB_LED
                showColorPattern(CRGB::Red, CRGB::Red, CRGB::Red);
#endif
                setAPstate(false, AP_STATE_FAILED);
                Serial.println("Failed to flash the AP :(");
                Serial.println("Seems like you're running into some issues with the wiring, or (very small chance) the tag itself");
                Serial.println("This ESP32-build expects the following pins connected to the ZBS243:");
                Serial.println("---  ZBS243 based tag              ESP32  ---");
                Serial.printf("       TXD     ----------------     %02d\r\n", FLASHER_AP_RXD);
                Serial.printf("       RXD     ----------------     %02d\r\n", FLASHER_AP_TXD);
                Serial.printf("       CS/SS   ----------------     %02d\r\n", FLASHER_AP_SS);
                Serial.printf("       MOSI    ----------------     %02d\r\n", FLASHER_AP_MOSI);
                Serial.printf("       MISO    ----------------     %02d\r\n", FLASHER_AP_MISO);
                Serial.printf("       CLK     ----------------     %02d\r\n", FLASHER_AP_CLK);
                Serial.printf("       RSET    ----------------     %02d\r\n", FLASHER_AP_RESET);
#ifdef POWER_NO_SOFT_POWER
                Serial.printf("Your firmware is configured without soft power control. This means you'll have to manually power-cycle the tag after flashing.\r\n");
#else
                Serial.printf("       POWER   ----------------     %02d\r\n", APpowerPins[0]);
#endif
                Serial.println("Please verify your wiring and try again!");
            }
#ifdef HAS_SDCARD
            if (SD_CARD_CLK == FLASHER_AP_CLK ||
                SD_CARD_MISO == FLASHER_AP_MISO ||
                SD_CARD_MOSI == FLASHER_AP_MOSI) {
                Serial.println("Reseting in 30 seconds to restore SPI state!\r\n");
                flashCountDown(30);
                ESP.restart();
            }
#endif
#endif
        }
    }

    uint8_t attempts = 0;
    while (1) {
        if (((apInfo.state == AP_STATE_ONLINE) || (apInfo.state == AP_STATE_FAILED)) && (millis() - lastAPActivity > AP_ACTIVITY_MAX_INTERVAL)) {
            bool reply = sendPing();
            if (!reply) {
                attempts++;
            } else {
                if (apInfo.isOnline == false)
                    setAPstate(true, AP_STATE_ONLINE);
                attempts = 0;
            }
            if (attempts > 5 && apInfo.state != AP_STATE_FLASHING) {
                setAPstate(false, AP_STATE_WAIT_RESET);
                if (!bringAPOnline()) {
                    // tried to reset the AP, but we failed... Maybe the AP-Tag died?
                    setAPstate(false, AP_STATE_FAILED);
#ifdef HAS_RGB_LED
                    showColorPattern(CRGB::Yellow, CRGB::Yellow, CRGB::Red);
#endif
                    lastAPActivity = millis();  // we set this to retrigger a recovery in AP_ACTIVITY_MAX_INTERVAL seconds
                } else {
                    setAPstate(true, AP_STATE_ONLINE);
                    attempts = 0;
                    refreshAllPending();
                }
            }
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
