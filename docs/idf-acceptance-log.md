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

后续（同一晚，继续记录）：

- **用户报告：设备列表文字乱码**。定位到 FATFS 默认值（`LFN_NONE` + CP437），
  已在 `sdkconfig.defaults` 打开长文件名 + UTF-8 API + CP936 兜底（`f195b0d`，镜像 `0x1F6430` /
  `B4BC9C45B8165151`），并把设置页「刷新列表」接成真扫描以重写卡上索引。
  **待确认**：刷新后是否仍有乱码；若仍有，需分清是文件名还是固定界面文字（两种方向见交接文档第 4 节）。
- PN532 按用户要求暂停调试：原生服务在探测失败后不再每 3.5 s 重试（`3a02810`），
  I2C NACK 从每 3.5 s 3 行降到 5 分钟一次；第 7 组整体挂起。
- 音量：整晚未改动，未写 FRAM（该镜像 FRAM 未确认，音量只在 RAM）。

## 2026-11-04 第二轮：FRAM / 界面 / 小程序 gap

用户上板后报三件事：配置重启丢失、列表/播放器与旧版完全不一致、蓝牙和小程序有 gap。

### FRAM 持久化

- 根因不是硬件：`build-ui` 的 `CONFIG_STORYBOX_FRAM_CONFIRMED` 是关闭的，整条 FRAM 路径被
  编译掉。I2C 扫描 `0x50..0x57` 全部应答，符合 MB85RC16 的 bank 别名。
- 默认开启 + 运行时 8 bank 写读探测后，真机启动日志：

```
I (1503) fram: legacy FRAM online (8 bank probes passed)
I (1513) app: FRAM ready: brightness=40 rotation=3 eq=0 volume=0 favorites=yes
```

- 用 `build-usb-product`（n=3）做写→重启→读：brightness 55、eq bass、rotation 1、volume 7
  全部保持；判定 **PASS**。测试后恢复 `40 / 3 / vocal / 0`，没有留下大音量。
- 结论：**FRAM 持久化已通**；家长记录（`bound:true`）也能从 FRAM 恢复。

### BLE

- 设备名改为 `StoryBox-CBD6`（BT MAC 后两字节），与小程序 `StoryBox-` 前缀一致；
  名字放 scan response，避免 31 字节广播包溢出。
- 启动日志：`BLE service 'StoryBox-CBD6' starting`、`GAP procedure initiated: advertise`
  （itvl 160/320），无 `advertising fields rejected` / `scan response fields rejected`。
- 手机侧扫描、绑定、童锁、`evt:state` 的 `mode/title/dur` 仍需实测（本轮无手机）。

### 界面

- 代码已按 `src/ui.cpp` 恢复 42px 列表行、旧图标/字体、播放器封面卡/信息列/控制条、
  音频故事复用播放器布局、视频控制条进度与时间。
- **没有像素级真机证据**：串口只能确认编译/启动，需要目视确认列表与播放器。

### 本轮其它观察

- PN532 仍不在线：`probe failed: NoDevice`，I2C 扫描仍能看到 `0x50..0x57`（FRAM），符合预期；
  NACK 已按 5 分钟退避。
- 主机测试：`file_reader`/`upload_transaction`/`transfer_frame` 仍是 Windows POSIX 语义问题；
  `media_library` 的非 ASCII 目录在 MinGW 下可能失败。其余相关测试通过。
