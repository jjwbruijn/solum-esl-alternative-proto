#include "powermgt.h"

#include <Arduino.h>

#include "settings.h"
#include "leds.h"

#ifdef HAS_EXT_FLASHER
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#endif

void simpleAPPower(uint8_t* pin, uint8_t pincount, bool state) {
    for (uint8_t c = 0; c < pincount; c++) {
        pinMode(pin[c], INPUT);
    }
    for (uint8_t c = 0; c < pincount; c++) {
#ifdef POWER_HIGH_SIDE_DRIVER
        digitalWrite(pin[c], !state);
#else
        digitalWrite(pin[c], state);
#endif
    }
    for (uint8_t c = 0; c < pincount; c++) {
        pinMode(pin[c], OUTPUT);
    }
}

#ifdef POWER_RAMPING
// On the OpenEPaperLink board, there is no in-rush current limiting. The tags that can be connected to the board can have significant capacity, which,
// when drained if the board applies power, will cause the 3v3 rail to sag enough to reset the ESP32. This is obviously not great. To prevent this from happening,
// we ramp up/down the voltage with PWM. Ramping down really is unnecessary, as the board has a resistor to dump the charge into.
void rampTagPower(uint8_t* pin, bool up) {
#ifdef HAS_EXT_FLASHER
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
#endif
    if (up) {
#if ESP_ARDUINO_VERSION_MAJOR == 2
        ledcSetup(0, 50000, 8);
        ledcWrite(0, 254);
        vTaskDelay(1 / portTICK_PERIOD_MS);
        pinMode(pin[0], OUTPUT);
        ledcAttachPin(pin[0], 0);
#else
        ledcWriteChannel(0, 254);
        ledcAttachChannel(pin[0], 50000, 8, 0);
#endif
        pinMode(FLASHER_EXT_RESET, OUTPUT);
        digitalWrite(FLASHER_EXT_RESET, LOW);
        vTaskDelay(10 / portTICK_PERIOD_MS);
        for (uint8_t c = 254; c != 0xFF; c--) {
            ledcSet(0, c);
            delayMicroseconds(700);
        }
        digitalWrite(pin[0], LOW);
        ledcDetachPin(pin[0]);
        digitalWrite(pin[0], LOW);
        digitalWrite(FLASHER_EXT_RESET, INPUT_PULLUP);
    } else {
        ledcSetup(0, 50000, 8);
        ledcSet(0, 0);
        vTaskDelay(1 / portTICK_PERIOD_MS);
        pinMode(pin[0], OUTPUT);
        pinMode(FLASHER_EXT_RESET, INPUT_PULLDOWN);
        ledcAttachPin(pin[0], 0);
        vTaskDelay(10 / portTICK_PERIOD_MS);
        for (uint8_t c = 0; c < 0xFF; c++) {
            ledcSet(0, c);
            if (c > 250) {
                vTaskDelay(2 / portTICK_PERIOD_MS);
            } else {
                delayMicroseconds(500);
            }
        }
        digitalWrite(pin[0], HIGH);
        ledcDetachPin(pin[0]);
        digitalWrite(pin[0], HIGH);
    }
#ifdef HAS_EXT_FLASHER
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 1);
#endif
}
#endif

void powerControl(bool powerState, uint8_t* pin, uint8_t pincount) {
    if (pincount == 0) return;
    if (pin == nullptr) return;

    static bool currentState = false;
    if (currentState == powerState) return;
    currentState = powerState;

#ifdef POWER_RAMPING
    if (powerState == true) {
#ifdef POWER_HIGH_SIDE_DRIVER
        rampTagPower(pin, true);
#else
        rampTagPower(pin, false);
#endif
    } else {
/*
        pinMode(pin[0], OUTPUT);
#ifdef POWER_HIGH_SIDE_DRIVER
        digitalWrite(pin[0], HIGH);
#else
        digitalWrite(pin[0], LOW);
#endif
*/
#ifdef POWER_HIGH_SIDE_DRIVER
        rampTagPower(pin, false);
#else
        rampTagPower(pin, true);
#endif
    }
#else
    simpleAPPower(pin, pincount, powerState);
    delay(500);
    // simpleAPPower(pin, pincount, true);
#endif
    }