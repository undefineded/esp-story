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

### 2026-11-25 会话（视频打开失败 / 列表卡顿）

- 板型 / 硬件改动：ESP32-S3（`legacy` 配置），PN532 未接。
- 镜像：`fd59da0-dirty` + 本次改动，`build-usb-nfc`，`0x2AF830`。
- 串口：COM5（USB-Serial-JTAG）；供电：USB。
- 结果汇总：

| 组 | 结论 | 关键观察 |
|---|---|---|
| 4 视频 | **pass（列表 / 播放 / 退出 / 再开）** | 修复前打开视频直接 `音频输出初始化失败`；修复后管线启动，`videoFirstBackendFrameMs=504`，`videoBackendFrames` 正常累加，退出走 `QUIT in state: PLAYING` |
| 2 音乐 | pass | 音频任务搬到 PSRAM 后播放正常，无断言、无断音 |

- 现象与根因（真实日志）：

```
E (41869) i2s_common: i2s_alloc_dma_desc(541): allocate DMA buffer failed
E (41869) pcm: i2s setup: ESP_ERR_NO_MEM
E (41869) video: i2s begin: ESP_ERR_NO_MEM
E (41869) video: audio output failed: internal=10443 largest=10240 dma=10439 psram=7034440
```

  内部 RAM 只剩 10 KB，而视频 I2S 环要 12×1920 B=23 KB 连续块；PSRAM 还有 7 MB。
  分阶段测量定位到：BLE 一项 76 KB（`after ble: 156267 -> 80527`），媒体索引 39 KB。
  只改 BLE 主机堆到 PSRAM 后，视频往后退了一步、卡在`esp_gmf_task_init:482 内存耗尽`
  （连信号量都建不出来），再把 audio/usb-tx/usb-rx/video-control 四个任务栈搬 PSRAM 才通过。

- 关键内存曲线（`build-usb-nfc`，开机各阶段 `internal free`）：

```
before media index   58.7 KB -> 105.5 KB
after media index    19.8 KB ->  66.4 KB
video: after i2s     10.4 KB ->  41.9 KB
播放中最低点          失败     ->  25.6 KB
```

- 踩到的坑：
  - `app` 任务栈放 PSRAM 会在 BLE 初始化写 NVS 时 panic：
    `assert failed: spi_flash_disable_interrupts_caches_and_other_cpu (esp_task_stack_is_sane_cache_disabled())`。
    `app` 与 `usb-files`（写 OTA 分区）必须留在内部 RAM。
  - 修改 `build-*/sdkconfig` 会触发 CMake 重配置，进而调用组件管理器访问 registry/git，离线环境下
    300 s 超时失败；此时可直接改 `build-*/config/sdkconfig.h` 并把 sdkconfig 的 mtime 回拨，
    用 `ninja -C build-…` 增量编译绕过。
  - 视频列表 400+ 集时逐文件 `f_stat` 是 O(n²)（FATFS 每次从目录首项线性查找），
    这是「点进视频 tab 卡顿」的真正原因，与内存无关。

- 未测：连续播放 10 分钟音画同步、12/15/20 fps 三档、USB+BLE+省电组合下的功耗。
- 完整过程与回归溯源见 [视频打不开与内部 RAM 排查记录](video-ram-bugfix-log.md)。

### 2026-11-20 会话（NFC 实卡，USB 产品镜像）

- 板型 / 硬件改动：ESP32-S3（`legacy` 配置），外接 PN532 模块；NTAG215 卡放在线圈上。
- 镜像：commit `fd59da0-dirty`，`build-usb-nfc`，`0x2A9AC0`，SHA256 `F38477C2BEB50FE6`。
- 串口：COM5（USB-Serial-JTAG）；供电：USB。
- 结果汇总：

| 组 | 结论 | 关键观察 |
|---|---|---|
| 7 NFC | **pass（模块/读/写/校验/原始读）** | PN532 fw 1.6；NTAG215 UID `04F9BD32E32A81`，cap 496B，formatted；36B 文本 NDEF 写入并 verify done；near-capacity 488B 写入并 verify done；page 4+12 原始读与写入 TLV 一致 |

- 启动日志关键行：

```
I (1315) app: NFC service ready
SBX4...{"v":4,"evt":"nfc","type":"module","present":false,...}
```

  接上模块后未重新上电，`nfc.poll` 立即探测成功（`present=true, fw=1.6`），验证了缺席退避不会被 host poll 吞掉。

- 实测记录：
  - `tools/nfc_smoke_test.py --port COM5`：`features=['fs_transfer','nfc','nfc.ndef','nfc.raw','nfc.write-verify']`，status `present=True chip=PN532 fw=1.6 polling=True`。
  - 36B 文本 `"月亮邮递员 · 第 03 集"`：`write_ndef` accepted → 分页进度 → verifying → done；`refresh` 回读 `03 24 D1 01 20 54 02 7A 68 … FE`。
  - 470 个 ASCII 字符（总写入 488B，请求 JSON 超过旧的 1 KB 上限）：写入/校验通过，覆盖 2 KB 请求上限修复。
  - `--read-pages 4,12`：48B 原始数据与 NDEF TLV 一致。
  - `media.info`：`ready=true`，285 首音乐、656 个故事、6 个分类；`media.tracks/stories` 分页路径正确（用于桌面资源卡片选择，替代可能超时的 `fs.list` 目录遍历）。
  - NFC 总开关：`sys.set {"nfc":false}` 后 `enabled=false`、`present=false`，`nfc.poll` 回 `NFC_DISABLED`；`sys.reboot` 后仍保持关闭；`sys.set {"nfc":true}` 后服务重新探测到 PN532 fw 1.6 并可正常 `poll`。
  - 独立自动播放：写入指向 `/music/儿歌多多原创/一根小手指.mp3` 的资源卡后重启，10 秒内不发送上位机命令，`sys stats` 返回 `player.playing=true`、`title=一根小手指.mp3`、`curSec≈12`；卡内 URI 为 `storybox://media?path=…`。当时音量为 0，播放是静音的。
- 未测：MIFARE Classic、监听中的移卡 `removed` 事件、桌面端资源卡片 UI 的人工操作、1 s 轮询周期的示波器/日志实测。会话结束时卡内为指向“一根小手指”的资源卡，NFC 开关为开；设备正在播放该曲目（音量 0）。

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
