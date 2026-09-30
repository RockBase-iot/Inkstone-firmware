// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.
    
#include "power/battery.h"
#include "boards/board.h"

#include <algorithm>

namespace battery {

uint32_t readMv() {
    if (PIN_BATT_ADC < 0) return 0;   // board has no battery divider
    if (PIN_ADC_EN >= 0) {
        pinMode(PIN_ADC_EN, OUTPUT);
        digitalWrite(PIN_ADC_EN, HIGH);
    }
    delay(BATT_ADC_SETTLE_MS);

    analogReadResolution(12);
    analogRead(PIN_BATT_ADC);   // establish the ADC channel first so later
                                // config / millivolt reads stay quiet
    analogSetPinAttenuation(PIN_BATT_ADC, ADC_11db);

    uint16_t samples[BATT_ADC_SAMPLES];
    for (int i = 0; i < BATT_ADC_SAMPLES; ++i) {
        samples[i] = analogReadMilliVolts(PIN_BATT_ADC);
        delay(2);
    }
    if (PIN_ADC_EN >= 0) digitalWrite(PIN_ADC_EN, LOW);

    std::sort(samples, samples + BATT_ADC_SAMPLES);
    uint32_t median = samples[BATT_ADC_SAMPLES / 2];
    return median * BATT_ADC_DIV;
}

uint8_t percent(uint32_t mv) {
    if (mv <= 2500) return 0;
    if (mv >= 4500) return 100;
    return (uint8_t)((mv - 2500) * 100 / 2000);
}

} // namespace battery
