// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#include "net/wifi_portal.h"
#include "boards/board.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_mac.h>

namespace wifiportal {

static const uint32_t STA_TIMEOUT_MS   = 15000;
static const uint32_t BOOT_HOLD_MS     = 5000;   // long-press to clear credentials
static const byte     DNS_PORT         = 53;

static Mode       s_mode = Mode::AP;
static DNSServer  s_dns;
static String     s_ssid;
static String     s_mac;       // "AA:BB:CC:DD:EE:FF"
static String     s_macTail;   // last 3 MAC bytes, uppercase hex, e.g. "2BF864"
static String     s_apSsid;    // Inkstone-XXXXXX
static String     s_mdnsHost;  // inkstone-xxxxxx
static uint32_t   s_bootPressedAt = 0;

static Preferences& prefs() {
    static Preferences p;
    return p;
}

// ─── token ──────────────────────────────────────────────────────────────────
static String genToken() {
    char buf[33];
    for (int i = 0; i < 16; ++i) sprintf(buf + i * 2, "%02x", (uint8_t)esp_random());
    return String(buf);
}

String apiToken() {
    prefs().begin("auth", true);
    String t = prefs().getString("token", "");
    prefs().end();
    if (t.length() == 32) return t;
    t = genToken();
    prefs().begin("auth", false);
    prefs().putString("token", t);
    prefs().end();
    Serial.printf("[wifi] API token generated: %s\n", t.c_str());
    return t;
}

void rotateApiToken() {
    String t = genToken();
    prefs().begin("auth", false);
    prefs().putString("token", t);
    prefs().end();
    Serial.printf("[wifi] API token rotated: %s\n", t.c_str());
}

// ─── AP password ────────────────────────────────────────────────────────────
String apPassword() {
    prefs().begin("wifi", true);
    String p = prefs().getString("ap_pass", BOARD_AP_DEFAULT_PASS);
    prefs().end();
    return p.length() >= 8 ? p : String(BOARD_AP_DEFAULT_PASS);
}

void setApPassword(const String& pass) {
    if (pass.length() < 8) return;
    prefs().begin("wifi", false);
    prefs().putString("ap_pass", pass);
    prefs().end();
}

// ─── provisioning ───────────────────────────────────────────────────────────
void saveCredentialsAndReboot(const String& ssid, const String& pass) {
    prefs().begin("wifi", false);
    prefs().putString("ssid", ssid);
    prefs().putString("pass", pass);
    prefs().end();
    Serial.printf("[wifi] credentials saved (ssid=%s), rebooting...\n", ssid.c_str());
    delay(500);
    ESP.restart();
}

static void startMdns() {
    if (MDNS.begin(s_mdnsHost.c_str())) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[wifi] mDNS: http://%s.local\n", s_mdnsHost.c_str());
    } else {
        Serial.println("[wifi] mDNS start failed");
    }
}

Mode begin() {
    // Read the STA MAC straight from eFuse (device identity; always valid
    // even before Wi-Fi starts — WiFi.macAddress() can return 00:00:00:00:00:00
    // if the STA interface is not up yet)
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_mac = macStr;
    char tail[7];
    snprintf(tail, sizeof(tail), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    s_macTail  = tail;
    s_apSsid   = String(BOARD_AP_SSID_PREFIX) + s_macTail;
    s_mdnsHost = String(BOARD_MDNS_HOST) + "-" + s_macTail;
    s_mdnsHost.toLowerCase();
    Serial.printf("[wifi] MAC=%s  AP=%s  mDNS=%s.local\n",
                  s_mac.c_str(), s_apSsid.c_str(), s_mdnsHost.c_str());

    prefs().begin("wifi", true);
    String ssid = prefs().getString("ssid", "");
    String pass = prefs().getString("pass", "");
    prefs().end();

    if (ssid.length() > 0) {
        Serial.printf("[wifi] connecting to %s ...\n", ssid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_TIMEOUT_MS) {
            delay(250);
        }
        if (WiFi.status() == WL_CONNECTED) {
            s_mode = Mode::STA;
            s_ssid = ssid;
            Serial.printf("[wifi] STA connected, IP=%s\n",
                          WiFi.localIP().toString().c_str());
            startMdns();
            return s_mode;
        }
        Serial.println("[wifi] STA timeout, falling back to AP");
        WiFi.disconnect(true);
    }

    // AP mode
    WiFi.mode(WIFI_AP);
    String apPass = apPassword();
    WiFi.softAP(s_apSsid.c_str(), apPass.c_str());
    s_mode = Mode::AP;
    s_ssid = s_apSsid;
    Serial.printf("[wifi] AP %s (pass %s) IP=%s\n", s_apSsid.c_str(),
                  apPass.c_str(), WiFi.softAPIP().toString().c_str());

    // captive portal: answer every DNS query with our own IP
    s_dns.start(DNS_PORT, "*", WiFi.softAPIP());
    startMdns();

    // token goes to the serial log (also shown on the AP page, see /setup-info)
    apiToken();
    return s_mode;
}

void tick() {
    if (s_mode == Mode::AP) s_dns.processNextRequest();

    // BOOT held 5 s: clear credentials -> reboot into AP (rescue channel)
    if (digitalRead(PIN_BOOT_BTN) == LOW) {
        if (s_bootPressedAt == 0) {
            s_bootPressedAt = millis();
        } else if (millis() - s_bootPressedAt >= BOOT_HOLD_MS) {
            Serial.println("[wifi] BOOT held 5s: clearing credentials, reboot to AP");
            prefs().begin("wifi", false);
            prefs().remove("ssid");
            prefs().remove("pass");
            prefs().end();
            delay(200);
            ESP.restart();
        }
    } else {
        s_bootPressedAt = 0;
    }
}

Mode   mode()        { return s_mode; }
String ip()          { return s_mode == Mode::STA ? WiFi.localIP().toString()
                                                  : WiFi.softAPIP().toString(); }
String ssid()        { return s_ssid; }
String apSsid()      { return s_apSsid; }
String macAddress()  { return s_mac; }
String mdnsHost()    { return s_mdnsHost; }
bool   isConnected() { return s_mode == Mode::STA && WiFi.status() == WL_CONNECTED; }

} // namespace wifiportal
