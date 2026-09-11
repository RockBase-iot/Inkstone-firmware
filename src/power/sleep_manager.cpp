// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#include "power/sleep_manager.h"
#include "boards/board.h"
#include "display/display_service.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <Preferences.h>

namespace sleepman {

static const uint32_t RTC_MAGIC = 0x4E4D4550;  // "NMEP"

static RTC_DATA_ATTR RtcState s_rtc;

static Params   s_params;
static uint32_t s_lastActivityMs = 0;
static uint32_t s_sleepRequestAt = 0;      // millis deadline, 0 = no request
static bool     s_timerWakeNoPull = false; // timer wake with no pull configured -> back to sleep

static void loadParams() {
    s_params.idleTimeoutS        = IDLE_TIMEOUT_S;
    s_params.postRefreshGraceS   = POST_REFRESH_GRACE_S;
    s_params.minRefreshIntervalS = MIN_REFRESH_INTERVAL_S;
    s_params.schedWakeS          = SCHED_WAKE_S;
    s_params.battLowMv           = BATT_LOW_MV;

    Preferences p;
    p.begin("sleep", true);
    s_params.idleTimeoutS      = p.getUInt("idle",  s_params.idleTimeoutS);
    s_params.postRefreshGraceS = p.getUInt("grace", s_params.postRefreshGraceS);
    s_params.schedWakeS        = p.getUInt("sched", s_params.schedWakeS);
    s_params.battLowMv         = p.getUInt("battmv", s_params.battLowMv);
    // minRefreshInterval may only be raised (hard floor protects panel lifetime)
    uint32_t mri = p.getUInt("minri", s_params.minRefreshIntervalS);
    if (mri > s_params.minRefreshIntervalS) s_params.minRefreshIntervalS = mri;
    p.end();
}

void begin() {
    if (s_rtc.magic != RTC_MAGIC) {
        memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.magic = RTC_MAGIC;
    }
    s_rtc.bootCount++;
    s_lastActivityMs = millis();
    loadParams();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:
            Serial.printf("[sleep] wake by BOOT key (boot #%u)\n", s_rtc.bootCount);
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            // Phase 3 pull reserved: no pull URL configured -> go back to sleep
            Serial.println("[sleep] wake by timer, no pull source configured");
            s_timerWakeNoPull = true;
            break;
        default:
            Serial.printf("[sleep] cold boot (boot #%u)\n", s_rtc.bootCount);
            break;
    }
}

// ─── Pre-sleep sequence (plan section 6.3 hard rule 2, order is fixed) ──────
static void enterDeepSleep() {
    Serial.println("[sleep] entering deep sleep...");

    // (1) RTC state
    s_rtc.lastUpdateEpoch = display::lastRefreshEpoch();

    // (2) cut panel drive voltage
    display::powerOff();

    // (3) pull all peripheral-enable pins low
    const int8_t enPins[] = {PIN_LORA_EN, PIN_CODEC_EN, PIN_TEMP_CTL,
                             PIN_PA_CTRL, PIN_ADC_EN};
    for (int8_t p : enPins) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }

    // (4) Wi-Fi off
    WiFi.disconnect(true);
    esp_wifi_stop();

    // (5) wake sources: BOOT key (pressed = LOW) + optional timer
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BOOT_BTN, 0);
    if (s_params.schedWakeS > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)s_params.schedWakeS * 1000000ULL);
    }

    // (6) deep sleep
    Serial.flush();
    esp_deep_sleep_start();
}

void tick() {
    // Hard rule 1: never sleep while refreshing
    if (display::isBusy()) {
        s_lastActivityMs = millis();
        return;
    }
    if (s_rtc.stayAwake) return;

    // Timer wake with no pull configured -> straight back to sleep
    if (s_timerWakeNoPull) { enterDeepSleep(); return; }

    uint32_t now = millis();

    // API-requested delayed sleep
    if (s_sleepRequestAt != 0 && (int32_t)(now - s_sleepRequestAt) >= 0) {
        enterDeepSleep();
        return;
    }

    // Forced awake window after each refresh
    uint32_t lastRefresh = display::lastRefreshMillis();
    if (lastRefresh != 0 &&
        now - lastRefresh < s_params.postRefreshGraceS * 1000UL) {
        return;
    }

    // Idle timeout -> PRE_SLEEP -> DEEP_SLEEP
    if (now - s_lastActivityMs >= s_params.idleTimeoutS * 1000UL) {
        enterDeepSleep();
    }
}

void notifyActivity() { s_lastActivityMs = millis(); }

bool stayAwake()          { return s_rtc.stayAwake != 0; }
void setStayAwake(bool on) {
    s_rtc.stayAwake = on ? 1 : 0;
    s_lastActivityMs = millis();
}

const Params& params() { return s_params; }

void setParams(const Params& p) {
    // minRefreshInterval hard floor: may only be raised
    s_params = p;
    if (s_params.minRefreshIntervalS < MIN_REFRESH_INTERVAL_S) {
        s_params.minRefreshIntervalS = MIN_REFRESH_INTERVAL_S;
    }
    Preferences pref;
    pref.begin("sleep", false);
    pref.putUInt("idle",  s_params.idleTimeoutS);
    pref.putUInt("grace", s_params.postRefreshGraceS);
    pref.putUInt("sched", s_params.schedWakeS);
    pref.putUInt("battmv", s_params.battLowMv);
    pref.putUInt("minri", s_params.minRefreshIntervalS);
    pref.end();
}

void requestSleep(uint32_t delayS) {
    s_sleepRequestAt = millis() + delayS * 1000UL;
}

uint32_t nextAllowedUpdateInS() {
    // Rate limiting counts user-requested refreshes only (boot self-check excluded)
    uint32_t last = display::lastUserRefreshMillis();
    if (last == 0) return 0;
    uint32_t elapsed = (millis() - last) / 1000;
    return elapsed >= s_params.minRefreshIntervalS
               ? 0 : s_params.minRefreshIntervalS - elapsed;
}

uint32_t sleepInS() {
    if (s_rtc.stayAwake) return 0;
    if (s_sleepRequestAt != 0) {
        int32_t d = (int32_t)(s_sleepRequestAt - millis());
        return d > 0 ? (uint32_t)(d / 1000) : 0;
    }
    uint32_t idleFor = (millis() - s_lastActivityMs) / 1000;
    return idleFor >= s_params.idleTimeoutS ? 0 : s_params.idleTimeoutS - idleFor;
}

uint32_t bootCount() { return s_rtc.bootCount; }

} // namespace sleepman
