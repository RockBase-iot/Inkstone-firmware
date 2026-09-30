// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#include "net/web_server.h"
#include "net/api_server.h"
#include "net/wifi_portal.h"
#include "net/embedded_page.h"
#include "boards/board.h"
#include "display/display_service.h"
#include "power/sleep_manager.h"
#include "power/battery.h"

namespace web {

#ifndef FW_VERSION
#define FW_VERSION "0.1.0"
#endif

static WebServer s_server(80);

// ─── Upload staging (streams straight into the display staging buffer) ──────
// The display service owns the EPD_FRAME_BYTES staging buffer (s_pending);
// writing the upload into it directly avoids a second 960KB PSRAM buffer on
// the Spectra E6. stagingBuffer() returns nullptr while a refresh is running,
// which we surface as a 409 at the end of the upload.
static uint8_t* s_uploadTarget = nullptr;
static size_t   s_uploadLen = 0;
static bool     s_uploadOverflow = false;

static void sendJson(int code, const String& body) {
    s_server.sendHeader("Access-Control-Allow-Origin", "*");
    s_server.send(code, "application/json", body);
}

static String statusJson() {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"busy\":%s,\"last_update\":%u,\"battery_mv\":%u,"
        "\"ip\":\"%s\",\"ssid\":\"%s\",\"mode\":\"%s\",\"sleep_in_s\":%u,"
        "\"fw\":\"%s\",\"board\":\"%s\",\"frame_bytes\":%u,\"mac\":\"%s\","
        "\"epd_w\":%u,\"epd_h\":%u,\"colors\":%u}",
        display::isBusy() ? "true" : "false",
        display::lastRefreshEpoch(),
        battery::readMv(),
        wifiportal::ip().c_str(),
        wifiportal::ssid().c_str(),
        wifiportal::mode() == wifiportal::Mode::STA ? "sta" : "ap",
        sleepman::sleepInS(),
        FW_VERSION, BOARD_NAME, (unsigned)EPD_FRAME_BYTES,
        wifiportal::macAddress().c_str(),
        (unsigned)EPD_WIDTH, (unsigned)EPD_HEIGHT, (unsigned)EPD_COLORS);
    return String(buf);
}

// ─── Phase 1 routes ─────────────────────────────────────────────────────────
static void handleRoot() {
    sleepman::notifyActivity();
    s_server.sendHeader("Content-Encoding", "gzip");
    s_server.sendHeader("Cache-Control", "no-cache");
    s_server.send_P(200, "text/html",
                    (const char*)EMBEDDED_PAGE_GZIP, EMBEDDED_PAGE_GZIP_LEN);
}

static void handleStatus() {
    sleepman::notifyActivity();
    sendJson(200, statusJson());
}

static void handleSetupInfo() {
    sleepman::notifyActivity();
    String j = "{\"mode\":\"";
    j += wifiportal::mode() == wifiportal::Mode::STA ? "sta" : "ap";
    j += "\",\"ap_ssid\":\"" + wifiportal::apSsid() + "\"";
    j += ",\"mdns\":\"" + wifiportal::mdnsHost() + "\"";
    j += ",\"mac\":\"" + wifiportal::macAddress() + "\"";
    // token is exposed to same-hotspot visitors only in AP mode (plan section 5.2)
    if (wifiportal::mode() == wifiportal::Mode::AP) {
        j += ",\"token\":\"" + wifiportal::apiToken() + "\"";
    }
    j += ",\"stay_awake\":";
    j += sleepman::stayAwake() ? "true" : "false";
    j += ",\"auth_open\":";
    j += api::openApi() ? "true" : "false";
    j += "}";
    sendJson(200, j);
}

// POST /display streaming receive
// NOTE: in arduino-esp32 3.x, application/octet-stream arrives via the RAW
// channel (HTTPRaw: RAW_START/RAW_WRITE/RAW_END); only multipart uses the
// upload() channel. Both share this handler, so we must dispatch on
// Content-Type — calling upload() in RAW mode dereferences a null pointer.
static void handleDisplayUpload() {
    if (s_server.header("Content-Type").startsWith("multipart/")) {
        HTTPUpload& up = s_server.upload();
        if (up.status == UPLOAD_FILE_START) {
            s_uploadTarget = display::stagingBuffer();
            s_uploadLen = 0;
            s_uploadOverflow = (s_uploadTarget == nullptr);
        } else if (up.status == UPLOAD_FILE_WRITE) {
            if (s_uploadLen + up.currentSize > EPD_FRAME_BYTES) {
                s_uploadOverflow = true;
            } else if (s_uploadTarget) {
                memcpy(s_uploadTarget + s_uploadLen, up.buf, up.currentSize);
                s_uploadLen += up.currentSize;
            }
        }
        return;
    }
    HTTPRaw& raw = s_server.raw();
    if (raw.status == RAW_START) {
        s_uploadTarget = display::stagingBuffer();
        s_uploadLen = 0;
        s_uploadOverflow = (s_uploadTarget == nullptr);
    } else if (raw.status == RAW_WRITE) {
        if (s_uploadLen + raw.currentSize > EPD_FRAME_BYTES) {
            s_uploadOverflow = true;
        } else if (s_uploadTarget) {
            memcpy(s_uploadTarget + s_uploadLen, raw.buf, raw.currentSize);
            s_uploadLen += raw.currentSize;
        }
    }
}

