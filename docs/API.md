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

**Multi-device addressing**: each device answers at
`inkstone-xxxxxx.local` (`xxxxxx` = last 3 MAC bytes, lowercase), so several
devices can share one network.

## POST /api/v1/display — main endpoint

| Content-Type | body | handling |
|---|---|---|
| `application/octet-stream` | 30000B raw 2bpp | written straight to the panel (fastest, recommended) |
| `image/jpeg` | JPEG bytes | on-device decode → 4:3 center-crop → scale to 400×300 → FS-dither quantize |
| `image/png` | PNG bytes | same as JPEG |

Query: `?dither=off` disables dithering (on by default).

| code | meaning |
|---|---|
| 202 | accepted, async refresh (~30 s) |
| 400 | bad length/format |
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
 "auth_open":false}
```

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

---

# NM-EPD-420-4C API 文档（Phase 2）

前缀 `/api/v1`，除标注外均需 `Authorization: Bearer <token>`。
token 首次启动随机生成（32 位 hex），显示在 AP 模式页面与串口日志；
`POST /api/v1/token` 可轮换（需旧 token）。

**开放模式（免 token，家用简化）**：`POST /api/v1/auth {"open": true}` 后，
`/api/v1/*` 全部免鉴权，NVS 持久化，默认关闭。AP 模式下可直接切换（同热点访客本就能看到 token）；STA 模式下必须持 token 才能改，防止局域网内他人偷偷放开鉴权。
家用可信网络可开；设备会暴露到不可信网络时请保持关闭。

**多设备寻址**：每台设备的地址为 `inkstone-xxxxxx.local`
（`xxxxxx` = MAC 后三字节小写），同网多台互不冲突。

## POST /api/v1/display — 主接口

| Content-Type | body | 处理 |
|---|---|---|
| `application/octet-stream` | 30000B raw 2bpp | 直写面板（最快，推荐） |
| `image/jpeg` | JPEG 字节流 | 设备端解码 → 裁剪 4:3 → 缩放 400×300 → FS 抖动量化 |
| `image/png` | PNG 字节流 | 同上 |

查询参数：`?dither=off` 关闭抖动（默认开）。

| 状态码 | 含义 |
|---|---|
| 202 | 已接受，异步刷新（约 30 s） |
| 400 | 长度/格式错误 |
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
 "auth_open":false}
```

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
