#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>

#include "commstructs.h"
#include "newproto.h"
#include "storage.h"
#include "tag_db.h"

#ifdef HAS_TFT

#include "ips_display.h"

#define YELLOW_SENSE 8  // sense AP hardware
#define TFT_BACKLIGHT 14

TFT_eSPI tft2 = TFT_eSPI();
uint8_t YellowSense = 0;
bool tftLogscreen = true;
bool tftOverride = false;

#if defined HAS_LILYGO_TPANEL || defined HAS_4inch_TPANEL

#if defined HAS_LILYGO_TPANEL
static const uint8_t st7701_type9_init_operations_lilygo[] = {

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x13,

    WRITE_C8_D8, 0xEF, 0x08,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x10,

    WRITE_C8_D16, 0xC0, 0x3B, 0x00,
    WRITE_C8_D16, 0xC1, 0x0B, 0x02,

    WRITE_COMMAND_8, 0xC2,
    WRITE_BYTES, 3, 0x30, 0x02, 0x37,

    WRITE_C8_D8, 0xCC, 0x10,

    WRITE_COMMAND_8, 0xB0,  // Positive Voltage Gamma Control
    WRITE_BYTES, 16,
    0x00, 0x0F, 0x16, 0x0E,
    0x11, 0x07, 0x09, 0x09,
    0x08, 0x23, 0x05, 0x11,
    0x0F, 0x28, 0x2D, 0x18,

    WRITE_COMMAND_8, 0xB1,  // Negative Voltage Gamma Control
    WRITE_BYTES, 16,
    0x00, 0x0F, 0x16, 0x0E,
    0x11, 0x07, 0x09, 0x08,
    0x09, 0x23, 0x05, 0x11,
    0x0F, 0x28, 0x2D, 0x18,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x11,

    WRITE_C8_D8, 0xB0, 0x4D,
    WRITE_C8_D8, 0xB1, 0x33,
    WRITE_C8_D8, 0xB2, 0x87,
    WRITE_C8_D8, 0xB5, 0x4B,
    WRITE_C8_D8, 0xB7, 0x8C,
    WRITE_C8_D8, 0xB8, 0x20,
    WRITE_C8_D8, 0xC1, 0x78,
    WRITE_C8_D8, 0xC2, 0x78,
    WRITE_C8_D8, 0xD0, 0x88,

    WRITE_COMMAND_8, 0xE0,
    WRITE_BYTES, 3, 0x00, 0x00, 0x02,

    WRITE_COMMAND_8, 0xE1,
    WRITE_BYTES, 11,
    0x02, 0xF0, 0x00, 0x00,
    0x03, 0xF0, 0x00, 0x00,
    0x00, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE2,
    WRITE_BYTES, 12,
    0x10, 0x10, 0x40, 0x40,
    0xF2, 0xF0, 0x00, 0x00,
    0xF2, 0xF0, 0x00, 0x00,

    WRITE_COMMAND_8, 0xE3,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE4, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 16,
    0x07, 0xEF, 0xF0, 0xF0,
    0x09, 0xF1, 0xF0, 0xF0,
    0x03, 0xF3, 0xF0, 0xF0,
    0x05, 0xED, 0xF0, 0xF0,

    WRITE_COMMAND_8, 0xE6,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE7, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE8,
    WRITE_BYTES, 16,
    0x08, 0xF0, 0xF0, 0xF0,
    0x0A, 0xF2, 0xF0, 0xF0,
    0x04, 0xF4, 0xF0, 0xF0,
    0x06, 0xEE, 0xF0, 0xF0,

    WRITE_COMMAND_8, 0xEB,
    WRITE_BYTES, 7,
    0x00, 0x00, 0xE4, 0xE4,
    0x44, 0x88, 0x40,

    WRITE_C8_D16, 0xEC, 0x78, 0x00,

    WRITE_COMMAND_8, 0xED,
    WRITE_BYTES, 16,
    0x20, 0xF9, 0x87, 0x76,
    0x65, 0x54, 0x4F, 0xFF,
    0xFF, 0xF4, 0x45, 0x56,
    0x67, 0x78, 0x9F, 0x02,

    WRITE_COMMAND_8, 0xEF,
    WRITE_BYTES, 6,
    0x10, 0x0D, 0x04, 0x08,
    0x3F, 0x1F,

    // WRITE_C8_D8, 0xCD, 0x05,//Test

    WRITE_C8_D8, 0x3A, 0x55,

    WRITE_C8_D8, 0x36, 0x08,

    WRITE_COMMAND_8, 0x11,

    // WRITE_COMMAND_8, 0xFF,//Test
    // WRITE_BYTES, 5,
    // 0x77, 0x01, 0x00, 0x00,
    // 0x12,

    // WRITE_C8_D8, 0xD1, 0x81,//Test
    // WRITE_C8_D8, 0xD2, 0x08,//Test

    WRITE_COMMAND_8, 0x29,  // Display On

    // WRITE_C8_D8, 0x35, 0x00,//Test
    // WRITE_C8_D8, 0xCE, 0x04,//Test

    // WRITE_COMMAND_8, 0xF2,//Test
    // WRITE_BYTES, 4,
    // 0xF0, 0xA3, 0xA3, 0x71,

    END_WRITE};
