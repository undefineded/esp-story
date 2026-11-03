# 交接文档：esp-story ESP-IDF 迁移

> **写于 2026-11-04 凌晨（第一块板子上机当晚）**。新会话请从头读完这一页：
> 第 4 节是**未结的 bug**，第 8 节是可复制的命令。
> 批次细节见 `docs/idf-migration-progress.md`，逐组真机结论见 `docs/idf-acceptance-log.md`。

## 1. 一句话状态

代码面（P1–P6 + BLE 家长命令）全部铺开、**五套配置编译通过、已推到 origin**（`feature/esp-idf-migration`）。
首晚上真机修掉 4 组硬件假设（BLE 完全不工作、NVS 未初始化、卡上中文名乱码、NFC 刷 I2C NACK）；
**2026-11-04 第二轮**又修掉三个现场问题：FRAM 持久化未启用、列表/播放器界面偏离旧版、
BLE 与小程序的能力 gap（设备名前缀、童锁、状态标题/时长、缓冲区上限）。详见 §1.1。
**未结**：设备列表乱码是否被 FATFS 修复彻底解决，仍缺一次目视确认（§4）；本轮界面只做了代码级对齐，
也需要目视。验收 10 组里只有第 0 组（上电冒烟）算过。

## 1.1 2026-11-04 第二轮：三个现场问题的修复

这一轮针对用户上板后报的三个问题：**FRAM 配置重启丢失**、**列表/播放器界面与旧版不一致**、
**蓝牙和小程序有 gap**。三处都已改完并重新构建；FRAM 与 BLE 名称已在 COM5 真机确认，
界面只做了代码级对齐（串口看不到像素，需要目视）。

### A. FRAM 持久化（重启丢配置）

**根因**：`CONFIG_STORYBOX_FRAM_CONFIRMED` 默认是 `n`，`build-ui` 的 sdkconfig 里就是
`# CONFIG_STORYBOX_FRAM_CONFIRMED is not set`。于是 `app.cpp` 里整条 FRAM 路径被
`#if` 编译掉，设置、音量、收藏、家长记录**全都只写 RAM**，重启必然回到默认值。
硬件本身没问题：I2C 扫描到 `0x50..0x57` 全部应答，这正是 MB85RC16 的 11 位地址
bank 别名（单个 AT24C512 只会在 `0x50` 应答）。

**修复**：

1. `main/Kconfig.projbuild` 默认改为 `y`，`sdkconfig.defaults` 显式写
   `CONFIG_STORYBOX_FRAM_CONFIRMED=y`；已生成的 5 套 `build-*/sdkconfig` 也同步改成 `y`
   （否则 `idf.py` 会沿用旧 sdkconfig，改 defaults 不生效）。
2. `FramDevice::begin()` 增加**运行时 8 bank 写读探测**：每个 bank 的 `0xF8` 写
   `0xA5 + i` 再读回，8 个都通过才启用持久化。这样即使板型/器件判断错了（例如新板
   FM24CL64B 在 `0x51`，或 `0x50` 是 EEPROM），也只退回 RAM-only，不会把记录写进错器件。
3. 启动日志新增 `FRAM ready: brightness=... rotation=... eq=... volume=... favorites=...`，
   以后一眼能看出这次启动到底有没有持久化。

**真机结论（COM5，build-ui）**：

```
I (1503) fram: legacy FRAM online (8 bank probes passed)
I (1513) app: FRAM ready: brightness=40 rotation=3 eq=0 volume=0 favorites=yes
```

USB 协议镜像（`build-usb-product`）上做过一次写→重启→读：
`brightness=55 / eq=bass / rotation=1 / volume=7` 重启后全部保持；
测试脚本 `tools/fram_check.py`（原 `_fram_check.py`）。测试后已把板子恢复成原来的
`brightness=40 / rotation=3 / eq=vocal / volume=0`，没有留大音量。

### B. 列表 / 播放器界面回到旧版

迁移时把页面重画成了简化版：列表行 38px、无图标、body 字体；播放页是深色底 + 简化信息。
旧 Arduino 版（`src/ui.cpp`）是 42px 卡片行 + 音符/文件夹/心形/胶片图标 + title 字体，
播放页是“黄色封面卡 + 信息列 + 墨色控制条 + 黄色播放按钮”。本轮按旧版逐项恢复：

- 音乐分类列表、歌曲列表：42px 行、旧图标、`N首`、`播放中`、`长按收藏`，文案与旧版一致。
- 音乐播放器：封面卡、分类徽章、`03 / 24`、跑马灯曲名、`MP3·128kbps`、时间、进度条
  （灰槽/蓝填充/黄端帽）、状态圆点、墨色控制条、黄色播放按钮和“播放/暂停”文字。
