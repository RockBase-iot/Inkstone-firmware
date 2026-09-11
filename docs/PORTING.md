# Porting: Adding a New Board

This firmware is a multi-board framework: no pin, resolution, or frame size
is hardcoded anywhere in the firmware — everything comes from a board header.
Adding a board takes two steps.

## 1. Add `src/boards/<board>.h`

Use `nm_epd_420_4c.h` as the template. The following macros are required
(`board.h` checks them at compile time):

| macro | meaning |
|---|---|
| `BOARD_NAME` | board name string |
| `BOARD_MDNS_HOST` | mDNS base name (runtime adds MAC suffix → `<host>-xxxxxx.local`) |
| `BOARD_AP_SSID_PREFIX` / `BOARD_AP_DEFAULT_PASS` | AP hotspot prefix (runtime: `prefix + last 3 MAC bytes`, distinguishes multiple devices) and default password |
| `EPD_WIDTH` / `EPD_HEIGHT` | panel resolution |
| `EPD_COLORS` | color count (only 4-color 2bpp implemented so far) |
| `EPD_FRAME_BYTES` | full frame size in bytes (4-color = W*H/4) |
| `PIN_EPD_*` | EPD SPI pins (SCK/MOSI/CS/DC/RST/BUSY) |
| `PIN_BOOT_BTN` | RTC GPIO wake button |
| `PIN_LORA_EN / CODEC_EN / ADC_EN / TEMP_CTL / PA_CTRL` | peripheral power enables (point NC pins somewhere safe and adjust the pre-sleep sequence) |
| `PIN_BATT_ADC / BATT_ADC_DIV / BATT_ADC_*` | battery ADC |
| Sleep defaults such as `IDLE_TIMEOUT_S` | see `nm_epd_420_4c.h` |

If the new panel uses a different GxEPD2 driver class, add an
`#elif defined(BOARD_<NAME>)` branch to the driver-selection block in
`src/display/display_service.cpp`.

## 2. Add an env in platformio.ini

```ini
[env:<board>]
build_flags =
    ${env.build_flags}
    -DBOARD_<NAME_UPPER>=1
```

`pio run -e <board>` produces `inkstone-<board>.bin` (merged 0x0 image).

## Current limitations

- The quantize/pack/direct-write pipeline implements only the **4-color 2bpp
  MSB-first line format** (matching GDEY0420F51). B/W or 3-color panels need
  packing-format branches in `image_pipeline` and `display_service`.
- Pin and panel constants must be verified against the board's official
  factory firmware before being written — never fill them in from memory.

## Frame-storage sizing for larger panels

The cached frame (frame_store) scales with resolution and bit depth:

| panel | pixels | format | frame size |
|---|---|---|---|
| GDEY0420F51 400×300 4-color (current) | 120,000 | 2bpp | 30,000 B ≈ 29.3 KB → cached in the 2MB raw `frame` partition |
| Spectra 6 1600×1200, packed | 1,920,000 | 3bpp | 720,000 B ≈ 703 KiB |
| Spectra 6 1600×1200, controller line format | 1,920,000 | 4bpp | 960,000 B ≈ 937.5 KiB |

Design notes for the 6-color generation:

1. **Frame storage uses a dedicated raw flash partition** (implemented):
   `frame, data, 0x99, 0x320000, 0x200000` (2MB) in
   `partitions_inkstone.csv`, accessed via `esp_partition_read/write` in
   `frame_store.cpp` with a 16-byte header (magic + length + CRC32). NVS
   stays for small config data only. The 2MB size covers the 30KB 4-color
   frame today and leaves room for the ~960KB 6-color frame later.
2. **PSRAM budgeting**: a 960KB frame buffer is fine, but the JPEG
   intermediate (1600×1200×2B = 3.84MB) plus the current full-frame FS error
   arrays (1.92M px × 3 ch × 4 B ≈ 23MB) would exceed 8MB PSRAM. Before
   bringing up a large panel, switch the FS dither to **two-row rolling error
   buffers** (≈ 2×1600×3×4 B ≈ 38KB, resolution-independent).
3. Suggested order: ~~migrate frame_store NVS → raw partition~~ (done) → two-row error
   dither → 6-color palette + packing branch → new board header + env.