Arduino_DataBus* bus = new Arduino_XL9535SWSPI(IIC_SDA /* SDA */, IIC_SCL /* SCL */, -1 /* XL PWD */,
                                               XL95X5_CS /* XL CS */, XL95X5_SCLK /* XL SCK */, XL95X5_MOSI /* XL MOSI */);
Arduino_ESP32RGBPanel* rgbpanel = new Arduino_ESP32RGBPanel(
    -1 /* DE */, LCD_VSYNC /* VSYNC */, LCD_HSYNC /* HSYNC */, LCD_PCLK /* PCLK */,
    LCD_B0 /* B0 */, LCD_B1 /* B1 */, LCD_B2 /* B2 */, LCD_B3 /* B3 */, LCD_B4 /* B4 */,
    LCD_G0 /* G0 */, LCD_G1 /* G1 */, LCD_G2 /* G2 */, LCD_G3 /* G3 */, LCD_G4 /* G4 */, LCD_G5 /* G5 */,
    LCD_R0 /* R0 */, LCD_R1 /* R1 */, LCD_R2 /* R2 */, LCD_R3 /* R3 */, LCD_R4 /* R4 */,
    1 /* hsync_polarity */, 20 /* hsync_front_porch */, 2 /* hsync_pulse_width */, 0 /* hsync_back_porch */,
    1 /* vsync_polarity */, 30 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 1 /* vsync_back_porch */,
    10 /* pclk_active_neg */, 6000000L /* prefer_speed */, true /* useBigEndian */,
    0 /* de_idle_high*/, 0 /* pclk_idle_high */);
Arduino_RGB_Display* gfx = new Arduino_RGB_Display(
    LCD_WIDTH /* width */, LCD_HEIGHT /* height */, rgbpanel, 0 /* rotation */, true /* auto_flush */,
    bus, -1 /* RST */, st7701_type9_init_operations_lilygo, sizeof(st7701_type9_init_operations_lilygo));