- 音乐播放器与**音频故事播放器**共用 `components/app/src/player_view.{h,cpp}`，
  音频故事页恢复成旧版“音乐播放器布局 + 音频故事封面/分类”，避免两页再次分叉。
- 故事列表、视频列表：42px 行、文件夹/音符/绘本/胶片图标；视频播放页恢复旧版
  30px 控制条：黄色暂停键 + 进度槽 + 右侧时间 + 顶部片名（前 3 秒/暂停时）。
- 导航按旧版恢复：选分类**直接起播并进播放页**；长按 NEXT 才进当前曲目列表；
  长按 OK/LAST 停止并回分类列表；长按 SET 回主页但音乐继续。

### C. 蓝牙与小程序对齐（`C:\code\SMALL_WX`）

对照 `src/utils/storyBle.ts` / `ble.ts` 与 `pages/story/*` 发现的实际 gap：

| # | 现象 | 修复 |
|---|---|---|
| 1 | 小程序只认 `StoryBox-` 前缀，IDF 固件却广播 `StoryBox` | 按 BT MAC 生成 `StoryBox-XXXX`（与旧 Arduino 一致），真机为 `StoryBox-CBD6` |
| 2 | 13 字节名字 + 128 位 UUID 超过 31 字节广播包，名字会被丢掉 | UUID 留在 advertising，名字放进 scan response（等价旧版 `setScanResponse(true)`）；重连后仍能扫到 |
| 3 | 小程序 `set childLock` 后设备只在状态里回 true，实体按键/旋钮没锁、顶栏也不显示童锁 | 主循环把 parental `childLock` 落到 `Input::setLocked()` 和 `ctx().child_lock`；支持 OK+NEXT 按住 5 秒本地解锁并写回 FRAM |
| 4 | 小程序播放故事时 `evt:state` 的 `mode` 永远是 `music`、`title` 是上一首音乐 | 新增 `setNowPlaying()` / `setCurrentStory()`，音乐/故事各自设置，`stop` 清回 `idle` |
| 5 | `dur` 永远 0，小程序和本机进度条都只能显示“活动点” | MP3 流解析 Xing/Info 帧数得到精确时长，无 Xing 时用文件大小 × 首帧码率估算；`PlaybackSnapshot.duration` 一路带到 UI 和 BLE |
| 6 | 内容白名单整包 JSON 超过 520 字节 RX 缓冲会被静默丢弃，小程序表现为“响应超时” | BLE RX 行缓冲 520→1024、单包 256→512、发送缓冲 760→1024 |
| 7 | 通知在控制器缓冲满时直接丢尾包，长回复（hello/config）可能只到一半 | `sendText()` 对每个分片重试最多 4 次（4 ms 间隔），失败才打日志 |

**真机结论**：启动日志 `BLE service 'StoryBox-CBD6' starting` +
`GAP procedure initiated: advertise`（`itvl=160/320`）；`adv_set_fields` 与
`adv_rsp_set_fields` 都没有报 rejected。小程序实际扫描/绑定/童锁还需要手机侧目视确认。

## 2. 环境（本机）

| 项 | 值 |
|---|---|
| ESP-IDF | 5.5.1，`C:\esp\v5.5.1\esp-idf` |
| 工具链 | `xtensa-esp-elf 14.2.0`、cmake 3.30.2、ninja 1.12.1，在 `C:\Users\a1509\.espressif\tools` |
| Python | 无系统 Python；IDF 环境在 `~/.espressif/python_env/idf5.5_py3.11_env`（pyserial 可用） |
| 主机编译器 | w64devkit GCC：`C:\tools\w64devkit\w64devkit\bin`（主机测试用） |
| 项目路径 | `C:\Users\a1509\Documents\PlatformIO\Projects\music-demo` |
| 代理 | Clash `127.0.0.1:7897`（`tools/idf/idf.ps1` 已内置），拉组件时确认在运行 |

### 构建入口（必须用这两个脚本）

```powershell
# 单次构建（脚本带 Zone.Identifier，必须显式 Bypass，否则报 "not digitally signed"）
powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 -B build-ui --define-cache-entry SDKCONFIG=build-ui/sdkconfig build

# 全配置回归（默认 / usb-product / ota+usb / power / fram）
powershell -ExecutionPolicy Bypass -File tools\idf\build-all.ps1
```

