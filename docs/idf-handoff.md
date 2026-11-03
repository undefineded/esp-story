# 交接文档：esp-story ESP-IDF 迁移

> **写于 2026-11-04 凌晨（第一块板子上机当晚）**。新会话请从头读完这一页：
> 第 4 节是**未结的 bug**，第 8 节是可复制的命令。
> 批次细节见 `docs/idf-migration-progress.md`，逐组真机结论见 `docs/idf-acceptance-log.md`。

## 1. 一句话状态

代码面（P1–P6 + BLE 家长命令）全部铺开、**五套配置编译通过、已全部推到 origin**（`feature/esp-idf-migration`，与 origin 同步）。
今晚第一次上真机，1.4 秒的启动日志就否掉了三条「编译通过」的假设，**已修 4 组真机 bug**（BLE 完全不工作、NVS 从未初始化、
卡上中文名全乱码、NFC 每 3.5 秒刷 I2C NACK）。
**未结**：设备列表乱码是否被 FATFS 修复彻底解决，仍缺一次目视确认（第 4 节）。
验收 10 组里只有第 0 组（上电冒烟）算过。

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
| I2C | 总线活着，扫描到 `0x50`~`0x57`（FRAM/EEPROM 区），**没有烧过**；PN532 不在线（`NoDevice`） |
| 镜像我烧进去的最后一版 | `build-ui` `0x1F6430` / SHA256 `B4BC9C45B8165151`（含下文全部修复） |

工作约定（用户当晚提出）：**深夜不外放，不要改音量**。当前音量只在 RAM（`build-ui` 没有确认 FRAM），没有写过 FRAM。

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

修复后同一块板：`GAP procedure initiated: advertise` + `BLE service 'StoryBox' starting`，
无 phy/NVS 告警，SD 挂载正常，`display: PSRAM canvas ready`。

## 6. 镜像（今晚最后一轮，全部含 5 个真机修复）

| 配置 | 构建目录 | `storybox.bin` | SHA256 前 16 位 | 用于 |
|---|---|---|---|---|
| 默认产品（BLE+NFC，单槽） | `build-ui` | `0x1F6430` | `B4BC9C45B8165151` | 第 0/1/2/3/4/5/7/8 组，**已在板子上** |
| 产品 + USB 协议 | `build-usb-product` | `0x1FF5D0` | `DC66200524D1D504` | 第 6 组、NFC 状态 |
| 产品 + USB + OTA 双槽 | `build-ota-usb` | `0x1FFB00` | `A3CA8A57A965E354` | 第 10 组（**整片擦除**），槽余量约 33% |
| 深睡 | `build-power` | `0x1F8F10` | `6F4412EA06FA68EE` | 第 9 组 |
| 确认 FRAM（编译覆盖） | `build-fram` | `0x1F8BD0` | `60666607B90DCB65` | **不要烧** |

比 FATFS 修复前每套约 +180 KB（CP936 码表）。工厂分区仍有 75% 余量，OTA 每槽从 39% 降到约 33%。

## 7. 已知坑（累积，别再踩）

1. `sdkconfig.defaults` / 片段的改动**对已存在的 `build-*/sdkconfig` 不生效**。删掉对应 `sdkconfig` 再构建；
   注意删了以后会**从头重建**（1268 个目标，几分钟），而且新 sdkconfig 只由默认值生成，
   和旧的其余配置项差异要先 `diff` 确认（今晚就是这样才敢删）。
2. `idf.py -D` 短选项会被当成构建目标，必须用 `--define-cache-entry`。
3. `tools/idf/*.ps1` 带 Zone.Identifier，`powershell -File` 会被执行策略拦下，必须加 `-ExecutionPolicy Bypass`。
4. **`idf.py flash` 会先做一次 build**：这会改写 `storybox.bin`（`App version` 内嵌 git HEAD / 时间戳），
   **冻结表里的哈希会漂移**。要严格固定镜像就先 `build` 再 `flash`，并在记录里写清楚。
