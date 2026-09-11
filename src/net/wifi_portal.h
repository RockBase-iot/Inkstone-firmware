// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// wifi_portal — STA first + AP fallback + NVS credentials + mDNS + rescue hold
//
// Behavior (plan section 5.1.1):
//   1. On boot, read ssid/pass from NVS ("wifi") and try to connect for 15 s
//   2. Success: STA mode, mDNS <mdnsHost>.local
//   3. Failure / no credentials: AP Inkstone-XXXXXX (XXXXXX = last 3 MAC
//      bytes, distinguishes multiple devices; default password 12345678,
//      changeable via NVS), IP 192.168.4.1, wildcard DNS (captive portal)
//   4. Holding BOOT for 5 s at runtime: clear credentials and reboot to AP
// Credentials live only in NVS Preferences — never hardcoded.
// ============================================================================

#include <Arduino.h>

namespace wifiportal {

enum class Mode : uint8_t { STA = 0, AP = 1 };

// Blocks up to 15 s waiting for STA; falls back to AP on failure.
// Returns the final mode.
Mode begin();

// Call in loop(): AP-mode DNS replies, BOOT long-press rescue detection.
void tick();

Mode mode();
String ip();         // current IP as string
String ssid();       // STA: connected SSID; AP: hotspot name
String apSsid();     // AP hotspot name (Inkstone-XXXXXX, XXXXXX = last 3 STA MAC bytes, uppercase hex)
String macAddress(); // device STA MAC ("AA:BB:CC:DD:EE:FF")
String mdnsHost();   // full mDNS hostname (inkstone-xxxxxx)
bool isConnected();

// Provisioning: save credentials and reboot into STA (web form /api/setup)
void saveCredentialsAndReboot(const String& ssid, const String& pass);

// AP password management (changeable on the page, stored in NVS)
String apPassword();
void   setApPassword(const String& pass);   // takes effect after reboot

// API token (Phase 2): 16 random bytes as hex, generated on first boot, NVS-stored
String apiToken();
void   rotateApiToken();                    // generate and save a new token

} // namespace wifiportal