`tools/idf/` 在 `.gitignore` 里（含本机绝对路径），换机需重建；脚本内容见 `docs/idf-handoff.md` 旧版本附录或按上句自己写。

## 3. 今晚的板子

| 项 | 值 |
|---|---|
| 串口 | **COM5**，ESP32-S3 板载 USB-Serial-JTAG（VID 303A:1001），console 就走它 |
| 板型 | 固件自报 `board=legacy (unverified)` —— **物理板型和宏的对应关系仍未核对** |
| Flash / PSRAM | 16 MB / QIO 80 MHz；PSRAM 8 MB 但只跑 **40 MHz** |
| CPU | **160 MHz**（不是 240 MHz；官方 esp_player 的 28 fps 是 240 MHz + 80 MHz 下的数字） |
| 内部堆 | 启动时空闲约 298 KiB |
| TF 卡 | 已挂载，`app: SD mount: ok`；索引文件在 `/.esp-story-index.bin`（还有 `.bak`、`.storybox-backup`、`.dirty`） |
| I2C | 总线活着，扫描到 `0x50`~`0x57`（MB85RC16 bank 别名；`0x50` 也可能是 EEPROM，所以只 ACK 不算数）；PN532 不在线（`NoDevice`） |
| 镜像我烧进去的最后一版 | `build-ui` `0x1FA310` / SHA256 `A4E636111788868E`（含 FRAM/UI/BLE 修复） |

工作约定（用户当晚提出）：**深夜不外放，不要改音量**。本轮 FRAM 测试临时把音量改成 7 验证持久化，
随后已恢复测试前的 `brightness=40 / rotation=3 / eq=vocal / volume=0`；之后没有播放音频。

## 4. 未结的 bug：设备列表乱码

**现象**：用户看到板子上的列表文字乱码（"设备列表的文字都乱码了"）。

**已做**（`f195b0d`）：根因之一是 ESP-IDF 的 FATFS 默认值 —— `CONFIG_FATFS_LFN_NONE=y` + `CODEPAGE_437`，
卡上的中文名在进字体前就被压成 8.3 短名并按 CP437 转码。`sdkconfig.defaults` 现在显式设置：

```
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_MAX_LFN=255
CONFIG_FATFS_API_ENCODING_UTF_8=y
CONFIG_FATFS_CODEPAGE_936=y     # 只为 8.3 短名兜底，码表约 +180 KB flash
```

**还差的一步**：卡上的索引缓存存的是旧（乱码）名字，`/.esp-story-index.bin` 不重写就不会变。
今晚顺手把设置页第 6 项「刷新列表」从占位接成了真扫描（`refreshCatalog()`：停播放 → 重扫 → 重写索引）。
所以下一步是让用户按一次 **设置 → 第 6 项「刷新列表」→ 单击 OK**，然后回音乐列表确认。

**如果刷新后仍然乱码，先分成两种可能**（排查方向完全不同）：

| 观察 | 结论 | 下一步 |
|---|---|---|
| 只有**文件名**乱，设置页「亮度/旋转/蓝牙广播/刷新列表」等**固定中文正常** | FATFS 那条路没走通 | 让卡插电脑看真实文件名是不是中文；确认卡是 FAT32 且名字是 LFN；抓扫描日志看 `app: catalog: N categories, M tracks`；确认 `/.esp-story-index.bin` 的修改时间变了（说明重扫确实落了盘） |
| 连**固定中文也乱** | **字体**问题，与 FATFS 无关 | 查 `src/fonts.cpp` / `src/fonts.h` 的 efontCN 字形表与 `Fonts::begin()`，看 16px 表是否真的包含这些码点；必要时写个主机小测试把码点渲染成位图 |

**抓日志**（不要用 `idf.py monitor` 卡住会话）：

```powershell
# 复位并从 ROM 启动日志开始抓 12 秒
& 'C:\Users\a1509\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' `
  tools\serial_capture.py COM5 12 115200 --reset