#else
static const uint8_t st7701_4848S040_init[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x10,

    WRITE_C8_D16, 0xC0, 0x3B, 0x00,
    WRITE_C8_D16, 0xC1, 0x0D, 0x02,
    WRITE_C8_D16, 0xC2, 0x31, 0x05,
    WRITE_C8_D8, 0xCD, 0x00,  // 0xCD, 0x08 !!

    WRITE_COMMAND_8, 0xB0,  // Positive Voltage Gamma Control
    WRITE_BYTES, 16,
    0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
    0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18,

    WRITE_COMMAND_8, 0xB1,  // Negative Voltage Gamma Control
    WRITE_BYTES, 16,
    0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
    0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18,

    // PAGE1
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x11,

    WRITE_C8_D8, 0xB0, 0x60,  // Vop=4.7375v
    WRITE_C8_D8, 0xB1, 0x32,  // VCOM=32
    WRITE_C8_D8, 0xB2, 0x07,  // VGH=15v
    WRITE_C8_D8, 0xB3, 0x80,
    WRITE_C8_D8, 0xB5, 0x49,  // VGL=-10.17v
    WRITE_C8_D8, 0xB7, 0x85,
    WRITE_C8_D8, 0xB8, 0x21,  // AVDD=6.6 & AVCL=-4.6
    WRITE_C8_D8, 0xC1, 0x78,
    WRITE_C8_D8, 0xC2, 0x78,

    WRITE_COMMAND_8, 0xE0,
    WRITE_BYTES, 3, 0x00, 0x1B, 0x02,

    WRITE_COMMAND_8, 0xE1,
    WRITE_BYTES, 11,
    0x08, 0xA0, 0x00, 0x00,
    0x07, 0xA0, 0x00, 0x00,
    0x00, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE2,
    WRITE_BYTES, 12,
    0x11, 0x11, 0x44, 0x44,
    0xED, 0xA0, 0x00, 0x00,
    0xEC, 0xA0, 0x00, 0x00,

    WRITE_COMMAND_8, 0xE3,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE4, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE5,
    WRITE_BYTES, 16,
    0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
    0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0,

    WRITE_COMMAND_8, 0xE6,
    WRITE_BYTES, 4, 0x00, 0x00, 0x11, 0x11,

    WRITE_C8_D16, 0xE7, 0x44, 0x44,

    WRITE_COMMAND_8, 0xE8,
    WRITE_BYTES, 16,
    0x09, 0xE8, 0xD8, 0xA0,
    0x0B, 0xEA, 0xD8, 0xA0,
    0x0D, 0xEC, 0xD8, 0xA0,
    0x0F, 0xEE, 0xD8, 0xA0,

    WRITE_COMMAND_8, 0xEB,
    WRITE_BYTES, 7, 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40,

    WRITE_C8_D16, 0xEC, 0x3C, 0x00,

    WRITE_COMMAND_8, 0xED,
    WRITE_BYTES, 16,
    0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA,

    //-----------VAP & VAN---------------
    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x13,

    WRITE_C8_D8, 0xE5, 0xE4,

    WRITE_COMMAND_8, 0xFF,
    WRITE_BYTES, 5, 0x77, 0x01, 0x00, 0x00, 0x00,

    // WRITE_COMMAND_8, 0x21,   // 0x20 normal, 0x21 IPS !!
    WRITE_C8_D8, 0x3A, 0x50,  // 0x70 RGB888, 0x60 RGB666, 0x50 RGB565
    WRITE_COMMAND_8, 0x11,    // Sleep Out
    END_WRITE,

    DELAY, 120,

    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x29,  // Display On
    END_WRITE};
Arduino_DataBus* bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC */, SPI_LCD_CS /* CS */, SPI_LCD_SCLK /* SCK */, SPI_LCD_MOSI /* MOSI */, GFX_NOT_DEFINED /* MISO */);
Arduino_ESP32RGBPanel* rgbpanel = new Arduino_ESP32RGBPanel(
    LCD_DE /* DE */, LCD_VSYNC /* VSYNC */, LCD_HSYNC /* HSYNC */, LCD_PCLK /* PCLK */,
    LCD_R0 /* B0 */, LCD_R1 /* B1 */, LCD_R2 /* B2 */, LCD_R3 /* B3 */, LCD_R4 /* B4 */,
    LCD_G0 /* G0 */, LCD_G1 /* G1 */, LCD_G2 /* G2 */, LCD_G3 /* G3 */, LCD_G4 /* G4 */, LCD_G5 /* G5 */,
    LCD_B0 /* R0 */, LCD_B1 /* R1 */, LCD_B2 /* R2 */, LCD_B3 /* R3 */, LCD_B4 /* R4 */,
    1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */,
    10 /* pclk_active_neg */, 6000000L /* prefer_speed */, true);
