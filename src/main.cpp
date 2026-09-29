// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

// ============================================================================
// Inkstone-firmware — main entry for the NM-EPD local image-push firmware
//
// State machine (plan section 6.2):
//   BOOT -> [low-battery guard] -> Wi-Fi portal -> HTTP service (AWAKE)
//        -> /display triggers REFRESHING (dedicated task, 25-30 s)
//        -> grace window -> idle timeout -> PRE_SLEEP -> DEEP_SLEEP
//        -> BOOT key ext0 wake back to AWAKE
// ============================================================================

#include <Arduino.h>
#include <esp_sleep.h>
#include "boards/board.h"
#include "display/display_service.h"
#include "display/frame_store.h"
#include "net/wifi_portal.h"
#include "net/web_server.h"
#include "net/pull_client.h"
#include "power/sleep_manager.h"
#include "power/battery.h"

static void disableUnusedPeripherals() {
    const int8_t enPins[] = {PIN_LORA_EN, PIN_CODEC_EN, PIN_TEMP_CTL, PIN_PA_CTRL};
    for (int8_t pin : enPins) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
    }
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.printf("\n=== Inkstone %s @ %s ===\n", "1.0.0", BOARD_NAME);

    pinMode(PIN_BOOT_BTN, INPUT);
    pinMode(PIN_USER_BTN, INPUT);
    disableUnusedPeripherals();

    // Sleep manager: restore RTC state, dispatch on wake cause
    sleepman::begin();

    // Low-battery guard (plan section 6.3: too low after wake -> skip service,
    // straight back to sleep)
    uint32_t mv = battery::readMv();
    Serial.printf("[boot] battery %u mV (%u%%)\n", (unsigned)mv,
                  (unsigned)battery::percent(mv));
    if (mv > 0 && mv < sleepman::params().battLowMv && !sleepman::stayAwake()) {
        Serial.println("[boot] battery low, back to sleep");
        sleepman::sleepNow();
        return;
    }

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        if (!pull::configured() || sleepman::params().schedWakeS == 0) {
            sleepman::sleepNow();
            return;
        }
        if (!wifiportal::connectStaOnly()) {
            Serial.println("[pull] STA unavailable; keeping current image");
            sleepman::sleepNow();
            return;
        }
        uint8_t* frame = (uint8_t*)ps_malloc(EPD_FRAME_BYTES);
        uint8_t* cached = (uint8_t*)ps_malloc(EPD_FRAME_BYTES);
        if (frame && cached && pull::fetchFrame(frame)) {
            bool same = framestore::load(cached) &&
                        memcmp(frame, cached, EPD_FRAME_BYTES) == 0;
            if (same) {
                Serial.println("[pull] image unchanged; skipping refresh");
            } else if (display::begin() && display::submitFrame(frame, EPD_FRAME_BYTES)) {
                while (display::isBusy()) delay(50);
                Serial.println("[pull] refresh complete");
            } else {
                Serial.println("[pull] display unavailable; keeping current image");
            }
        } else {
            Serial.println("[pull] fetch failed; keeping current image");
        }
        free(frame);
        free(cached);
        sleepman::sleepNow();
        return;
    }

    // Display service
    if (!display::begin()) {
        Serial.println("[boot] display init failed (PSRAM?)");
    }

    // Cold boot (not a deep-sleep wake): restore the cached user frame when
    // possible, otherwise run the built-in panel test pattern. Deep-sleep
    // wakes retain the existing e-paper image and do not refresh it.
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
        uint8_t* buf = (uint8_t*)ps_malloc(EPD_FRAME_BYTES);
        if (buf && framestore::load(buf)) {
            display::showSavedFrame(buf);
        } else {
            display::showTestPattern();
        }
        if (buf) free(buf);
    }

    // Wi-Fi: STA first, 15 s timeout, then AP + captive portal + mDNS
    wifiportal::Mode mode = wifiportal::begin();

    // HTTP service (Phase 1 page + Phase 2 API)
    web::begin();

    Serial.printf("[boot] ready. http://%s/  (mode=%s, stay_awake=%s)\n",
                  wifiportal::ip().c_str(),
                  mode == wifiportal::Mode::STA ? "sta" : "ap",
                  sleepman::stayAwake() ? "yes" : "no");
}

void loop() {
    web::handleClient();
    wifiportal::tick();    // AP DNS replies + BOOT long-press rescue
    sleepman::tick();      // idle detection -> deep sleep
    delay(2);
}
