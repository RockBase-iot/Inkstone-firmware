# NM-EPD-420-4C API Reference (Phase 2)

All endpoints are prefixed with `/api/v1` and, unless noted, require
`Authorization: Bearer <token>`. The token is generated randomly on first
boot (32 hex chars) and shown on the AP-mode page and in the serial log;
`POST /api/v1/token` rotates it (requires the old token).

**Open mode (no token, for home use)**: after
`POST /api/v1/auth {"open": true}`, all `/api/v1/*` calls skip auth.
NVS-persisted, off by default. Directly toggleable in AP mode (anyone on the
hotspot can already see the token); in STA mode a valid token is required to
change it, so nobody on the LAN can silently disable auth. Fine for trusted
home networks; keep it off if the device may join untrusted networks.

**mDNS access**: after provisioning, the device answers at `inkstone.local`,
so users do not need to look up its IP. Only one device using this fixed
hostname should be active on a LAN.

## POST /api/v1/display — main endpoint

| Content-Type | body | handling |
|---|---|---|
| `application/octet-stream` | 30000B raw 2bpp | written straight to the panel (fastest, recommended) |
| `image/jpeg` | JPEG bytes | on-device decode → 4:3 center-crop → scale to 400×300 → quantize by `profile` |
| `image/png` | PNG bytes | same as JPEG |

### Query parameters

| Query | Values | Default | Meaning |
|---|---|---|---|
| `profile` | `default` / `retro` / `none` | `default` | Quantization profile. `default` = luma-weighted Floyd–Steinberg. `retro` = OKLab matching against a panel-calibrated warm palette, with hue-aware candidate masking (cool hues are dithered with black+white only) and a stylised tone curve. `none` = nearest colour, no dithering. |
| `dither` | `on` / `off` | `on` | Legacy alias. `off` is equivalent to `profile=none`. `profile` wins when both are given. |

An unrecognised `profile` value returns **400**, not a silent fallback. With
`application/octet-stream` the `profile` parameter is ignored: the body is
already a packed frame, so it is written as-is.

`retro` holds roughly 1.9 MB of PSRAM (work + mask + error buffers) versus the
`default` path's error buffers alone. If allocation fails it answers **422**
with `"hint":"retro requires more PSRAM"`.

### Choosing between them

Neither profile is strictly better; they differ in what they optimise.

| | `default` | `retro` |
|---|---|---|
| Goal | fidelity | stylised, film-like grain |
| Cool regions (sky, water) | dithers into red speckle | masked to black+white only |
| Highlights / shadows | can crush to pure white / black | lifted and compressed |
| Good for | warm-toned subjects, general use | images with large cool areas |

For an interactive side-by-side comparison, use the upload page — it renders both
and lets the user pick. Both profiles are byte-identical across the firmware, the
browser and `tools/convert_image.py`; see `docs/RETRO.md`.

| code | meaning |
|---|---|
| 202 | accepted, async refresh (~30 s) |
| 400 | bad length/format, or unknown `profile` |
| 401 | missing/wrong token |
| 409 | refresh in progress, body carries `retry_after_s` |
| 415 | unsupported Content-Type |
| 422 | decode/quantize failed |
| 429 | min refresh interval not met, carries `Retry-After` header |

## GET /api/v1/status

```json
{"busy":false,"last_update":1726..., "battery_mv":3902,"ip":"192.168.1.66",
 "ssid":"Home","mode":"sta","sleep_in_s":241,"min_interval_s":60,
 "next_allowed_update":1726...,"stay_awake":false,"boot_count":7,
 "auth_open":false,"profiles":["default","retro","none"]}
```

`profiles` lists the quantization profiles this build accepts, so a caller can
discover capability rather than probe for it.

## POST /api/v1/sleep

- `{"delay_s": N}` — enter deep sleep N seconds from now
- `{"stay_awake": true|false}` — stay-awake mode (kept in RTC, survives reboot)

## POST /api/v1/token — rotate token, returns `{"token":"..."}`

## POST /api/v1/auth — open mode toggle

`{"open": true|false}`: once open, `/api/v1/*` skips Bearer auth (home
simplification). Callable without a token in AP mode; requires a token in STA
mode. `auth_open` in `/api/v1/status` and `/setup-info` reflects the state.

## Constraints for AI callers (plan section 6.4)

1. The device may be in deep sleep: probe with `GET /api/v1/status` first;
   **no response within 3 s means it is asleep** — ask the user to press the
   BOOT key, or wait for a scheduled wake window.
2. After a refresh the device stays awake for `POST_REFRESH_GRACE_S`
   (default 120 s) and may then sleep; never assume it stays online.
3. For always-on desktop use (USB powered), call
   `POST /api/v1/sleep {"stay_awake":true}` first.
4. Minimal call example: `tools/push_image.py`.

## Pixel format cheat sheet (plan section 3)

2 bits/px: 0=black 1=white 2=yellow 3=red; 4 pixels per byte, MSB first;
row-major; full frame 400×300/4 = 30000 bytes. Reference implementation:
`tools/convert_image.py`.

**Index order is per-profile.** The packer writes the palette index straight
into the byte, so each profile defines its own order:

| index | `default` | `retro` |
|---|---|---|
| 0 | black (0,0,0) | black (35,31,32) |
| 1 | white (255,255,255) | white (233,231,222) |
| 2 | **yellow** (255,210,0) | **red** (193,80,62) |
| 3 | **red** (200,30,30) | **yellow** (229,197,79) |

Red and yellow are swapped between the two, and the RETRO values are the warm
panel-calibrated ones rather than pure RGB. A caller that decodes a frame must
know which profile produced it.

---

# NM-EPD-420-4C API 文档（Phase 2）

