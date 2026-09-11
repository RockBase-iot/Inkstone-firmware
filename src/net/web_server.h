// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// web_server — local browser push + provisioning endpoints
//
// Routes (plan section 5.1.3):
//   GET  /            gzip upload page (Content-Encoding: gzip)
//   POST /display     30000B application/octet-stream, streamed -> 202 async refresh
//   GET  /status      {busy, last_update, battery_mv, ip, ssid, mode, sleep_in_s, fw, mac}
//   GET  /preview.raw current frame, 30000B
//   GET  /setup-info  page bootstrap info (includes API token in AP mode)
//   POST /api/setup   provision {ssid, password} -> save to NVS and reboot
//   POST /api/ap-password  change AP password
//   POST /test-pattern     built-in 4-color stripes (T1 acceptance)
// ============================================================================

#include <Arduino.h>
#include <WebServer.h>

namespace web {

WebServer& server();      // lets api_server mount /api/v1/* routes
void begin();
void handleClient();

} // namespace web
