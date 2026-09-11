// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#include "net/api_server.h"
#include "net/wifi_portal.h"
#include "boards/board.h"
#include "display/display_service.h"
#include "display/image_pipeline.h"
#include "power/sleep_manager.h"
#include "power/battery.h"

#include <Preferences.h>

namespace api {

static WebServer* s_srv = nullptr;

// Image upload staging (JPEG/PNG may exceed 30000B; 2MB cap)
static const size_t IMG_MAX = 2 * 1024 * 1024;
static uint8_t* s_imgBuf = nullptr;
static size_t   s_imgLen = 0;
static bool     s_imgOverflow = false;

// Open mode (no token): simplifies calls in home setups. Off by default
// (per plan section 5.2); toggled via POST /api/v1/auth, NVS-persisted.
static bool s_openApi = false;

bool openApi() { return s_openApi; }

static void loadAuthMode() {
    Preferences p;
    p.begin("auth", true);
    s_openApi = p.getBool("open", false);
    p.end();
    if (s_openApi) Serial.println("[api] OPEN mode: bearer token not required");
}

// ─── auth ───────────────────────────────────────────────────────────────────
static bool authed() {
    if (s_openApi) return true;    // open mode passes through
    if (!s_srv->hasHeader("Authorization")) return false;
    String h = s_srv->header("Authorization");
    if (!h.startsWith("Bearer ")) return false;
    return h.substring(7) == wifiportal::apiToken();
}

static bool requireAuth() {
    if (authed()) return true;
    s_srv->send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return false;
}

static void sendJson(int code, const String& body) {
    s_srv->send(code, "application/json", body);
}

// ─── POST /api/v1/display ───────────────────────────────────────────────────
static void handleDisplayUpload() {
    // RAW / multipart dual-channel dispatch (see web_server.cpp note)
    if (s_srv->header("Content-Type").startsWith("multipart/")) {
        HTTPUpload& up = s_srv->upload();
        if (up.status == UPLOAD_FILE_START) {
            s_imgLen = 0;
            s_imgOverflow = false;
        } else if (up.status == UPLOAD_FILE_WRITE) {
            if (s_imgLen + up.currentSize > IMG_MAX) {
                s_imgOverflow = true;
            } else if (s_imgBuf) {
                memcpy(s_imgBuf + s_imgLen, up.buf, up.currentSize);
                s_imgLen += up.currentSize;
            }
        }
        return;
    }
    HTTPRaw& raw = s_srv->raw();
    if (raw.status == RAW_START) {
        s_imgLen = 0;
        s_imgOverflow = false;
    } else if (raw.status == RAW_WRITE) {
        if (s_imgLen + raw.currentSize > IMG_MAX) {
            s_imgOverflow = true;
        } else if (s_imgBuf) {
            memcpy(s_imgBuf + s_imgLen, raw.buf, raw.currentSize);
            s_imgLen += raw.currentSize;
        }
    }
}

static void handleDisplay() {
    sleepman::notifyActivity();
    if (!requireAuth()) return;

    if (s_imgOverflow || s_imgLen == 0) {
        sendJson(400, "{\"error\":\"body empty or exceeds 2MB\"}");
        return;
    }
    if (display::isBusy()) {
        sendJson(409, "{\"error\":\"refresh in progress\",\"retry_after_s\":30}");
        return;
    }
    uint32_t wait = sleepman::nextAllowedUpdateInS();
    if (wait > 0) {
        s_srv->sendHeader("Retry-After", String(wait));
        sendJson(429, "{\"error\":\"min refresh interval\",\"retry_after_s\":"
                      + String(wait) + "}");
        return;
    }

    String ct = s_srv->header("Content-Type");
    bool dither = s_srv->arg("dither") != "off";   // FS dither on by default for JPEG/PNG
    uint8_t* frame = (uint8_t*)ps_malloc(EPD_FRAME_BYTES);
    if (!frame) { sendJson(500, "{\"error\":\"oom\"}"); return; }

    bool ok = false;
    if (ct.startsWith("application/octet-stream")) {
        if (s_imgLen != EPD_FRAME_BYTES) {
            sendJson(400, "{\"error\":\"raw frame must be exactly 30000 bytes\"}");
            free(frame);
            return;
        }
        memcpy(frame, s_imgBuf, EPD_FRAME_BYTES);
        ok = true;
    } else if (ct.startsWith("image/jpeg")) {
        ok = imagepipe::convertJpeg(s_imgBuf, s_imgLen, frame, dither);
    } else if (ct.startsWith("image/png")) {
        ok = imagepipe::convertPng(s_imgBuf, s_imgLen, frame, dither);
    } else {
        sendJson(415, "{\"error\":\"supported: application/octet-stream, image/jpeg, image/png\"}");
        free(frame);
        return;
    }

    if (!ok) {
        free(frame);
        sendJson(422, "{\"error\":\"decode/quantize failed\"}");
        return;
    }
    if (!display::submitFrame(frame, EPD_FRAME_BYTES)) {
        free(frame);
        sendJson(409, "{\"error\":\"refresh in progress\",\"retry_after_s\":30}");
        return;
    }
    free(frame);
    sendJson(202, "{\"accepted\":true,\"refresh_eta_s\":30}");
}

// ─── GET /api/v1/status ─────────────────────────────────────────────────────
static void handleStatus() {
    sleepman::notifyActivity();
    if (!requireAuth()) return;

    const sleepman::Params& p = sleepman::params();
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"busy\":%s,\"last_update\":%u,\"battery_mv\":%u,"
        "\"ip\":\"%s\",\"ssid\":\"%s\",\"mode\":\"%s\",\"sleep_in_s\":%u,"
        "\"min_interval_s\":%u,\"next_allowed_update\":%u,"
        "\"stay_awake\":%s,\"boot_count\":%u}",
        display::isBusy() ? "true" : "false",
        display::lastRefreshEpoch(),
        battery::readMv(),
        wifiportal::ip().c_str(),
        wifiportal::ssid().c_str(),
        wifiportal::mode() == wifiportal::Mode::STA ? "sta" : "ap",
        sleepman::sleepInS(),
        p.minRefreshIntervalS,
        display::lastRefreshEpoch() == 0
            ? 0u
            : display::lastRefreshEpoch() + sleepman::nextAllowedUpdateInS(),
        sleepman::stayAwake() ? "true" : "false",
        sleepman::bootCount());
    String j = String(buf);
    j.remove(j.length() - 1);   // drop the trailing }
    j += ",\"auth_open\":";
    j += s_openApi ? "true" : "false";
    j += "}";
    sendJson(200, j);
}

