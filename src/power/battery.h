// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// battery — battery voltage ADC (timing per factory config.h notes,
// plan section 6.3 hard rule 5)
//
// PIN_ADC_EN=HIGH -> wait 80 ms -> 24 samples, take median -> x BATT_ADC_DIV
// -> mV -> pull enable low
// ============================================================================

#include <Arduino.h>

namespace battery {

// Read battery voltage in mV. Returns <= 0 on failure.
uint32_t readMv();

// Estimated remaining charge in percent (linear over 2500-4500 mV).
uint8_t percent(uint32_t mv);

} // namespace battery
