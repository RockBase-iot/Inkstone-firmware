#include "net/pull_client.h"
#include "boards/board.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>

namespace pull {

static constexpr size_t MAX_IMAGE_BYTES = 2 * 1024 * 1024;
static constexpr uint32_t FETCH_DEADLINE_MS = 30000;

String url() {
    Preferences prefs;
    prefs.begin("pull", true);
    String value = prefs.getString("url", "");
    prefs.end();
    return value;
}

imagepipe::Profile profile() {
    Preferences prefs;
    prefs.begin("pull", true);
    String value = prefs.getString("profile", "default");
    prefs.end();
    imagepipe::Profile result;
    return imagepipe::parseProfile(value.c_str(), &result)
               ? result : imagepipe::Profile::IPP_DEFAULT;
}

bool configured() { return url().length() != 0; }

bool save(const String& source, imagepipe::Profile selected) {
    if (source.length() > MAX_URL_LENGTH ||
        (source.length() && (!source.startsWith("http://") ||
                             source.length() <= 7 || source[7] == '/' ||
                             source[7] == '?' || source.indexOf(' ') >= 0 ||
                             source.indexOf('\r') >= 0 || source.indexOf('\n') >= 0 ||
                             source.indexOf('#') >= 0 ||
                             source.indexOf('@') >= 0))) return false;

    const char* token = selected == imagepipe::Profile::IPP_RETRO ? "retro"
                        : selected == imagepipe::Profile::IPP_NONE ? "none" : "default";
    Preferences prefs;
    if (!prefs.begin("pull", false)) return false;
    bool ok = prefs.putString("url", source) == source.length() &&
              prefs.putString("profile", token) == strlen(token);
    prefs.end();
    return ok;
}

bool fetchFrame(uint8_t* frame) {
    String source = url();
    if (!frame || source.isEmpty() || WiFi.status() != WL_CONNECTED) return false;

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, source)) return false;
    const char* headers[] = {"Content-Type"};
    http.collectHeaders(headers, 1);

    bool ok = false;
    uint8_t* body = nullptr;
    do {
        int status = http.GET();
        if (status != HTTP_CODE_OK) {
            Serial.printf("[pull] HTTP status %d\n", status);
            break;
        }
        int length = http.getSize();
        String contentType = http.header("Content-Type");
        bool raw = contentType.startsWith("application/octet-stream");
        bool jpeg = contentType.startsWith("image/jpeg");
        bool png = contentType.startsWith("image/png");
        if (length <= 0 || length > (int)MAX_IMAGE_BYTES ||
            (!raw && !jpeg && !png) || (raw && length != EPD_FRAME_BYTES)) {
            Serial.println("[pull] invalid length or Content-Type");
            break;
        }
        body = (uint8_t*)ps_malloc(length);
        if (!body) {
            Serial.println("[pull] image buffer alloc failed");
            break;
        }
        WiFiClient* stream = http.getStreamPtr();
        size_t received = 0;
        uint32_t started = millis();
        while (received < (size_t)length &&
               millis() - started < FETCH_DEADLINE_MS) {
            size_t chunk = min((size_t)1024, (size_t)length - received);
            size_t count = stream->readBytes(body + received, chunk);
            if (count == 0) break;
            received += count;
        }
        if (received != (size_t)length) {
            Serial.printf("[pull] truncated image: %u/%d bytes\n",
                          (unsigned)received, length);
            break;
        }
        if (raw) {
            memcpy(frame, body, EPD_FRAME_BYTES);
            ok = true;
        } else if (jpeg) {
            ok = imagepipe::convertJpeg(body, received, frame, profile());
        } else {
            ok = imagepipe::convertPng(body, received, frame, profile());
        }
    } while (false);

    free(body);
    http.end();
    return ok;
}

} // namespace pull