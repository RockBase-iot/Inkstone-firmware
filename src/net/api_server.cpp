// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#include "net/api_server.h"
#include "net/wifi_portal.h"
#include "net/pull_client.h"
#include "boards/board.h"
#include "display/display_service.h"
#include "display/image_pipeline.h"
#include "power/sleep_manager.h"
#include "power/battery.h"

#include <Preferences.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include <limits.h>

namespace api {

static WebServer* s_srv = nullptr;

// Image upload staging. 4-color boards accept JPEG/PNG up to 2MB and decode
// them on-device. On the Spectra E6 (frame = 960KB, PSRAM = 4MB) the cap is
// the frame size itself: the intended channel is the web page, which
// quantizes in the browser and pushes the packed 4bpp frame. On-device
// decode still works for small images (the RGB565 source must fit in the
// remaining PSRAM).
#if EPD_COLORS == 6
static const size_t IMG_MAX = EPD_FRAME_BYTES;
#else
static const size_t IMG_MAX = 2 * 1024 * 1024;
#endif
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

enum class JsonField : int8_t { INVALID = -1, ABSENT = 0, PRESENT = 1 };

static JsonField jsonUInt(const String& body, const char* name, uint32_t* value) {
    int key = body.indexOf(String("\"") + name + "\"");
    if (key < 0) return JsonField::ABSENT;
    int colon = body.indexOf(':', key);
    if (colon < 0) return JsonField::INVALID;
    const char* p = body.c_str() + colon + 1;
    while (isspace((unsigned char)*p)) ++p;
    if (*p < '0' || *p > '9') return JsonField::INVALID;
    uint64_t parsed = 0;
    while (*p >= '0' && *p <= '9') {
        parsed = parsed * 10 + (uint64_t)(*p++ - '0');
        if (parsed > UINT32_MAX) return JsonField::INVALID;
    }
    while (isspace((unsigned char)*p)) ++p;
    if (*p != ',' && *p != '}' && *p != '\0') return JsonField::INVALID;
    *value = (uint32_t)parsed;
    return JsonField::PRESENT;
}

static JsonField jsonBool(const String& body, const char* name, bool* value) {
    int key = body.indexOf(String("\"") + name + "\"");
    if (key < 0) return JsonField::ABSENT;
    int colon = body.indexOf(':', key);
    if (colon < 0) return JsonField::INVALID;
    const char* p = body.c_str() + colon + 1;
    while (isspace((unsigned char)*p)) ++p;
    if (strncmp(p, "true", 4) == 0) {
        *value = true;
        p += 4;
    } else if (strncmp(p, "false", 5) == 0) {
        *value = false;
        p += 5;
    } else {
        return JsonField::INVALID;
    }
    while (isspace((unsigned char)*p)) ++p;
    return (*p == ',' || *p == '}' || *p == '\0') ? JsonField::PRESENT
                                                    : JsonField::INVALID;
}

static String sleepParamsJson(const sleepman::Params& p) {
    char buf[224];
    snprintf(buf, sizeof(buf),
             "{\"idle_timeout_s\":%u,\"post_refresh_grace_s\":%u,"
             "\"min_refresh_interval_s\":%u,\"sched_wake_s\":%u,"
             "\"batt_low_mv\":%u}",
             (unsigned)p.idleTimeoutS, (unsigned)p.postRefreshGraceS,
             (unsigned)p.minRefreshIntervalS, (unsigned)p.schedWakeS,
             (unsigned)p.battLowMv);
    return String(buf);
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

    // Quantization profile. `profile` wins over the legacy `dither` flag, which
    // is kept so old clients and the ?dither=off spelling keep working.
    imagepipe::Profile prof = imagepipe::Profile::IPP_DEFAULT;
    String pArg = s_srv->arg("profile");
    if (pArg.length() > 0) {
        if (!imagepipe::parseProfile(pArg.c_str(), &prof)) {
            sendJson(400, "{\"error\":\"profile must be default|retro|none\"}");
            return;
        }
    } else if (s_srv->arg("dither") == "off") {
        prof = imagepipe::Profile::IPP_NONE;
    }
#if EPD_COLORS == 6
    if (prof == imagepipe::Profile::IPP_RETRO) {
        sendJson(400, "{\"error\":\"retro is a 4-color profile; on Spectra E6 use default|none\"}");
        return;
    }
#endif

    // Decode/quantize directly into the display staging buffer: avoids a
    // transient EPD_FRAME_BYTES allocation (960KB on E6) next to s_imgBuf.
    uint8_t* frame = display::stagingBuffer();
    if (!frame) {
        sendJson(409, "{\"error\":\"refresh in progress\",\"retry_after_s\":30}");
        return;
    }

    bool ok = false;
    if (ct.startsWith("application/octet-stream")) {
        // Already-quantized raw frame: `profile` is meaningless here, the
        // client (e.g. web/uploader.html) did the quantization itself.
        if (s_imgLen != EPD_FRAME_BYTES) {
            sendJson(400, String("{\"error\":\"raw frame must be exactly ")
                          + String((unsigned)EPD_FRAME_BYTES) + " bytes\"}");
            return;
        }
        memcpy(frame, s_imgBuf, EPD_FRAME_BYTES);
        ok = true;
    } else if (ct.startsWith("image/jpeg")) {
        ok = imagepipe::convertJpeg(s_imgBuf, s_imgLen, frame, prof);
    } else if (ct.startsWith("image/png")) {
        ok = imagepipe::convertPng(s_imgBuf, s_imgLen, frame, prof);
    } else {
        sendJson(415, "{\"error\":\"supported: application/octet-stream, image/jpeg, image/png\"}");
        return;
    }

    if (!ok) {
        if (prof == imagepipe::Profile::IPP_RETRO) {
            // RETRO holds work+mask+err at once (~1.9 MB); PSRAM exhaustion is
            // the realistic failure mode, so name it.
            sendJson(422, "{\"error\":\"decode/quantize failed\","
                          "\"hint\":\"retro requires more PSRAM\"}");
        } else {
            sendJson(422, "{\"error\":\"decode/quantize failed\","
                          "\"hint\":\"on Spectra E6 large sources may not fit; "
                          "use the web page to quantize and push a packed frame\"}");
        }
        return;
    }
    if (!display::submitStagedFrame()) {
        sendJson(409, "{\"error\":\"refresh in progress\",\"retry_after_s\":30}");
        return;
    }
    sendJson(202, "{\"accepted\":true,\"refresh_eta_s\":30}");
}

// ─── GET /api/v1/status ─────────────────────────────────────────────────────
static void handleStatus() {
    sleepman::notifyActivity();
    if (!requireAuth()) return;

    const sleepman::Params& p = sleepman::params();
    char buf[640];
    snprintf(buf, sizeof(buf),
        "{\"busy\":%s,\"last_update\":%u,\"battery_mv\":%u,"
        "\"ip\":\"%s\",\"ssid\":\"%s\",\"mode\":\"%s\",\"sleep_in_s\":%u,"
        "\"min_interval_s\":%u,\"idle_timeout_s\":%u,"
        "\"post_refresh_grace_s\":%u,\"sched_wake_s\":%u,\"batt_low_mv\":%u,"
        "\"next_allowed_update\":%u,"
        "\"stay_awake\":%s,\"boot_count\":%u,"
        "\"profiles\":[\"default\",\"retro\",\"none\"]}",
        display::isBusy() ? "true" : "false",
        (unsigned)display::lastRefreshEpoch(),
        (unsigned)battery::readMv(),
        wifiportal::ip().c_str(),
        wifiportal::ssid().c_str(),
        wifiportal::mode() == wifiportal::Mode::STA ? "sta" : "ap",
        (unsigned)sleepman::sleepInS(),
        (unsigned)p.minRefreshIntervalS,
        (unsigned)p.idleTimeoutS,
        (unsigned)p.postRefreshGraceS,
        (unsigned)p.schedWakeS,
        (unsigned)p.battLowMv,
        display::lastRefreshEpoch() == 0
            ? 0u
            : (unsigned)(display::lastRefreshEpoch() + sleepman::nextAllowedUpdateInS()),
        sleepman::stayAwake() ? "true" : "false",
        (unsigned)sleepman::bootCount());
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
    bool stayAwake = false;
    JsonField stayField = jsonBool(body, "stay_awake", &stayAwake);
    if (stayField == JsonField::INVALID) {
        sendJson(400, "{\"error\":\"stay_awake must be true or false\"}");
        return;
    }

    sleepman::Params updated = sleepman::params();
    const char* names[] = {"idle_timeout_s", "post_refresh_grace_s",
                           "min_refresh_interval_s", "sched_wake_s", "batt_low_mv"};
    uint32_t* values[] = {&updated.idleTimeoutS, &updated.postRefreshGraceS,
                          &updated.minRefreshIntervalS, &updated.schedWakeS,
                          &updated.battLowMv};
    bool hasParams = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        JsonField field = jsonUInt(body, names[i], values[i]);
        if (field == JsonField::INVALID) {
            sendJson(400, String("{\"error\":\"") + names[i]
                         + " must be an unsigned 32-bit integer\"}");
            return;
        }
        hasParams = hasParams || field == JsonField::PRESENT;
    }