static void handleDisplayPost() {
    sleepman::notifyActivity();

    if (s_uploadOverflow || s_uploadLen != EPD_FRAME_BYTES) {
        sendJson(400, String("{\"error\":\"frame must be exactly ")
                      + String((unsigned)EPD_FRAME_BYTES) + " bytes\"}");
        return;
    }
    if (display::isBusy()) {
        sendJson(409, "{\"error\":\"refresh in progress\",\"retry_after_s\":30}");
        return;
    }
    uint32_t wait = sleepman::nextAllowedUpdateInS();
    if (wait > 0) {
        s_server.sendHeader("Retry-After", String(wait));
        sendJson(429, "{\"error\":\"min refresh interval\",\"retry_after_s\":"
                      + String(wait) + "}");
        return;
    }
    if (!display::submitStagedFrame()) {
        sendJson(409, "{\"error\":\"refresh in progress\",\"retry_after_s\":30}");
        return;
    }
    sendJson(202, "{\"accepted\":true,\"refresh_eta_s\":30}");
}

static void handlePreviewRaw() {
    sleepman::notifyActivity();
    const uint8_t* f = display::currentFrame();
    if (!f) { sendJson(503, "{\"error\":\"no frame\"}"); return; }
    s_server.setContentLength(EPD_FRAME_BYTES);
    s_server.send(200, "application/octet-stream", "");
    s_server.sendContent_P((const char*)f, EPD_FRAME_BYTES);
}

static void handleSetup() {   // POST /api/setup
    sleepman::notifyActivity();
    String ssid = s_server.arg("ssid");
    String pass = s_server.arg("pass");
    if (ssid.length() == 0) { sendJson(400, "{\"error\":\"ssid required\"}"); return; }
    sendJson(200, "{\"ok\":true,\"note\":\"rebooting into STA\"}");
    wifiportal::saveCredentialsAndReboot(ssid, pass);
}

static void handleApPassword() {   // POST /api/ap-password
    sleepman::notifyActivity();
    String pass = s_server.arg("pass");
    if (pass.length() < 8) {
        sendJson(400, "{\"error\":\"password must be >= 8 chars\"}");
        return;
    }
    wifiportal::setApPassword(pass);
    sendJson(200, "{\"ok\":true,\"note\":\"takes effect after reboot\"}");
}

static void handleTestPattern() {
    sleepman::notifyActivity();
    if (display::isBusy()) {
        sendJson(409, "{\"error\":\"refresh in progress\",\"retry_after_s\":30}");
        return;
    }
    display::showTestPattern();
    sendJson(202, "{\"accepted\":true}");
}

static void handleNotFound() {
    // captive portal: unknown paths in AP mode get the home page
    if (wifiportal::mode() == wifiportal::Mode::AP) { handleRoot(); return; }
    sendJson(404, "{\"error\":\"not found\"}");
}

WebServer& server() { return s_server; }

void begin() {
    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/status", HTTP_GET, handleStatus);
    s_server.on("/setup-info", HTTP_GET, handleSetupInfo);
    s_server.on("/display", HTTP_POST, handleDisplayPost, handleDisplayUpload);
    s_server.on("/preview.raw", HTTP_GET, handlePreviewRaw);
    s_server.on("/api/setup", HTTP_POST, handleSetup);
    s_server.on("/api/ap-password", HTTP_POST, handleApPassword);
    s_server.on("/test-pattern", HTTP_POST, handleTestPattern);
    s_server.onNotFound(handleNotFound);

    api::registerRoutes(s_server);   // Phase 2 /api/v1/*

    s_server.begin();
    Serial.printf("[web] HTTP server on :80 (%s mode, %s)\n",
                  wifiportal::mode() == wifiportal::Mode::STA ? "STA" : "AP",
                  wifiportal::ip().c_str());
}

void handleClient() { s_server.handleClient(); }

} // namespace web