Arduino_RGB_Display* gfx = new Arduino_RGB_Display(
    LCD_WIDTH /* width */, LCD_HEIGHT /* height */, rgbpanel, 0 /* rotation */, true /* auto_flush */,
    bus, -1 /* RST */, st7701_4848S040_init, sizeof(st7701_4848S040_init));

#endif
#endif

void TFTLog(String text) {
#if defined HAS_LILYGO_TPANEL || defined HAS_4inch_TPANEL

    gfx->setTextSize(2);

    if (tftLogscreen == false) {
        gfx->fillScreen(BLACK);
        gfx->setCursor(0, 0);
        tftLogscreen = true;
    }
    if (text.isEmpty()) return;
    gfx->setTextColor(WHITE);
    gfx->println(text);

#else

    if (tftLogscreen == false) {
        tft2.fillScreen(TFT_BLACK);
        tft2.setCursor(0, 0, (tft2.width() == 160 ? 1 : 2));
        tftLogscreen = true;
    } else {
        if (tft2.width() == 160) tft2.setCursor(0, tft2.getCursorY(), 1);
    }
    if (text.isEmpty()) return;
    tft2.setTextColor(TFT_SILVER);
    if (text.startsWith("!")) {
        tft2.setTextColor(TFT_RED);
        text = text.substring(1);
    } else if (text.indexOf("http") != -1) {
        int httpIndex = text.indexOf("http");
        tft2.print(text.substring(0, httpIndex));
        tft2.setTextColor(TFT_YELLOW);
        if (tft2.width() == 160) {
            tft2.setCursor(0, tft2.getCursorY() + 8, 2);
            text = text.substring(httpIndex + 7);
        } else {
            text = text.substring(httpIndex);
        }
    } else if (text.indexOf(":") != -1) {
        int colonIndex = text.indexOf(":");
        tft2.setTextColor(TFT_SILVER);
        tft2.print(text.substring(0, colonIndex + 1));
        tft2.setTextColor(TFT_WHITE);
        if (tft2.width() == 160) tft2.setCursor(0, tft2.getCursorY() + 8, 2);
        text = text.substring(colonIndex + 1);
    } else if (text.endsWith("!")) {
        tft2.setTextColor(TFT_GREEN);
    }
    tft2.println(text);
#endif
}

int32_t findId(uint8_t mac[8]) {
    for (uint32_t c = 0; c < tagDB.size(); c++) {
        tagRecord* tag = tagDB.at(c);
        if (memcmp(tag->mac, mac, 8) == 0 && tag->version == 0) {
            return c;
        }
    }
    return -1;
}

void sendAvail(uint8_t wakeupReason) {
    struct espAvailDataReq eadr = {0};
    uint8_t mac[6];
    WiFi.macAddress(mac);
    memcpy(&eadr.src, mac, 6);
    eadr.adr.lastPacketRSSI = WiFi.RSSI();
    eadr.adr.currentChannel = config.channel;
#if defined HAS_LILYGO_TPANEL || defined HAS_4inch_TPANEL
    eadr.adr.hwType = 0xE2;
#else
    eadr.adr.hwType = (tft2.width() == 160 ? 0xE1 : 0xE0);
#endif
    eadr.adr.wakeupReason = wakeupReason;
    eadr.adr.capabilities = 0;
    eadr.adr.tagSoftwareVersion = 0;
    eadr.adr.customMode = 0;
    processDataReq(&eadr, true);
}

void yellow_ap_display_init(void) {
#if defined HAS_LILYGO_TPANEL || defined HAS_4inch_TPANEL

    tftLogscreen = true;

    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);

