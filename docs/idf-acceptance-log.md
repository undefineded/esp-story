# 真机验收记录

配合 [验收清单](idf-hardware-acceptance.md) 使用。每次上机新加一节，**只记录真实观察到的现象**；
没有测的写「未测」，不要用推测填空。分组编号与验收清单一致。

## 记录规则

- 一节对应一次上机（同一块板、同一份镜像、同一天）。
- 必须写清镜像：commit + 构建目录 + `storybox.bin` 的 SHA256 前 16 位（见清单 0.1 的冻结表）。
  重新构建过就换一行，不要混着记。
- 每组给一个结论：`pass` / `fail` / `partial` / `未测`，失败要写**现象 + 怀疑点 + 日志关键词**。
- 长日志放仓库外的 `logs/`，这里只贴关键几行。

## 会话模板（复制这一节来填）

### 2026-MM-DD 会话 N

- 板型 / 硬件改动：
- 镜像：commit `________`，`build-____`，SHA256 `________________`（前 16 位）
- 串口：`COM__`，供电方式：
- 结果汇总：

| 组 | 结论 | 关键观察 / 失败信号 |
|---|---|---|
| 0 上电冒烟 | 未测 | |
| 1 显示/背光/输入 | 未测 | |
| 2 音乐 | 未测 | |
| 3 故事 | 未测 | |
| 4 视频 | 未测 | fps=?，单帧解码=?ms，发热=? |
| 5 BLE | 未测 | |
| 6 USB 文件 | 未测 | |
| 7 NFC | 未测 | |
| 8 家长控制 | 未测 | |
| 9 电源 | 未测 | 熄屏=?mA，深睡=?mA |
| 10 OTA | 未测 | 成功/断电/panic 三条路径 |

- 启动日志（关键行）：
- 本次踩到的问题与下一步：

## 历史会话

### 2026-11-04 会话 1（凌晨，第一块板）

- 板型 / 硬件改动：`legacy`（未确认），PN532 未见应答；TF 卡已插。COM5 = 板载 USB-Serial-JTAG。
- 镜像（同一晚两次，第一次暴露出两个 bug）：
  - `024dd25` 构建，`build-ui`，`0x1C8390`，SHA256 `4A6E17A99948A19E` — **BLE 完全不工作**
  - `456d80d` 构建，`build-ui`，`0x1CAA30`，SHA256 `E23778F76E12F550` — 修复后
- 串口：COM5（USB-Serial-JTAG，console 走它），供电：USB。

| 组 | 结论 | 关键观察 |
|---|---|---|
| 0 上电冒烟 | **pass（日志层）** | Flash 16777216 B / QIO 80 MHz；PSRAM 8388608 B；SD mount ok；`display: PSRAM canvas ready`；BLE 广播启动 |
| 1 显示/背光/输入 | 未测 | 需要目视 |
| 2 音乐 | 未测 | 因「夜里不能出声」推迟 |
| 3 故事 | 未测 | 同上 |
| 4 视频 | 未测 | |
| 5 BLE | 部分 | 广播已启动（修复后）；连接/hello/命令未测 |
| 6 USB 文件 | 未测 | 默认镜像没有 USB 协议，需要换 `build-usb-product` |
| 7 NFC | 部分 | `PN532 probe failed: NoDevice (ping=0)`；I2C 扫描到 `0x50`~`0x57`（FRAM/EEPROM 区），未烧写 |
| 8 家长控制 | 未测 | 默认镜像没有 FRAM，音量/策略只在 RAM |
| 9 电源 | 未测 | 需要 `build-power` |
| 10 OTA | 未测 | 需要 `build-ota-usb`（整片擦除） |

启动日志关键行（修复后）：

```
I storybox: board=legacy (unverified); IDF=v5.5.1; reset=11
I storybox: Flash=16777216 bytes
I storybox: PSRAM=8388608 bytes; internal free=305035; largest=212992
I cpu_start: cpu freq: 160000000 Hz
I esp_psram: Speed: 40MHz
I display: PSRAM canvas: ready
I NimBLE: GAP procedure initiated: advertise;
I ble: BLE service 'StoryBox' starting
I app: SD mount: ok
I pn532: [NFC] PN532 probe failed: NoDevice (ping=0)
I pn532: [NFC] I2C scan: 0x50 0x51 0x52 0x53 0x54 0x55 0x56 0x57
```

本次踩到的问题：

1. **BLE 从没启动过**：TX 特征没有 access callback，NimBLE 5.5 的
   `ble_gatts_chr_is_sane()` 直接判 `BLE_HS_EINVAL`，`ble_gatts_count_cfg` 返回 3，
   服务未注册、无广播。已修（`456d80d`）。
2. **NVS 从未初始化**：`phy_init` 报 `NVS has not been initialized`，每次启动都重做 RF 校准。
   已在 `app_main` 里先初始化 NVS。已修（`456d80d`）。
3. **PN532 每个轮询周期刷 3 条 I2C NACK**（约 3.5 s 一次），因为探测失败后仍在轮询。
   待定：是否需要「探测失败就不再轮询」。
4. 配置观察（不是 bug，但影响性能判断）：CPU 只跑 **160 MHz**、PSRAM **40 MHz**，
   官方 esp_player 的 28 fps 是 240 MHz/80 MHz 条件下的数字。

下一步：先目视确认第 1 组，再决定是否补 PN532 的退避逻辑；第 2/3 组等安静时段或戴耳机。