前缀 `/api/v1`，除标注外均需 `Authorization: Bearer <token>`。
token 首次启动随机生成（32 位 hex），显示在 AP 模式页面与串口日志；
`POST /api/v1/token` 可轮换（需旧 token）。

**开放模式（免 token，家用简化）**：`POST /api/v1/auth {"open": true}` 后，
`/api/v1/*` 全部免鉴权，NVS 持久化，默认关闭。AP 模式下可直接切换（同热点访客本就能看到 token）；STA 模式下必须持 token 才能改，防止局域网内他人偷偷放开鉴权。
家用可信网络可开；设备会暴露到不可信网络时请保持关闭。

**mDNS 访问**：配网后设备地址为 `inkstone.local`，无需查询 IP；同一局域网内只应有一台设备使用该固定名称。

## POST /api/v1/display — 主接口

| Content-Type | body | 处理 |
|---|---|---|
| `application/octet-stream` | 30000B raw 2bpp | 直写面板（最快，推荐） |
| `image/jpeg` | JPEG 字节流 | 设备端解码 → 裁剪 4:3 → 缩放 400×300 → 按 `profile` 量化 |
| `image/png` | PNG 字节流 | 同上 |

### 查询参数

| 参数 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `profile` | `default` / `retro` / `none` | `default` | 量化方案。`default` = 亮度加权 Floyd–Steinberg；`retro` = OKLab 匹配面板校准暖色盘，附带色相感知候选掩码（冷色相只用黑白抖动）与风格化影调曲线；`none` = 最近色，不抖动。 |
| `dither` | `on` / `off` | `on` | 旧参数别名。`off` 等价于 `profile=none`；两者同时给出时以 `profile` 为准。 |

无法识别的 `profile` 取值返回 **400**，不会静默回退。当 Content-Type 为
`application/octet-stream` 时 `profile` 被忽略：body 已是打包好的帧，原样写入。

`retro` 需要约 1.9 MB PSRAM（work + mask + 误差缓冲），而 `default` 只有误差
缓冲。分配失败时返回 **422**，body 带 `"hint":"retro requires more PSRAM"`。

### 两者怎么选

没有绝对优劣，差别在优化目标：

| | `default` | `retro` |
|---|---|---|
| 目标 | 保真 | 风格化、胶片颗粒 |
| 冷色区域（天空、水面） | 被扩散成红色噪点 | 掩码限制为黑白 |
| 高光 / 暗部 | 可能压成纯白 / 纯黑 | 抬升并压缩动态范围 |
| 适合 | 暖调主体、通用场景 | 含大面积冷色的图 |

要交互式并排对比，用上传页 —— 页面上两个效果同时渲染，由用户选一个推送。
两种方案在固件、浏览器、`tools/convert_image.py` 三端逐字节一致，
详见 `docs/RETRO.md`。

| 状态码 | 含义 |
|---|---|
| 202 | 已接受，异步刷新（约 30 s） |
| 400 | 长度/格式错误，或 `profile` 取值非法 |
| 401 | 无/错 token |
| 409 | 刷新中，body 含 `retry_after_s` |
| 415 | 不支持的 Content-Type |
| 422 | 解码/量化失败 |
| 429 | 距上次刷新不足 `min_interval_s`，带 `Retry-After` 头 |

## GET /api/v1/status

```json
{"busy":false,"last_update":1726..., "battery_mv":3902,"ip":"192.168.1.66",
 "ssid":"Home","mode":"sta","sleep_in_s":241,"min_interval_s":60,
 "next_allowed_update":1726...,"stay_awake":false,"boot_count":7,
 "auth_open":false,"profiles":["default","retro","none"]}
```

`profiles` 列出本固件支持的量化方案，调用方可直接发现能力而不必试探。

## POST /api/v1/sleep

- `{"delay_s": N}` — N 秒后进入深度睡眠
- `{"stay_awake": true|false}` — 常醒模式（RTC 保持，重启保留）

## POST /api/v1/token — 轮换 token，返回 `{"token":"..."}`

## POST /api/v1/auth — 开放模式开关

`{"open": true|false}`：开启后 `/api/v1/*` 免 Bearer 鉴权（家用简化）。
AP 模式可直接调用；STA 模式需 token。`/api/v1/status` 与 `/setup-info`
中的 `auth_open` 字段反映当前状态。

## AI 调用约束（§6.4）

1. 设备可能深睡：推送前先 `GET /api/v1/status`，**3 s 无响应即视为睡眠**，
   提示用户按 BOOT 键唤醒或等待定时唤醒窗口。
2. 刷新完成后设备再醒 `POST_REFRESH_GRACE_S`（默认 120 s）随后可能深睡，
   不要假设设备长期在线。
3. 桌面常供电场景先调 `POST /api/v1/sleep {"stay_awake":true}`。
4. 最小调用示例见 `tools/push_image.py`。

## 像素格式速查（§3）

2bit/px：0=黑 1=白 2=黄 3=红；每字节 4 像素 MSB 优先；行优先；
整帧 400×300/4 = 30000 字节。参考实现：`tools/convert_image.py`。

**索引顺序随 profile 而异。** 打包时索引直接写进字节，所以每个 profile
各自定义顺序：

| 索引 | `default` | `retro` |
|---|---|---|
| 0 | 黑 (0,0,0) | 黑 (35,31,32) |
| 1 | 白 (255,255,255) | 白 (233,231,222) |
| 2 | **黄** (255,210,0) | **红** (193,80,62) |
| 3 | **红** (200,30,30) | **黄** (229,197,79) |

红与黄的索引在两套之间是**相反的**，且 RETRO 用的是面板校准暖色而非纯 RGB。
解码一帧之前必须知道它是哪套方案产出的。
