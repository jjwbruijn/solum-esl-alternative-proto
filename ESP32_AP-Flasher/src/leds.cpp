#include <Arduino.h>

#ifdef HAS_RGB_LED
#define FASTLED_INTERNAL
#include <FastLED.h>
#endif

#include "leds.h"
#include "settings.h"
#include "tag_db.h"
#include "serialap.h"

QueueHandle_t ledQueue;
int maxledbrightness = 255;

#ifdef HAS_RGB_LED
QueueHandle_t rgbLedQueue;
CRGB rgbIdleColor = CRGB::Green;
uint16_t rgbIdlePeriod = 511;

struct ledInstructionRGB {
    CRGB ledColor;
    uint16_t fadeTime;
    uint16_t length;
    bool reQueue = false;
};

CRGB leds[1];
volatile bool rgbQueueFlush = false;
#endif

struct ledInstruction {
    uint16_t value;
    uint16_t fadeTime;
    uint16_t length;
    bool reQueue = false;
};

void ledcSet(uint8_t channel, uint8_t brightness) {
#if ESP_ARDUINO_VERSION_MAJOR == 2
    ledcWrite(channel, brightness);
#else
    ledcWriteChannel(channel, brightness);
#endif 
}

#ifdef HAS_RGB_LED

void addToRGBQueue(struct ledInstructionRGB* rgb, bool requeue) {
    rgb->reQueue = requeue;
    if (!rgbLedQueue) {
        delete rgb;
        return;
    }
    BaseType_t queuestatus = xQueueSend(rgbLedQueue, &rgb, 0);
    if (queuestatus == pdFALSE) {
        delete rgb;
    }
}

void addFadeColor(CRGB cname) {
    struct ledInstructionRGB* rgb = new struct ledInstructionRGB;
    rgb->ledColor = cname;
    rgb->fadeTime = 750;
    rgb->length = 0;
    addToRGBQueue(rgb, false);
}

void shortBlink(CRGB cname) {
    struct ledInstructionRGB* rgb = new struct ledInstructionRGB;
    rgb->ledColor = CRGB::Black;
    rgb->fadeTime = 0;
    rgb->length = 3;
    addToRGBQueue(rgb, false);
    rgb = new struct ledInstructionRGB;
    rgb->ledColor = cname;
    rgb->ledColor.maximizeBrightness(0x80);
    rgb->fadeTime = 0;
    rgb->length = 10;
    addToRGBQueue(rgb, false);
    rgb = new struct ledInstructionRGB;
    rgb->ledColor = CRGB::Black;
    rgb->fadeTime = 0;
    rgb->length = 3;
    addToRGBQueue(rgb, false);
}

void flushRGBQueue() {
    rgbQueueFlush = true;
}

void rgbIdle() {
    flushRGBQueue();
}

void showColorPattern(CRGB colorone, CRGB colortwo, CRGB colorthree) {
    struct ledInstructionRGB* rgb;
    const int patternLengths[] = {600, 120, 200, 120, 200, 120};
    const CRGB patternColors[] = {CRGB::Black, colorone, CRGB::Black, colortwo, CRGB::Black, colorthree};

    while (xQueueReceive(rgbLedQueue, &rgb, 0) == pdPASS) { }

    for (int i = 0; i < sizeof(patternLengths) / sizeof(patternLengths[0]); i++) {
        rgb = new struct ledInstructionRGB;
        rgb->ledColor = patternColors[i];
        rgb->fadeTime = 0;
        rgb->length = patternLengths[i];
        addToRGBQueue(rgb, true);
    }
}

void showRGB() {
    FastLED.show();
}

void rgbIdleStep() {
    static bool dirUp = true;
    static uint16_t step = 0;

    if (dirUp) {
        // up
        step++;
        if (step >= rgbIdlePeriod) {
            dirUp = false;
        }
    } else {
        // down
        step--;
        if (step == 0) {
            dirUp = true;
        }
    }
    CRGB newvalue = blend(CRGB::Black, (const CRGB&)rgbIdleColor, map(step, 0, rgbIdlePeriod, 0, 255));
    if (newvalue != leds[0]) {
        leds[0] = newvalue;
        showRGB();
    }
}
#endif

void setBrightness(int brightness) {
    maxledbrightness = brightness;

#if defined HAS_LILYGO_TPANEL || defined HAS_4inch_TPANEL || HAS_TFT
    ledcSet(1, config.tft);
#endif
#ifdef HAS_RGB_LED
    FastLED.setBrightness(maxledbrightness);
#endif
}

void updateBrightnessFromConfig() {
    int newbrightness = config.led;
    if (newbrightness != maxledbrightness) {
        setBrightness(newbrightness);
    }
#if defined HAS_LILYGO_TPANEL || defined HAS_4inch_TPANEL || HAS_TFT
    ledcSet(1, config.tft);
#endif
    if (apInfo.state == AP_STATE_NORADIO) addFadeMono(config.led);
}