5. 组件管理器哈希：本机 `core.autocrlf=true` 会让 LovyanGFX 检出内容哈希不匹配 → `idf.py` 脚本已用 `GIT_CONFIG_*` 临时关掉。
6. 主机测试是 POSIX 语义：Windows 上 `file_reader`/`upload_transaction` 断言失败、`transfer_frame` 访问冲突。
   能跑的：`board_profile storage_access settings_record media_records parental_record parental_binding
   parental_config story_timeline pn532_frame render_math transfer_crc`。
7. 中文源码用 `write`/`edit` 工具改，不要 `Get-Content -Raw` + `Set-Content` 重写（按 ANSI 解码会损坏 UTF-8）。
8. **PowerShell 5.1 的 `Set-Content -Encoding utf8` 会写 BOM**，用它生成 `git commit -F` 的消息文件，
   commit 标题会带 U+FEFF。消息是纯 ASCII 时用 `-Encoding ascii`。
9. 抓串口别把 `Tee-Object` 的输出文件和脚本的 `outfile` 指到同一个路径（文件被锁 → `PermissionError`）。
10. `hostlink/route.h` 特意不含 `esp_err.h`；`Pn532::errorName` 只在本 .cpp 内定义（NFC 路由里别用）。
11. `sdkconfig.ota` 会整片擦除（单槽 → 双槽），FRAM 与 TF 卡不受影响。

## 8. 命令速查

```powershell
# 单配置构建 / 烧录 / 监视
powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 -B build-ui --define-cache-entry SDKCONFIG=build-ui/sdkconfig build
powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 -B build-ui --define-cache-entry SDKCONFIG=build-ui/sdkconfig -p COM5 flash
powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 -B build-ui --define-cache-entry SDKCONFIG=build-ui/sdkconfig -p COM5 monitor

# USB 版（第 6/7 组）
powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 -B build-usb-product --define-cache-entry SDKCONFIG=build-usb-product/sdkconfig --define-cache-entry "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.usb-product" -p COM5 flash

# 抓一段日志（自己退出）
& 'C:\Users\a1509\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' tools\serial_capture.py COM5 12 115200 --reset

# 主机测试（IDF_PATH + w64devkit 必需）
$env:IDF_PATH='C:\esp\v5.5.1\esp-idf'; $env:PATH='C:\tools\w64devkit\w64devkit\bin;'+$env:PATH
& 'C:\Program Files\Git\bin\bash.exe' -c "cd /c/Users/a1509/Documents/PlatformIO/Projects/music-demo && bash tools/test_host.sh"
```

## 9. 下一步（按优先级）

1. **收尾乱码这个 bug**：让用户按一次「刷新列表」并按第 4 节的两条路径判定；
   若属字体问题，主战场在 `src/fonts.cpp`（efontCN 字形表）。
2. 继续验收清单：第 1 组（显示/输入，纯目视）→ 第 5 组（BLE：广播已通，验连接/hello/state/新命令）
   → 第 2/3 组（音乐/故事，**要等能出声的时段**）→ 第 4 组（视频，先量基线 fps/解码耗时）。
3. PN532 相关的第 7 组整体挂起（用户明确：先停，后续再调）。
4. 功能尾巴（BLE 已齐，剩下）：USB `mkdir/remove/rename` + 容量字段、桌面端 OTA 上传界面、
   音视频漂移/丢帧统计、白名单逐项 UI + 独立锁定页。
5. 官方播放栈（`espressif/esp_player`）评估已归档：**1.1.1 在 IDF 5.5.1 上装不上，2.0.0 可以编译**，
   两个接入点（`esp_audio_render.out_writer`、`esp_video_render` 自定义 backend）都有官方口子，
   但**先要真机基线**，见 `docs/idf-player-stack-eval.md`。

## 10. 事实边界

- 所有尺寸来自编译产物，**以重新构建为准**；`build-ui` 今晚反复重建过多次，哈希只在验收记录里固定。
- 真机结论只到「启动 + 外设初始化 + BLE 广播」这一层：播放、显示内容、USB、NFC、家长、电源、OTA **都还没测**。
- 桌面端（`desktop/`）本轮只改过一处（`FileManagerPage` 接受三种能力）；小程序源码不在本仓库，BLE 线协议只做过源码级对照。