```

## 5. 今晚已修并验证的真机 bug

| # | 现象 | 根因 | 提交 |
|---|---|---|---|
| 1 | BLE 完全不工作（无广播/连不上） | TX 特征 `access_cb=nullptr`；NimBLE 5.5 的 `ble_gatts_chr_is_sane()` 对任何无回调特征都判非法 → `ble_gatts_count_cfg` 返回 `EINVAL`(3)，服务没注册 | `456d80d` |
| 2 | 每次启动重做 RF 校准 | 产品路径从未 `nvs_flash_init()`（只有 WiFi 服务里调过） | `456d80d` |
| 3 | 卡上中文名乱码 | FATFS 默认关长文件名 + CP437 | `f195b0d` |
| 4 | 设置页「刷新列表」是占位 | 从未接到 `refreshCatalog()` | `f195b0d` |
| 5 | 无 PN532 时每 3.5 s 刷 3 行 I2C NACK | 探测失败仍按 `PROBE_INTERVAL_MS` 重试 | `3a02810`（探测失败后 5 分钟才重试，14 秒抓包里 NACK 从 ~12 条降到 3 条） |

修复后同一块板：`GAP procedure initiated: advertise` + `BLE service 'StoryBox-CBD6' starting`，
`fram: legacy FRAM online (8 bank probes passed)`，无 phy/NVS 告警，SD 挂载正常，
`display: PSRAM canvas ready`。

## 6. 镜像（2026-11-04 第二轮，含 FRAM/UI/BLE 修复）

| 配置 | 构建目录 | `storybox.bin` | SHA256 前 16 位 | 用于 |
|---|---|---|---|---|
| 默认产品（BLE+NFC，单槽） | `build-ui` | `0x1FA310` | `A4E636111788868E` | **已在板子上**，本轮主镜像 |
| 产品 + USB 协议 | `build-usb-product` | `0x203540` | `96D65A0BA0D0BB67` | FRAM 写→重启→读验证（同时验证 BLE 名称） |
| 产品 + USB + OTA 双槽 | `build-ota-usb` | 未重建 | — | 第 10 组（**整片擦除**） |
| 深睡 | `build-power` | 未重建 | — | 第 9 组 |
| 确认 FRAM（编译覆盖） | `build-fram` | 未重建 | — | 现在默认已开 FRAM，这套只剩历史意义 |

`idf.py flash` 会先 build，`App version` 内嵌时间戳，所以每次烧录后哈希会漂移；上表是
本轮最后两次 build 的产物。工厂分区仍有 75% 余量。

## 7. 已知坑（累积，别再踩）

1. `sdkconfig.defaults` / 片段的改动**对已存在的 `build-*/sdkconfig` 不生效**。删掉对应 `sdkconfig` 再构建；
   注意删了以后会**从头重建**（1268 个目标，几分钟），而且新 sdkconfig 只由默认值生成，
   和旧的其余配置项差异要先 `diff` 确认（今晚就是这样才敢删）。
   **本轮 FRAM 开不起来就是这条**：改了 Kconfig 默认值还不够，5 个 `build-*/sdkconfig` 里的
   `# CONFIG_STORYBOX_FRAM_CONFIRMED is not set` 也必须改掉（或删 sdkconfig 重建）。
2. `idf.py -D` 短选项会被当成构建目标，必须用 `--define-cache-entry`。
3. `tools/idf/*.ps1` 带 Zone.Identifier，`powershell -File` 会被执行策略拦下，必须加 `-ExecutionPolicy Bypass`。
4. **`idf.py flash` 会先做一次 build**：这会改写 `storybox.bin`（`App version` 内嵌 git HEAD / 时间戳），
   **冻结表里的哈希会漂移**。要严格固定镜像就先 `build` 再 `flash`，并在记录里写清楚。
5. 组件管理器哈希：本机 `core.autocrlf=true` 会让 LovyanGFX 检出内容哈希不匹配 → `idf.py` 脚本已用 `GIT_CONFIG_*` 临时关掉。
6. 主机测试是 POSIX 语义：Windows 上 `file_reader`/`upload_transaction` 断言失败、`transfer_frame` 访问冲突、
   `media_library` 的非 ASCII 目录（`儿歌`）在 MinGW 下可能编码不一致导致 `nursery` 断言失败。
   能稳定跑的：`board_profile storage_access settings_record media_records parental_record parental_binding
   parental_config story_timeline pn532_frame render_math transfer_crc`。
7. 中文源码用 `write`/`edit` 工具改，不要 `Get-Content -Raw` + `Set-Content` 重写（按 ANSI 解码会损坏 UTF-8）。
8. **PowerShell 5.1 的 `Set-Content -Encoding utf8` 会写 BOM**，用它生成 `git commit -F` 的消息文件，
   commit 标题会带 U+FEFF。消息是纯 ASCII 时用 `-Encoding ascii`。
