# Sleep Behavior (SLEEP)

Wi-Fi is down in deep sleep, so **neither the web page nor the API is
reachable**. The device is designed around "a well-defined service window
after each wake, reclaimed automatically when idle".

## State machine

```
power-on / wake → AWAKE (HTTP service)
  ├─ /display received → REFRESHING (25–30 s, dedicated task) → grace window
  ├─ idle beyond IDLE_TIMEOUT_S (default 300 s) → PRE_SLEEP → DEEP_SLEEP
  └─ never sleeps automatically when stay_awake=true
DEEP_SLEEP → BOOT key (ext0) wakes back to AWAKE;
             timer wake is reserved for pull mode
```

## Default parameters (overridable via `POST /api/v1/sleep` and persisted in NVS)

| parameter | default | meaning |
|---|---|---|
| IDLE_TIMEOUT_S | 300 | sleep after this long without HTTP activity |
| POST_REFRESH_GRACE_S | 120 | forced awake time after a refresh (lets you check the result) |
| MIN_REFRESH_INTERVAL_S | 60 (debug; 600 recommended for production) | min interval between refreshes (may only be raised) |
| SCHED_WAKE_S | 0 | scheduled wake period, 0 = disabled |
| BATT_LOW_MV | 3400 | below this after wake: skip refresh, back to sleep |

## Hard rules

1. Never sleep while refreshing.
2. Fixed pre-sleep sequence: RTC state → `powerOff()` → pull peripheral-enable
   pins low (47/44/40/41/43) → Wi-Fi off → configure wake sources →
   `esp_deep_sleep_start()`.
3. Wake dispatch: button → AWAKE; timer → back to sleep immediately when no
   pull source is configured; cold boot → normal provisioning flow.
4. GPIO0 doubles as the download strap: **a short BOOT press wakes the device
   from deep sleep — do not hold BOOT at power-on**.
5. Low-battery guard: below 3400 mV after wake (and not stay-awake) →
   straight back to sleep.

## Frame persistence (cold boot)

Each successful user-requested refresh is cached to the dedicated 2MB raw
flash partition `frame` (with a magic + length + CRC32 header). On cold boot,
the cached frame is displayed again; if no cache exists (or the CRC check
fails), the built-in 4-color test pattern is shown. After deep sleep the panel
retains its image, so no refresh happens on wake.

## Usage notes

- Battery setups: probe before pushing (see API.md); the device being offline
  is normal behavior.
- USB-powered desktop setups: enable `{"stay_awake":true}` for
  always-on-call behavior.
- Holding BOOT for 5 s at runtime: clears Wi-Fi credentials and reboots into
  AP mode (rescue channel).

---

# 休眠逻辑说明（SLEEP）

深睡时 Wi-Fi 断开，**Web 页面与 API 均不可用**。设备围绕"唤醒后有明确
服务窗口，空闲后自动收回"设计。

## 状态机

```
上电/唤醒 → AWAKE（HTTP 服务）
  ├─ 收到 /display → REFRESHING（25–30 s，独立任务）→ 完成后 grace 窗口
  ├─ 空闲超过 IDLE_TIMEOUT_S（默认 300 s）→ PRE_SLEEP → DEEP_SLEEP
  └─ stay_awake=true 时永不自动休眠
DEEP_SLEEP → BOOT 键（ext0）唤醒回 AWAKE；定时唤醒为 pull 模式预留
```

## 默认参数（可通过 `POST /api/v1/sleep` 覆盖并持久化到 NVS）

| 参数 | 默认 | 说明 |
|---|---|---|
| IDLE_TIMEOUT_S | 300 | 无 HTTP 活动多久后睡 |
| POST_REFRESH_GRACE_S | 120 | 刷新后强制清醒（确认效果） |
| MIN_REFRESH_INTERVAL_S | 60（调试期；量产建议 600） | 刷屏最小间隔（仅可上调） |
| SCHED_WAKE_S | 0 | 定时唤醒周期，0=禁用 |
| BATT_LOW_MV | 3400 | 低电跳过刷新立刻回睡 |

## 硬规则

1. 刷新中绝不睡眠。
2. 睡前序列顺序固定：RTC 状态 → `powerOff()` → 拉低外设使能脚
   （47/44/40/41/43）→ Wi-Fi 断开 → 配置唤醒源 → `esp_deep_sleep_start()`。
3. 唤醒分流：按键 → AWAKE；定时 → 无 pull 配置则立即回睡；冷启动 → 正常配网流程。
4. GPIO0 复用下载模式：**深睡时短按 BOOT 唤醒即可，不要在上电瞬间长按**。
5. 低电保护：唤醒后电量 < 3400 mV 且非常醒 → 立即回睡。
6. 若需要重新刷写代码，可按住BOOT键，然后RESET键进入下载模式。

## 帧持久化（冷启动）

每次用户成功刷屏的帧会缓存进独立的 2MB raw flash 分区 `frame`
（带 magic + 长度 + CRC32 校验头）。冷启动时若缓存有效则显示用户内容，
无缓存（或 CRC 校验失败）则显示内置四色条纹。
深睡唤醒后面板图像仍在，不触发刷新。

## 使用建议

- 电池场景：推送前先探活（见 API.md），设备不在线是正常行为。
- 桌面常供电：调 `{"stay_awake":true}` 后随叫随到。
- 运行期长按 BOOT 5 s：清除 Wi-Fi 凭据并重启回 AP 模式（救援通道）。