#if ESP_ARDUINO_VERSION_MAJOR == 2
    ledcAttachPin(LCD_BL, 1);
    ledcSetup(1, 1000, 8);
    ledcWrite(1, config.tft);  // brightness
#else
    ledcAttachChannel(LCD_BL, 1000, 8, 1);
    ledcWriteChannel(1, config.tft);
#endif

#if defined HAS_LILYGO_TPANEL
    Wire.begin(IIC_SDA, IIC_SCL);
#endif
    gfx->begin();
    gfx->fillScreen(BLACK);

#else
    pinMode(YELLOW_SENSE, INPUT_PULLDOWN);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    if (digitalRead(YELLOW_SENSE) == HIGH) YellowSense = 1;
    pinMode(TFT_BACKLIGHT, OUTPUT);
    digitalWrite(TFT_BACKLIGHT, LOW);

    tft2.init();
#ifdef ST7735_NANO_TLSR
    YellowSense = 0;
    tft2.setRotation(1);
#else
    tft2.setRotation(YellowSense == 1 ? 1 : 3);
#endif
    tft2.fillScreen(TFT_BLACK);
    tft2.setCursor(12, 0, (tft2.width() == 160 ? 1 : 2));
    tft2.setTextColor(TFT_WHITE);
    tftLogscreen = true;

#if ESP_ARDUINO_VERSION_MAJOR == 2
    ledcSetup(1, 5000, 8);
    ledcAttachPin(TFT_BACKLIGHT, 1);
    ledcWrite(1, config.tft);
    if (tft2.width() == 160) {
        GPIO.func_out_sel_cfg[TFT_BACKLIGHT].inv_sel = 1;
    }
#else
    ledcAttachChannel(TFT_BACKLIGHT, 5000, 8, 1);
    ledcWriteChannel(1, config.tft);
    if (tft2.width() == 160) ledcOutputInvert(TFT_BACKLIGHT, true);
#endif
#endif
}

void yellow_ap_display_loop(void) {
    static bool first_run = 0;
    static time_t last_checkin = 0;
    static time_t last_update = 0;

    if (millis() - last_checkin >= 60000) {
        sendAvail(0);
        last_checkin = millis();
        tftLogscreen = false;
    }
    if (first_run == 0) {
        sendAvail(0xFC);
        first_run = 1;
    }
    if (millis() - last_update >= 3000) {
        uint8_t wifimac[8];
        WiFi.macAddress(wifimac);
        memset(&wifimac[6], 0, 2);
        tagRecord* tag = tagRecord::findByMAC(wifimac);
        if (tag == nullptr) {
            last_update = millis();
            return;
        }
        if (tag->pendingCount > 0 && tftOverride == false) {
            String filename = tag->filename;
            fs::File file = contentFS->open(filename);
            if (!file) {
                Serial.print("No current file. Canceling request\r\n");
                prepareCancelPending(tag->mac);
                return;
            }

            TFT_eSprite spr = TFT_eSprite(&tft2);
            spr.setColorDepth(16);
            if (tag->len == tft2.width() * tft2.height()) spr.setColorDepth(8);
            spr.createSprite(tft2.width(), tft2.height());
            void* spriteData = spr.getPointer();
            size_t bytesRead = file.readBytes((char*)spriteData, spr.width() * spr.height() * 2);
            file.close();

#if defined HAS_LILYGO_TPANEL || defined HAS_4inch_TPANEL
            long dy = spr.height();
            long dx = spr.width();

            uint16_t* data = static_cast<uint16_t*>(const_cast<void*>(spriteData));

            gfx->draw16bitRGBBitmap(0, 0, (uint16_t*)spriteData, dx, dy);
            spr.deleteSprite();
#else
            spr.pushSprite(0, 0);
#endif

            tftLogscreen = false;

            struct espXferComplete xfc = {0};
            memcpy(xfc.src, tag->mac, 8);
            processXferComplete(&xfc, true);
        }
        last_update = millis();
    }
}

#endif