    uint32_t delayS = 0;
    JsonField delayField = jsonUInt(body, "delay_s", &delayS);
    if (delayField == JsonField::INVALID || delayS > INT32_MAX / 1000UL) {
        sendJson(400, "{\"error\":\"delay_s must be 0..2147483\"}");
        return;
    }

    uint8_t commands = (stayField == JsonField::PRESENT) + hasParams +
                       (delayField == JsonField::PRESENT);
    if (commands != 1) {
        sendJson(400, "{\"error\":\"provide exactly one sleep command\"}");
        return;
    }
    if (stayField == JsonField::PRESENT) {
        bool on = stayAwake;
        sleepman::setStayAwake(on);
        sendJson(200, String("{\"stay_awake\":") + (on ? "true}" : "false}"));
        return;
    }
    if (hasParams) {
        sleepman::setParams(updated);
        sendJson(200, sleepParamsJson(sleepman::params()));
        return;
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

static void handlePullConfig() {
    sleepman::notifyActivity();
    if (!requireAuth()) return;

    if (s_srv->method() == HTTP_GET) {
        JsonDocument result;
        result["url"] = pull::url();
        result["interval_s"] = sleepman::params().schedWakeS;
        imagepipe::Profile p = pull::profile();
        result["profile"] = p == imagepipe::Profile::IPP_RETRO ? "retro"
                            : p == imagepipe::Profile::IPP_NONE ? "none" : "default";
        String json;
        serializeJson(result, json);
        sendJson(200, json);
        return;
    }

    String body = s_srv->arg("plain");
    JsonDocument input;
    if (body.length() > 1024 || deserializeJson(input, body) ||
        !input.is<JsonObject>() || !input["url"].is<const char*>()) {
        sendJson(400, "{\"error\":\"expected JSON object with url string\"}");
        return;
    }
    String source = input["url"].as<String>();
    imagepipe::Profile selected = pull::profile();
    if (!input["profile"].isNull() &&
        (!input["profile"].is<const char*>() ||
         !imagepipe::parseProfile(input["profile"].as<const char*>(), &selected))) {
        sendJson(400, "{\"error\":\"profile must be default|retro|none\"}");
        return;
    }

    sleepman::Params updated = sleepman::params();
    if (!source.isEmpty() && updated.schedWakeS == 0) updated.schedWakeS = 3600;
    if (!input["interval_s"].isNull()) {
        if (!input["interval_s"].is<uint32_t>()) {
            sendJson(400, "{\"error\":\"interval_s must be a positive integer\"}");
            return;
        }
        updated.schedWakeS = input["interval_s"].as<uint32_t>();
    }
    if (source.isEmpty()) updated.schedWakeS = 0;
    if ((!source.isEmpty() &&
         (updated.schedWakeS < updated.minRefreshIntervalS ||
          updated.schedWakeS > 604800)) ||
        !pull::save(source, selected)) {
        sendJson(400, "{\"error\":\"invalid URL or interval (HTTP only, 256 chars max)\"}");
        return;
    }
    sleepman::setParams(updated);
    sendJson(200, "{\"saved\":true}");
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
    server.on("/api/v1/pull",    HTTP_GET,  handlePullConfig);
    server.on("/api/v1/pull",    HTTP_POST, handlePullConfig);
}

} // namespace api
