# Inkstone-firmware

English | [中文文档](README_zh.md)

Inkstone-firmware is RockBase-iot's local firmware for its e-ink device family,
currently targeting the ESP32 platform. It runs fully standalone today and can
later be paired with the Inkstone cloud platform.

It currently implements a local image-push firmware for the NM-EPD e-ink
series: zero installation, fully local. Open the device page in a browser
(`http://inkstone.local`), then pick an image → crop → preview →
push to the panel; AI agents and scripts can push directly through an
authenticated HTTP API.

- **Phase 1**: The device self-hosts an upload page (STA `inkstone.local`
  or AP `192.168.4.1`). The page renders the image under **two quantization
  algorithms side by side** — `default` (luma-weighted Floyd–Steinberg) and
  `retro` (OKLab + panel-calibrated warm palette + hue-aware masking) — and the
  user picks one to push. The chosen frame is quantized in the browser and sent
  as a raw 2bpp bitstream written straight to the panel. The page is bilingual
  (English / 中文, defaults to English).
- **Phase 2**: `/api/v1/*` Bearer-authenticated API with raw / JPEG / PNG
  channels, `?profile=default|retro|none` selection, refresh-rate protection
  (60 s during debugging; 600 s recommended for production), and windowed deep
  sleep. An open mode is available for trusted home networks
  (`POST /api/v1/auth {"open":true}`, no token needed).
- **Multi-board framework**: all board-level constants live in
  `src/boards/*.h`; see [docs/PORTING.md](docs/PORTING.md) to add a board.
- **Easy access**: after Wi-Fi provisioning, open `inkstone.local` without
  looking up the device IP. The AP hotspot remains `Inkstone-XXXXXX` to
  distinguish devices during setup. Only one device using this hostname should
  be active on the same LAN.
- **Phase 3**: reserved for multi-color preview options (e.g. 6-color panels)
  and direct image push over the API for AI and script callers.

### Two quantization profiles

Neither is strictly better — they optimise different things, which is exactly
why the page shows both.

| | `default` | `retro` |
|---|---|---|
| Goal | fidelity | stylised, film-like grain |
| Cool regions (sky, water) | dithers into red speckle | masked to black + white only |
| Highlights / shadows | can crush to pure white / black | lifted and compressed |
| Good for | warm-toned subjects, general use | images with large cool areas |

Both are **byte-identical across the firmware, the browser and
`tools/convert_image.py`** — the same input frame must come out the same on all
three. That property is enforced by `scripts/verify_parity.sh`; the float
contract that makes it hold is documented in [docs/RETRO.md](docs/RETRO.md).

> The upload page keeps **two separate implementations** on purpose: `retro`
> calls `web/retro_core.js`, while `default` stays an inline integer
> Floyd–Steinberg — `retro_core.js` does not implement `default`, and the
> inline one is already the byte-exact match for the firmware.

## Build

```bash
pio run -e nm-epd-420-4c        # compile + produce inkstone-nm-epd-420-4c.bin (merged 0x0 image)
pio run -e nm-epd-420-4c -t upload
```

Baseline: pioarduino 54.x (arduino-esp32 3.1 + IDF 5.3), aligned with the
[RockBase-iot/NM-EPD-420](https://github.com/RockBase-iot/NM-EPD-420) factory
firmware; depends on GxEPD2 1.6.8 / JPEGDEC / PNGdec.

Verify the three-port byte-parity claim (runs everything that does not need a
board):

```bash
bash scripts/verify_parity.sh
```

## Layout

```
src/boards/      Board definitions (board.h dispatch, hardware ground truth)
src/display/     display_service (GxEPD2 direct write + async refresh)
                 image_pipeline (profile dispatch) + retro_quantize (RETRO)
src/net/         wifi_portal (STA/AP/NVS/mDNS) + web_server + api_server
src/power/       sleep_manager (windowed deep sleep) + battery (ADC)
web/             uploader.html (upload page) + retro_core.js (RETRO quantizer,
                 inlined into the page by the build)
tools/           convert_image.py (golden reference) / push_image.py (AI call example)
                 hosttest/ (firmware-on-host parity) / check_js_parity.js
scripts/         PlatformIO build hooks (page embedding + firmware merge)
                 verify_parity.sh (one-shot parity check)
docs/            RETRO.md (algorithm + float contract) / API.md / SLEEP.md /
                 PORTING.md / project plan
```

## Quick start

1. With no stored credentials the device opens the hotspot `Inkstone-XXXXXX`
   (XXXXXX = last three MAC bytes, password `12345678`). Open
   `http://192.168.4.1` in a browser to configure Wi-Fi.
2. After provisioning, visit `http://inkstone.local` to push images.
3. The API token is shown on the AP-mode page and in the serial log; see
   [docs/API.md](docs/API.md) and `tools/push_image.py` for usage, or enable
   open mode to skip the token entirely.
4. Sleep behavior is documented in [docs/SLEEP.md](docs/SLEEP.md).

![Inkstone Web](docs/images/inkstone.png)