void addToMonoQueue(struct ledInstruction* mono) {
    BaseType_t queuestatus = xQueueSend(ledQueue, &mono, 0);
    if (queuestatus == pdFALSE) {
        delete mono;
    }
}

void addFadeMono(uint8_t value) {
    struct ledInstruction* mono = new struct ledInstruction;
    mono->value = value;
    mono->fadeTime = 750;
    mono->length = 0;
    addToMonoQueue(mono);
}

void showMono(uint8_t brightness) {
    ledcSet(7, gamma8[brightness]);
}

void quickBlink(uint8_t repeat) {
    for (int i = 0; i < repeat; i++) {
        struct ledInstruction* mono = new struct ledInstruction;
#ifdef HAS_TFT
        mono->value = 255;
#else
        mono->value = maxledbrightness;
#endif
        mono->fadeTime = 120 / repeat;
        mono->length = 0;
        addToMonoQueue(mono);
        mono = new struct ledInstruction;
        mono->value = 0;
        mono->fadeTime = 120 / repeat;
        mono->length = 0;
        addToMonoQueue(mono);
    }
}

volatile uint16_t monoIdlePeriod = 900;

void ledTask(void* parameter) {
#ifdef HAS_RGB_LED
    FastLED.addLeds<WS2812B, FLASHER_RGB_LED, GRB>(leds, 1);  // GRB ordering is typical
    leds[0] = CRGB::Blue;
    showRGB();
    rgbLedQueue = xQueueCreate(30, sizeof(struct ledInstructionRGB*));

    struct ledInstructionRGB* rgb = nullptr;
    // open with a nice RGB crossfade
    addFadeColor(CRGB::Red);
    addFadeColor(CRGB::Green);
    addFadeColor(CRGB::Blue);
    CRGB oldColor = CRGB::Black;
    uint16_t rgbInstructionFadeTime = 0;
#endif

    ledQueue = xQueueCreate(30, sizeof(struct ledInstruction*));

    if (FLASHER_LED != -1) {
#if ESP_ARDUINO_VERSION_MAJOR == 2
        ledcSetup(7, 5000, 8);
        ledcAttachPin(FLASHER_LED, 7);
#else
        ledcAttachChannel(FLASHER_LED, 1000, 8, 7);
#endif
    }

    struct ledInstruction* monoled = nullptr;

    addFadeMono(0);
#ifdef HAS_TFT
    addFadeMono(255);
#else
    addFadeMono(maxledbrightness);
#endif
    addFadeMono(0);

    uint8_t oldBrightness = 0;

    uint16_t monoInstructionFadeTime = 0;

    while (1) {
#ifdef HAS_RGB_LED
        // handle RGB led instructions
        if (rgb == nullptr) {
            // fetch a led instruction
            BaseType_t q = xQueueReceive(rgbLedQueue, &rgb, 1);
            if (q == pdTRUE) {
                if (rgb->reQueue && !rgbQueueFlush) {
                    // requeue this instruction at the end of the queue, caveman style.
                    struct ledInstructionRGB* requeue = new ledInstructionRGB;
                    requeue->fadeTime = rgb->fadeTime;
                    requeue->ledColor = rgb->ledColor;
                    requeue->length = rgb->length;
                    addToRGBQueue(requeue, true);
                }

                if (rgbQueueFlush) {
                    delete rgb;
                    rgb = nullptr;
                } else {
                    rgbInstructionFadeTime = rgb->fadeTime;
                    if (rgb->fadeTime <= 1) {
                        leds[0] = rgb->ledColor;
                        showRGB();
                    }
                }
            } else {
                rgbQueueFlush = false;
                // no commands, run idle led task
                rgbIdleStep();
            }
        } else {
            // process instruction
            if (rgb->fadeTime) {
                rgb->fadeTime--;
                leds[0] = blend(rgb->ledColor, oldColor, map(rgb->fadeTime, 0, rgbInstructionFadeTime, 0, 255));
                showRGB();
            } else if (rgb->length) {
                rgb->length--;
            } else {
                oldColor = rgb->ledColor;
                delete rgb;
                rgb = nullptr;
            }
        }
#endif
        // handle flasher LED (single color)
        if (monoled == nullptr) {
            BaseType_t q = xQueueReceive(ledQueue, &monoled, 1);
            if (q == pdTRUE) {
                monoInstructionFadeTime = monoled->fadeTime;
                if (monoled->fadeTime <= 1) {
                    showMono(monoled->value);
                }
            } 
        } else {
            if (monoled->fadeTime) {
                monoled->fadeTime--;
                showMono(map(monoled->fadeTime, 0, monoInstructionFadeTime, monoled->value, oldBrightness));
            } else if (monoled->length) {
                monoled->length--;
            } else {
                oldBrightness = monoled->value;
                delete monoled;
                monoled = nullptr;
            }
        }

        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
}