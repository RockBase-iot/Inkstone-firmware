// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// NM-EPD-420-4C board definition (Ground Truth)
//
// Source: RockBase-iot/NM-EPD-420 factory firmware src/config.h (FW v1.4.05)
//         + project plan sections 2/3. Hardware constants must not be changed
//         ad hoc; open an issue first if anything disagrees.
//
// Panel: GDEY0420F51 (HX8717 controller), 400x300 landscape, B/W/R/Y,
//        full refresh 25-30 s, no partial refresh.
// ============================================================================

#define BOARD_NAME       "NM-EPD-420-4C"
#define BOARD_MDNS_HOST  "inkstone"           // fixed runtime hostname -> inkstone.local
#define BOARD_AP_SSID_PREFIX "Inkstone-"      // last 3 MAC bytes (uppercase hex) appended at runtime
#define BOARD_AP_DEFAULT_PASS "12345678"      // changeable on the page, stored in NVS

// ─── Panel / pixel format (plan section 3) ──────────────────────────────────
// 2 bits/px: 0=black 1=white 2=yellow 3=red; 4 pixels per byte, MSB first;
// row-major, no padding: 400*300/4 = 30000 bytes.
#define EPD_WIDTH        400
#define EPD_HEIGHT       300
#define EPD_COLORS       4
#define EPD_FRAME_BYTES  (EPD_WIDTH * EPD_HEIGHT / 4)

// ─── EPD pins (dedicated FSPI/SPI2 bus; MISO is panel NC, never claim it) ───
#define PIN_EPD_SCK    2
#define PIN_EPD_MOSI   1
#define PIN_EPD_CS    46
#define PIN_EPD_DC     4
#define PIN_EPD_RST    5
#define PIN_EPD_BUSY   6

// ─── Buttons ────────────────────────────────────────────────────────────────
#define PIN_BOOT_BTN   0    // RTC GPIO, ext0 deep-sleep wake source, pressed=LOW
#define PIN_USER_BTN  45    // not an RTC GPIO, runtime use only

// ─── Peripheral power enables (HIGH=on, all pulled LOW before sleep) ────────
#define PIN_LORA_EN   47
#define PIN_CODEC_EN  44
#define PIN_ADC_EN    43
#define PIN_TEMP_CTL  40
#define PIN_PA_CTRL   41

// ─── Battery ADC (2:1 divider) ──────────────────────────────────────────────
#define PIN_BATT_ADC           3
#define BATT_ADC_DIV           2
#define BATT_ADC_SETTLE_MS    80
#define BATT_ADC_SAMPLES      24

// ─── Sleep parameter defaults (plan section 6.3, overridable via API/NVS) ───
#define IDLE_TIMEOUT_S          300
#define POST_REFRESH_GRACE_S    120
#define MIN_REFRESH_INTERVAL_S   60
#define SCHED_WAKE_S              0   // 0 = scheduled wake disabled
#define BATT_LOW_MV            3400