---

# 支持新板子（PORTING）

本固件是多 board 框架：固件代码不硬编码任何引脚/分辨率/帧长，
全部来自板级头文件。接入一块新板子只需两步：

## 1. 新增 `src/boards/<board>.h`

以 `nm_epd_420_4c.h` 为模板，必须定义（`board.h` 会在编译期检查）：

| 宏 | 说明 |
|---|---|
| `BOARD_NAME` | 板名字符串 |
| `BOARD_MDNS_HOST` | mDNS 基础名（运行时追加 MAC 后缀 → `<host>-xxxxxx.local`） |
| `BOARD_AP_SSID_PREFIX` / `BOARD_AP_DEFAULT_PASS` | 配网热点前缀（运行时为 `前缀+MAC后三字节`，多设备区分）与默认密码 |
| `EPD_WIDTH` / `EPD_HEIGHT` | 面板分辨率 |
| `EPD_COLORS` | 颜色数（当前仅实现 4 色 2bpp） |
| `EPD_FRAME_BYTES` | 整帧字节数（4 色 = W*H/4） |
| `PIN_EPD_*` | EPD SPI 引脚（SCK/MOSI/CS/DC/RST/BUSY） |
| `PIN_BOOT_BTN` | RTC GPIO 唤醒键 |
| `PIN_LORA_EN / CODEC_EN / ADC_EN / TEMP_CTL / PA_CTRL` | 外设电源使能（无则指向 NC 脚并在睡前序列中调整） |
| `PIN_BATT_ADC / BATT_ADC_DIV / BATT_ADC_*` | 电池 ADC |
| `IDLE_TIMEOUT_S` 等休眠默认值 | 见 `nm_epd_420_4c.h` |

若新面板的 GxEPD2 驱动类不同，在 `src/display/display_service.cpp`
的驱动选择块追加一个 `#elif defined(BOARD_<NAME>)` 分支即可。

## 2. 新增 platformio.ini env

```ini
[env:<board>]
build_flags =
    ${env.build_flags}
    -DBOARD_<NAME_UPPER>=1
```

`pio run -e <board>` 即可产出 `inkstone-<board>.bin`（0x0 合并固件）。

## 当前限制

- 量化/打包/直写链路只实现了 **4 色 2bpp MSB-first 线格式**（与
  GDEY0420F51 一致）。黑白/三色面板需扩展 `image_pipeline` 与
  `display_service` 的打包格式分支。
- 引脚与面板常量必须与对应板的官方 factory 固件核实后再写入，
  禁止凭印象填。

## 大面板的帧存储空间测算

缓存帧（frame_store）随分辨率和位深增长：

| 面板 | 像素数 | 格式 | 整帧大小 |
|---|---|---|---|
| GDEY0420F51 400×300 四色（当前） | 120,000 | 2bpp | 30,000 B ≈ 29.3 KB → 缓存在 2MB raw `frame` 分区 |
| Spectra 6 1600×1200 紧凑打包 | 1,920,000 | 3bpp | 720,000 B ≈ 703 KiB |
| Spectra 6 1600×1200 控制器线格式 | 1,920,000 | 4bpp | 960,000 B ≈ 937.5 KiB |

6 色世代的设计要点：

1. **帧存储使用独立 raw flash 分区**（已实现）：
   `partitions_inkstone.csv` 中的 `frame, data, 0x99, 0x320000, 0x200000`
   （2MB），通过 `frame_store.cpp` 里的 `esp_partition_read/write` 访问，
   带 16 字节头（magic + 长度 + CRC32）。NVS 仅保留小配置数据。
   2MB 容量既覆盖当前 30KB 四色帧，也为后续约 960KB 的六色帧预留空间。
2. **PSRAM 预算**：960KB 帧缓冲没问题，但 JPEG 中间缓冲
   （1600×1200×2B = 3.84MB）叠加当前全帧 FS 误差数组
   （1.92M 像素 × 3 通道 × 4B ≈ 23MB）会超出 8MB PSRAM。
   接大屏前须把 FS 抖动改为**双行滚动误差缓冲**
   （约 2×1600×3×4B ≈ 38KB，与分辨率无关）。
