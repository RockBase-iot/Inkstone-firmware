// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#pragma once
// ============================================================================
// api_server — AI-readable/writable API (prefix /api/v1, Bearer auth)
//
//   POST /api/v1/display   raw 2bpp direct write | JPEG/PNG on-device decode
//   GET  /api/v1/status    same as /status + min_interval_s / next_allowed_update
//   POST /api/v1/sleep     {delay_s:N} | {stay_awake:true|false}
//   POST /api/v1/token     rotate token (requires old token)
//   POST /api/v1/auth      open mode {open:true|false} (skip Bearer; for
//                          trusted home networks, NVS-persisted)
//
// Error semantics: 401 missing/wrong token; 400 bad length/format; 409 busy
// (body carries retry_after_s); 429 min refresh interval not met (carries
// Retry-After header). Token checks stay enforced in open AP mode unless
// open mode is explicitly enabled.
// ============================================================================

#include <Arduino.h>
#include <WebServer.h>

namespace api {

void registerRoutes(WebServer& server);
bool openApi();   // whether open mode (no token) is enabled

} // namespace api
