// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// sleep_manager — windowed sleep state machine (plan section 6)
//
// States: BOOT -> AWAKE(serve) -> REFRESHING(25-30s) -> AWAKE(grace)
//         -> PRE_SLEEP(idle timeout) -> DEEP_SLEEP -> ext0(BOOT key)/timer wake
//
// Hard rules (plan section 6.3, must be implemented):
//   1. Never sleep while refreshing
//   2. Pre-sleep sequence order is fixed: RTC state -> powerOff -> pull all
//      peripheral-enable pins low -> WiFi off -> configure wake sources ->
//      esp_deep_sleep_start()
//   3. Wake-cause dispatch: ext0 = button -> AWAKE; timer = pull reserved
//      (back to sleep immediately when unconfigured)
//   4. GPIO0 doubles as the download strap: do not hold BOOT at power-on
// ============================================================================

#include <Arduino.h>

namespace sleepman {

struct Params {
    uint32_t idleTimeoutS;        // sleep after this long without HTTP activity
    uint32_t postRefreshGraceS;   // forced awake time after each refresh
    uint32_t minRefreshIntervalS; // min interval between refreshes (429 basis)
    uint32_t schedWakeS;          // scheduled wake period, 0 = disabled (pull reserved)
    uint32_t battLowMv;           // low-battery threshold
};

// State kept across deep sleep in RTC RAM
struct RtcState {
    uint32_t magic;
    uint32_t bootCount;
    uint32_t lastUpdateEpoch;
    uint8_t  stayAwake;
};

// Call in setup(): restore RTC state, load NVS parameter overrides,
// dispatch on the wake cause.
void begin();

// Call in loop(): idle detection, then pre-sleep sequence and deep sleep.
void tick();

// Call on any HTTP activity; resets the idle timer.
void notifyActivity();

// Stay-awake mode (kept in RTC, survives reboot)
bool stayAwake();
void setStayAwake(bool on);

// Parameters (NVS overrides + board-header defaults)
const Params& params();
void setParams(const Params& p);   // persisted to NVS

// Enter sleep N seconds from now (called by /api/v1/sleep); 0 = let the
// normal idle path decide.
void requestSleep(uint32_t delayS);

// Seconds until the next allowed refresh (0 = allowed now)
uint32_t nextAllowedUpdateInS();

// Seconds until the expected sleep (for the /status sleep_in_s field)
uint32_t sleepInS();

uint32_t bootCount();

} // namespace sleepman
