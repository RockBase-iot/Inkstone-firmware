// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// web_server — local browser push + provisioning endpoints
//
// Routes (plan section 5.1.3):
//   GET  /            gzip upload page (Content-Encoding: gzip)
//   POST /display     EPD_FRAME_BYTES application/octet-stream (30000B on
//                     NM-EPD-420-4C, 960000B on ESP32-C5-Spectra-E6), streamed
//                     into the display staging buffer -> 202 async refresh
//   GET  /status      {busy, last_update, battery_mv, ip, ssid, mode, sleep_in_s,
//                      fw, board, frame_bytes, mac, epd_w, epd_h, colors}
//   GET  /preview.raw current frame, EPD_FRAME_BYTES
//   GET  /setup-info  page bootstrap info (includes API token in AP mode)
//   POST /api/setup   provision {ssid, password} -> save to NVS and reboot
//   POST /api/ap-password  change AP password
//   POST /test-pattern     built-in color stripes (T1 acceptance)
// ============================================================================

#include <Arduino.h>
#include <WebServer.h>

namespace web {

WebServer& server();      // lets api_server mount /api/v1/* routes
void begin();
void handleClient();

} // namespace web