// ─── POST /api/v1/sleep ─────────────────────────────────────────────────────
static void handleSleep() {
    sleepman::notifyActivity();
    if (!requireAuth()) return;

    String body = s_srv->arg("plain");
    if (body.indexOf("\"stay_awake\"") >= 0) {
        bool on = body.indexOf("true") > body.indexOf("\"stay_awake\"");
        sleepman::setStayAwake(on);
        sendJson(200, String("{\"stay_awake\":") + (on ? "true}" : "false}"));
        return;
    }
    int idx = body.indexOf("\"delay_s\"");
    uint32_t delayS = 0;
    if (idx >= 0) {
        int colon = body.indexOf(':', idx);
        if (colon > 0) delayS = (uint32_t)body.substring(colon + 1).toInt();
    }
    sleepman::requestSleep(delayS);
    sendJson(202, "{\"sleep_in_s\":" + String(delayS) + "}");
}

// ─── POST /api/v1/token ─────────────────────────────────────────────────────
static void handleTokenRotate() {
    sleepman::notifyActivity();
    if (!requireAuth()) return;
    wifiportal::rotateApiToken();
    sendJson(200, "{\"token\":\"" + wifiportal::apiToken() + "\"}");
}

// ─── POST /api/v1/auth — open mode toggle ───────────────────────────────────
// In AP mode anyone on the hotspot can already see the token, so toggling is
// allowed directly; in STA mode a valid token is required (prevents others on
// the LAN from silently disabling auth).
static void handleAuthMode() {
    sleepman::notifyActivity();
    if (wifiportal::mode() == wifiportal::Mode::STA && !requireAuth()) return;

    String body = s_srv->arg("plain");
    bool open = body.indexOf("\"open\"") >= 0 &&
                body.indexOf("true") > body.indexOf("\"open\"");
    s_openApi = open;
    Preferences p;
    p.begin("auth", false);
    p.putBool("open", open);
    p.end();
    Serial.printf("[api] open mode %s\n", open ? "ON (no token)" : "OFF (token required)");
    sendJson(200, String("{\"auth_open\":") + (open ? "true}" : "false}"));
}

void registerRoutes(WebServer& server) {
    s_srv = &server;
    loadAuthMode();
    s_imgBuf = (uint8_t*)ps_malloc(IMG_MAX);
    if (!s_imgBuf) Serial.println("[api] image buffer alloc failed");

    server.on("/api/v1/display", HTTP_POST, handleDisplay, handleDisplayUpload);
    server.on("/api/v1/status",  HTTP_GET,  handleStatus);
    server.on("/api/v1/sleep",   HTTP_POST, handleSleep);
    server.on("/api/v1/token",   HTTP_POST, handleTokenRotate);
    server.on("/api/v1/auth",    HTTP_POST, handleAuthMode);
}

} // namespace api
