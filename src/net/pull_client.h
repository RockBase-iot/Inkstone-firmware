#pragma once

#include <Arduino.h>
#include "display/image_pipeline.h"

namespace pull {

static constexpr size_t MAX_URL_LENGTH = 256;

String url();
imagepipe::Profile profile();
bool configured();
bool save(const String& url, imagepipe::Profile profile);

// Reads a bounded HTTP response and converts it to the panel's fixed 2bpp codes.
// The caller owns an EPD_FRAME_BYTES PSRAM buffer and decides whether to refresh.
bool fetchFrame(uint8_t* frame);

} // namespace pull