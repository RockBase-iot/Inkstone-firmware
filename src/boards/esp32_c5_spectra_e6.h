// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// ESP32-C5-Spectra-E6 board definition
//
// Hardware reference: RockBase-iot/esp32-spectra-e6
//   include/board_esp32_c5_spectra_e6.h + src/Epd1200x1600E6.cpp
//   (ESP32-C5-WROOM-1 + GDEB0709E01 1200x1600 Spectra E6 dual-controller panel)
//
// Panel: GDEB0709E01 (Spectra 6), 1200x1600 portrait, 6 colors
//        (black / white / green / blue / red / yellow), full refresh ~30-45 s,
//        no partial refresh. Dual-controller panel: master + slave CS.
//
// Wire frame format: 4 bits/px (2 px per byte, MSB nibble first), row-major,
// nibble = GxEPD2 7-color index 0=black 1=white 2=green 3=blue 4=red 5=yellow
// (the EPD driver translates these to panel-native codes on write, see
// src/drivers/epd1200x1600_e6.cpp convertByte()).
// 1200*1600/2 = 960000 bytes per frame.
//
// Wi-Fi: ESP32-C5 is a dual-band (2.4 GHz + 5 GHz) Wi-Fi 6 radio. STA mode
// uses WIFI_BAND_MODE_AUTO (set in wifi_portal.cpp), so saved networks on
// either band connect without configuration changes.
// ============================================================================

#define BOARD_NAME       "ESP32-C5-Spectra-E6"
#define BOARD_MDNS_HOST  "inkstone"           // same hostname on all boards -> inkstone.local
#define BOARD_AP_SSID_PREFIX "Inkstone-E6-"   // last 3 MAC bytes (uppercase hex) appended at runtime
#define BOARD_AP_DEFAULT_PASS "12345678"      // changeable on the page, stored in NVS

// ─── Panel / pixel format ───────────────────────────────────────────────────
// 4 bits/px: 0=black 1=white 2=green 3=blue 4=red 5=yellow; 2 pixels per byte,
// MSB nibble first; row-major, no padding: 1200*1600/2 = 960000 bytes.
#define EPD_WIDTH        1200
#define EPD_HEIGHT       1600
#define EPD_COLORS       6
#define EPD_FRAME_BYTES  (EPD_WIDTH * EPD_HEIGHT / 2)
#define EPD_BYTE_WHITE   0x11   // full byte of "white" pixels (nibble 1, twice)

// ─── EPD pins (dual controller: CS_M master + CS_S slave) ──────────────────
#define PIN_EPD_SCK    6
#define PIN_EPD_MOSI   7
#define PIN_EPD_CS     5    // CS_M (master controller)
#define PIN_EPD_CS_S   4    // CS_S (slave controller)
#define PIN_EPD_DC     8
#define PIN_EPD_RST    9
#define PIN_EPD_BUSY  10

// ─── Buttons ────────────────────────────────────────────────────────────────
// DevKitC-1 BOOT key is GPIO28 (a strapping pin, pressed=LOW). GPIO28 is NOT
// an RTC GPIO, so it cannot be an ext0 deep-sleep wake source; sleep_manager
// logs a warning and sleeps with the timer only. Rescue (long-press to clear
// Wi-Fi credentials) still works while awake.
#define PIN_BOOT_BTN  28
#define PIN_USER_BTN  -1    // not populated

// ─── Peripheral power enables (not present on this board) ───────────────────
#define PIN_LORA_EN   -1
#define PIN_CODEC_EN  -1
#define PIN_ADC_EN    -1
#define PIN_TEMP_CTL  -1
#define PIN_PA_CTRL   -1

// ─── Battery ADC ────────────────────────────────────────────────────────────
// The reference board (esp32-spectra-e6) has a 2:1 divider on GPIO2. The
// DevKitC-1 has NO battery circuit: GPIO2 floats, reads low, and the boot-time
// low-battery guard would send the board straight back to deep sleep — and
// with no ext0 wake on C5 and no scheduled wake configured, it would never
// come back. -1 disables the measurement; battery::readMv() then returns 0
// and the guard (mv > 0 && mv < BATT_LOW_MV) never triggers. Re-point this at
// GPIO2 when a divider is actually wired.
#define PIN_BATT_ADC          -1
#define BATT_ADC_DIV           2
#define BATT_ADC_SETTLE_MS    80
#define BATT_ADC_SAMPLES      24

// ─── Sleep parameter defaults (overridable via API/NVS) ─────────────────────
#define IDLE_TIMEOUT_S          300
#define POST_REFRESH_GRACE_S    120
#define MIN_REFRESH_INTERVAL_S   60
#define SCHED_WAKE_S              0   // 0 = scheduled wake disabled
#define BATT_LOW_MV            3400
