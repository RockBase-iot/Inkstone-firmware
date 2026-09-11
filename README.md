# Inkstone-firmware

English | [中文文档](README_zh.md)

Inkstone-firmware is RockBase-iot's local firmware for its e-ink device family,
currently targeting the ESP32 platform. It runs fully standalone today and can
later be paired with the Inkstone cloud platform.

It currently implements a local image-push firmware for the NM-EPD e-ink
series: zero installation, fully local. Open the device page in a browser
(`http://inkstone-xxxxxx.local`), then pick an image → crop → preview →
push to the panel; AI agents and scripts can push directly through an
authenticated HTTP API.

- **Phase 1**: The device self-hosts an upload page (STA `inkstone-xxxxxx.local`
  or AP `192.168.4.1`). Browser-side Floyd-Steinberg 4-color quantization
  produces a raw 2bpp bitstream written straight to the panel. The page is
  bilingual (English / 中文, defaults to English).
- **Phase 2**: `/api/v1/*` Bearer-authenticated API with raw / JPEG / PNG
  channels, refresh-rate protection (60 s during debugging; 600 s recommended
  for production), and windowed deep sleep. An open mode is available for
  trusted home networks (`POST /api/v1/auth {"open":true}`, no token needed).
- **Multi-board framework**: all board-level constants live in
  `src/boards/*.h`; see [docs/PORTING.md](docs/PORTING.md) to add a board.
- **Multi-device coexistence**: the AP hotspot `Inkstone-XXXXXX` and mDNS
  `inkstone-xxxxxx.local` derive `xxxxxx` from the last three MAC bytes, so
  multiple devices on one network never collide.
- **Phase 3**: reserved for multi-color preview options (e.g. 6-color panels)
  and direct image push over the API for AI and script callers.

## Build

```bash
pio run -e nm-epd-420-4c        # compile + produce inkstone-nm-epd-420-4c.bin (merged 0x0 image)
pio run -e nm-epd-420-4c -t upload
```

Baseline: pioarduino 54.x (arduino-esp32 3.1 + IDF 5.3), aligned with the
[RockBase-iot/NM-EPD-420](https://github.com/RockBase-iot/NM-EPD-420) factory
firmware; depends on GxEPD2 1.6.8 / JPEGDEC / PNGdec.

## Layout

```
src/boards/      Board definitions (board.h dispatch, hardware ground truth)
src/display/     display_service (GxEPD2 direct write + async refresh) + image_pipeline (decode/quantize)
src/net/         wifi_portal (STA/AP/NVS/mDNS) + web_server + api_server
src/power/       sleep_manager (windowed deep sleep) + battery (ADC)
web/             uploader.html (single-file upload page, gzip-embedded at build time)
tools/           convert_image.py (golden reference) / push_image.py (AI call example)
scripts/         PlatformIO build hooks (page embedding + firmware merge)
docs/            API.md / SLEEP.md / PORTING.md / project plan
```

## Quick start

1. With no stored credentials the device opens the hotspot `Inkstone-XXXXXX`
   (XXXXXX = last three MAC bytes, password `12345678`). Open
   `http://192.168.4.1` in a browser to configure Wi-Fi.
2. After provisioning, visit `http://inkstone-xxxxxx.local` to push images
   (`xxxxxx` is the same MAC suffix in lowercase).
3. The API token is shown on the AP-mode page and in the serial log; see
   [docs/API.md](docs/API.md) and `tools/push_image.py` for usage, or enable
   open mode to skip the token entirely.
4. Sleep behavior is documented in [docs/SLEEP.md](docs/SLEEP.md).