9. 抓串口别把 `Tee-Object` 的输出文件和脚本的 `outfile` 指到同一个路径（文件被锁 → `PermissionError`）。
10. `hostlink/route.h` 特意不含 `esp_err.h`；`Pn532::errorName` 只在本 .cpp 内定义（NFC 路由里别用）。
11. `sdkconfig.ota` 会整片擦除（单槽 → 双槽），FRAM 与 TF 卡不受影响。
12. **FRAM 探测会写 8 个 bank 的 `0xF8` 字节**（每 bank 最后一个字节，现有记录都没用到）。
    这是有意的：只 ACK 不算数，必须写读回；探测失败会关闭 FRAM 而不是继续写。
13. BLE 名字是 13 字节 `StoryBox-XXXX`，**不能和 128 位 UUID 同时塞进 31 字节 advertising**；
    名字必须走 scan response（本轮已这样改）。改回广播包会重现“小程序扫不到”。
14. `evt:state` 的 `mode/title/dur` 现在由 `setNowPlaying()` 和 `Mp3Stream::duration()` 提供；
    新增媒体入口时要记得设置，否则小程序仍看到上一首/`idle`。

## 8. 命令速查

```powershell
# 单配置构建 / 烧录 / 监视
powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 -B build-ui --define-cache-entry SDKCONFIG=build-ui/sdkconfig build
powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 -B build-ui --define-cache-entry SDKCONFIG=build-ui/sdkconfig -p COM5 flash
powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 -B build-ui --define-cache-entry SDKCONFIG=build-ui/sdkconfig -p COM5 monitor

# USB 版（第 6/7 组；也是 FRAM 写读复测用的镜像）
powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 -B build-usb-product --define-cache-entry SDKCONFIG=build-usb-product/sdkconfig --define-cache-entry "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.usb-product" -p COM5 flash

# FRAM 持久化复测（USB 版镜像；写 → 重启 → 读 → 恢复原值）
& 'C:\Users\a1509\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools\fram_check.py COM5
& 'C:\Users\a1509\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools\fram_check.py COM5 --restore

# 抓一段日志（自己退出）
& 'C:\Users\a1509\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools\serial_capture.py COM5 12 115200 --reset

# 主机测试（IDF_PATH + w64devkit 必需）
$env:IDF_PATH='C:\esp\v5.5.1\esp-idf'; $env:PATH='C:\tools\w64devkit\w64devkit\bin;'+$env:PATH
& 'C:\Program Files\Git\bin\bash.exe' -c "cd /c/Users/a1509/Documents/PlatformIO/Projects/music-demo && bash tools/test_host.sh"
```

## 9. 下一步（按优先级）

1. **三个界面目视确认**：音乐分类/歌曲列表、音乐播放器、音频故事播放器、视频列表/控制条。
   代码已按 `src/ui.cpp` 逐项恢复，但本轮只能编译 + 串口确认，没有屏幕像素证据；
   重点看：42px 行高、音符/文件夹/心形图标、封面卡、`03 / 24`、进度条黄端帽、播放按钮文字。
2. **小程序实测**（手机侧）：扫描应出现 `StoryBox-CBD6`（前缀 `StoryBox-`），
   `hello`/`bind`/`set childLock`/`evt:state` 的 `mode/title/dur` 与设备一致；
   本机已绑定的设备仍然 `bound:true`，首次绑定码在设置页第 9 项。
3. **收尾乱码这个 bug**：让用户按一次「刷新列表」并按第 4 节的两条路径判定；
   若属字体问题，主战场在 `src/fonts.cpp`（efontCN 字形表）。
4. 继续验收清单：第 1 组（显示/输入，纯目视）→ 第 5 组（BLE）→ 第 2/3 组（音乐/故事，
   **要等能出声的时段**）→ 第 4 组（视频，先量基线 fps/解码耗时）。
5. PN532 相关的第 7 组整体挂起（用户明确：先停，后续再调）。
6. 功能尾巴：USB `mkdir/remove/rename` + 容量字段、桌面端 OTA 上传界面、
   音视频漂移/丢帧统计、白名单逐项 UI + 独立锁定页。

## 10. 事实边界

- 所有尺寸来自编译产物，**以重新构建为准**；本轮 `build-ui` 最后产物 `0x1FA310`。
- 真机结论到：启动、FRAM 8 bank 探测、FRAM 写→重启→读、BLE MAC/服务名/广播启动、SD 挂载、媒体索引。
  **列表/播放器的像素、手机端扫描/绑定/童锁、播放出声、USB、NFC、电源、OTA 仍未在本轮验证。**
- 桌面端（`desktop/`）本轮未改；小程序源码不在本仓库，BLE 只做了协议源码对照 + 设备端广播日志确认。
