# 小小故事盒

基于 ESP32-S3 的儿童音乐播放器和故事机固件。设备使用 320×240 ST7789 屏幕、TF 卡、MAX98357A I2S 功放、EC11 旋钮和按键，带音乐 / 故事 / 童谣 / 设置主页入口，支持音乐分类播放、童谣汇总播放、故事插画跟随音频进度切换、亮度/旋转/EQ 设置。

当前版本新增 **NXP PN532 I2C 读卡** 与配套的 **Electron 桌面工作台**（`desktop/`）：可以检测读卡模块是否插入、实时展示射频卡信息，并把文本 / 链接写入 NTAG 标签。工作台还提供 **Windows 资源管理器风格的 TF 卡文件管理**，支持拖放 / 粘贴上传文件或文件夹。协议与接线见 [docs/pn532-i2c.md](docs/pn532-i2c.md)。

## 功能

- 主页：音乐 / 故事 / 童谣 / 设置一排四个 60×60 图标，选中放大到 72×72（无阴影）；名称以小号文字显示在图标下方，旋钮或 NEXT/LAST 移动焦点，OK 进入；开机固定回到主页。
- 音乐模式：按分类列表浏览 MP3（每页 4 行），进入分类后播放曲目，支持暂停、上一首、下一首和音量调节。
- 故事模式：`/story` 下的故事文件夹以列表展示（每页 4 行）；读取故事目录中的 MP3、插画和 `timeline.lrc`，按播放进度切换画面。
- 童谣：把分类名含“童谣 / 儿歌 / nursery / rhyme”的曲目汇总成一个播放列表，主页一键进入。
- 童锁：小程序开启后锁定实体按键和旋钮，手机端播放/音量控制不受影响；设备顶栏显示“童锁”，OK+NEXT 同时按住 5 秒可在本地解除（防止手机不在身边时锁死）。
- 设置：亮度、横屏方向、EQ、“刷新列表”，以及蓝牙广播 / WiFi / 热点 AP 三个无线开关。
- efont 中文点阵字体：标题和正文 16px，故事标题 14px，辅助信息 12px，按原生尺寸绘制。
- PSRAM 画布和局部提交：只把变化区域发送到屏幕，减少刷新闪烁和 SPI 传输。
- SD 媒体索引：首次启动或手动刷新时扫描文件，之后启动和页面切换直接读取索引。
- 文件管理（上位机）：浏览 TF 卡、拖放/粘贴上传文件或文件夹、下载、新建文件夹、重命名、删除；支持复选框多选、批量移动和拖拽移动；上传/下载显示传输进度，移动/删除显示任务进度；上传到 `/music` 后会自动重建媒体索引。

主页预览（320×240，2× 放大；一排四个，选中图标放大到 72×72，无阴影；由 `tools/render_home_preview.py` 生成）：

![主页预览](docs/home-preview.png)

图标原图在 `docs/icons/home-icons-source.png`（音乐/故事/童谣）和 `docs/icons/settings-source.png`（设置），用 `tools/build_home_icons.py` 切成 60×60 并生成 `src/home_icons.h`（RGB565）。

## 硬件

| 部件 | 接口/引脚 |
| --- | --- |
| ST7789 TFT | SCK 5，MOSI 4，DC 6，RST 7，CS 8，背光 9 |
| MicroSD | CLK 12，CMD 11，DAT0 13，1-bit SDMMC |
| MAX98357A | BCLK 16，LRC 17，DOUT 18 |
| OK / NEXT / LAST | 47 / 48 / 39 |
| EC11 编码器 | A 14，B 15，SET 21 |
| 霍尔开关 | 40 |
| I2C FRAM | SDA 1，SCL 2 |
| PN532 NFC | I2C SDA 1 / SCL 2，地址 0x24，3.3V |

完整引脚和设备限制见 [src/config.h](src/config.h)。当前限制为 300 首音乐、32 个音乐分类、32 个故事，每个故事最多 256 幅插画。

## NFC 读卡与上位机

固件通过 I2C 连接 PN532（与 FRAM 共用 SDA/SCL），并在 USB CDC 上提供换行分隔 JSON 协议，供 `desktop/` 下的 Electron 上位机使用。

- 模块检测：PN532 在线状态、固件版本、I2C 地址；拔出/插入会自动上报。
- 读卡：UID、ATQA/SAK、卡片类型、容量、NDEF 记录、原始 Page/Block。
- 写卡：文本 / 链接 / 自定义 NDEF，自动写 CC 与 TLV，支持逐页回读校验。
- 高级：Type 2 原始页读写；MIFARE Classic 原始块读写（Key A）。
- 上位机：概览 / 读卡器 / 写卡 / 卡片库 / 设备与连接 / 扩展中心 / 设置，内置无硬件演示模式。

![NFC Studio 概览](docs/nfc-studio-dashboard.png)

桌面端开发和打包：

```powershell
cd desktop
npm install
npm run dev        # 开发模式
npm run typecheck  # TypeScript 检查
npm test           # NDEF/卡片工具测试
npm run dist       # 生成 Windows NSIS 安装包
```

无硬件时可设置 `NFC_DEMO=1` 启动演示模式。上位机说明见 [desktop/README.md](desktop/README.md)，固件 I2C 驱动、串口协议和错误码见 [docs/pn532-i2c.md](docs/pn532-i2c.md)。

**本机启动方式**：安装包产物为 `desktop/dist/StoryBox-NFC-Studio-Setup-1.0.0.exe`，安装后从桌面/开始菜单快捷方式启动（快捷方式已带 `--no-sandbox`）。未安装时也可双击 [启动演示.cmd](desktop/启动演示.cmd) 或 [启动上位机.cmd](desktop/启动上位机.cmd)。当前受管环境里 Electron 必须带 `--no-sandbox` 才能启动，直接双击打包目录里的 exe 会在一闪而过后退出；安装器已处理快捷方式参数。

连接实体设备后的命令行冒烟测试（需要 `pyserial`）：

```powershell
python tools/nfc_smoke_test.py --port COM7 --text "月亮邮递员 · 第 03 集"
```

## 构建和烧录

需要 PlatformIO、Python 3 和可用的 PlatformIO 网络访问。

```sh
cd esp-story
python3 -m pip install --target /tmp/codex-platformio platformio
PYTHONPATH=/tmp/codex-platformio \
PLATFORMIO_CORE_DIR=/tmp/codex-platformio-core \
python3 -m platformio run
```

烧录和串口监视：

```sh
PYTHONPATH=/tmp/codex-platformio \
PLATFORMIO_CORE_DIR=/tmp/codex-platformio-core \
python3 -m platformio run --target upload

PYTHONPATH=/tmp/codex-platformio \
PLATFORMIO_CORE_DIR=/tmp/codex-platformio-core \
python3 -m platformio device monitor
```

固件目标为 `esp32-s3-devkitm-1`，串口速度为 115200，烧录速度为 921600。启动日志中应看到 `efontCN` 和 `PSRAM canvas: OK`。实际烧录需要连接 ESP32-S3 硬件；云端环境只能验证编译。

## TF 卡目录

音乐文件放在 `/music` 下。根目录中的 MP3 属于“全部歌曲”，一级子目录成为分类，子目录内的 MP3 会递归收录：

```text
/music/
  01.mp3
  1-2岁/
    小兔子.mp3
  3-5岁/
    森林.mp3
  童谣/
     小星星.mp3
     拔萝卜.mp3
```

童谣不需要单独目录结构：分类名包含“童谣”“儿歌”“nursery”或“rhyme”的文件夹（例如 `/music/童谣/`、`/music/儿歌/`）会自动汇总到主页的“童谣”入口；没有匹配分类时，入口会提示放文件的路径。

故事文件夹放在 `/story` 下，推荐使用故事构建脚本生成设备目录：

```text
/story/月亮邮递员/
  月亮邮递员.mp3
  01.jpg
  02.jpg
  timeline.lrc
```

`timeline.lrc` 的每行格式为：

```text
[00:00.00]01.jpg | 故事开始
[00:08.50]02.jpg | 月亮升起来了
```

可以使用 `tools/build_story.mjs` 将素材转换成 320×240 JPG、单声道 MP3 和设备时间轴。该脚本需要 Node.js 和 ffmpeg。

## 媒体索引

设备会在 SD 卡根目录生成 `/.esp-story-index.bin`。首次启动没有有效索引时扫描 `/music` 和 `/story`；以后直接加载索引，不再重复枚举目录。

增删、重命名或更换媒体后，在设置中选择“刷新列表”，单击 OK 重新扫描并保存索引。刷新期间会停止播放，请勿拔出 TF 卡。索引包含版本和校验值，并使用临时文件及备份文件更新。详细行为见 [docs/media-index.md](docs/media-index.md)。

## 操作

| 操作 | 主页 | 音乐分类 / 歌曲 / 故事列表 | 音乐播放页 | 故事播放页 | 设置页 |
| --- | --- | --- | --- | --- | --- |
| 旋转编码器 | 移动焦点 | 移动选择 | 调节音量 | 调节音量 | 调整当前项 |
| NEXT / LAST 短按 | 移动焦点 | 移动选择 | 下一首 / 上一首 | — | 上下选择 |
| NEXT / LAST 长按 | — | — | 歌曲列表 / 分类列表 | — | — |
| OK 单击 | 进入选中块 | 播放 / 进入 | 播放或暂停 | 暂停 / 继续 | 递增当前项 |
| OK 双击 | — | — | — | — | 保存并回主页 |
| OK 长按 | — | 收藏（仅歌曲列表） | 停止并回分类列表 | 停止并回故事列表 | — |
| SET 短按 | 进入选中块 | 播放选中项 | 播放或暂停 | 暂停 / 继续 | — |
| SET 长按 | — | 回主页 | 回主页（音乐继续） | 停止并回主页 | 保存并回主页 |

主页固定为“音乐 / 故事 / 童谣 / 设置”四块。原来的“OK 双击进设置”和“SET 长按切换音乐/故事模式”已取消：设置只能从主页进入，返回主页统一用 SET 长按（设置页同时也是保存）。

在设置页使用 NEXT/LAST 选择项目，旋钮或 OK 修改；双击 OK 保存并回主页，长按 SET 也可以保存并回主页。设置页共 8 项，右侧滚动条表示当前窗口位置。

### 无线开关

| 设置项 | 行为 |
| --- | --- |
| 蓝牙广播 | 开：设备广播 `StoryBox-XXXX`，小程序可扫描/连接；关：停止广播（已建立的连接不受影响） |
| WiFi | 开：启用 WiFi STA，使用 `src/config.h` 中的 `WIFI_STA_SSID` / `WIFI_STA_PASS` 连接；未配置时只打开射频 |
| 热点 AP | 开：启动 SoftAP，SSID `StoryBox-XXXX`，密码 `storybox123`，默认地址 `192.168.4.1` |
| 显示绑定码 | 在屏幕上显示 6 位首次配对码，双击 OK 返回 |

WiFi 与 AP 可同时开启（AP+STA）。三个无线开关都会写入 FRAM，重启后保持。默认：蓝牙广播开，WiFi/AP 关。

首次绑定码保存在 FRAM，未绑定设备开机时显示 4 秒；手机连接时也会自动切到绑定码页，方便在小程序里输入。绑定成功后不再显示。

## 项目结构

```text
src/display.*       ST7789、PSRAM 画布和局部提交
src/ui.*            页面绘制和 UI 状态缓存
src/fonts.*         efont 点阵字体选择
src/player.*        ESP32-audioI2S 和音频 FreeRTOS 任务
src/storage.*       音乐列表和分类
src/story.*         故事目录、时间轴和插画切换
src/catalog.*       SD 媒体索引加载、扫描和保存
src/parental.*      家长控制、用量、音量范围、白名单、PIN、绑定
src/ble.*           BLE GATT 服务、JSON 协议、广播开关
src/network.*       WiFi STA / SoftAP 开关
src/pn532.*         PN532 I2C 驱动：帧校验、寻卡、Type 2 页 / Classic 块读写
src/nfc.*           NFC 业务层：卡片模型、NDEF 写入状态机、事件队列
src/hostlink.*      USB CDC 上位机 JSON 协议（请求/响应/事件）
src/home_icons.h    主页 60x60 RGB565 图标（由 tools/build_home_icons.py 生成）
desktop/            Electron + React + TypeScript 上位机
tools/              故事、时间轴、字体/主页图标切图和 BLE 冒烟测试
test/               主机端索引及渲染逻辑测试
docs/               刷新、索引、字体对比、PN532 协议和上位机截图
```

## 验证

固件构建：

```sh
PYTHONPATH=/tmp/codex-platformio \
PLATFORMIO_CORE_DIR=/tmp/codex-platformio-core \
python3 -m platformio run
```

主机端逻辑测试：

```sh
g++ -std=c++11 -Wall -Wextra -pedantic -fsanitize=address,undefined \
  test/render_math_test.cpp -o /tmp/esp-story-render-test
/tmp/esp-story-render-test

g++ -std=c++11 -Wall -Wextra -pedantic -fsanitize=address,undefined \
  -Itest/catalog_host -Isrc src/catalog.cpp test/catalog_host/test_catalog.cpp \
  -o /tmp/esp-story-catalog-test
/tmp/esp-story-catalog-test
```

NFC 上位机：

```powershell
cd desktop
npm install
npm run typecheck
npm test
npm run build
```

当前已验证固件编译、上位机类型检查、NDEF 单元测试、Electron 演示模式端到端流程和 Windows 打包启动；真实 PN532 的 I2C 时序与标签写入仍需连接硬件验收。

显示刷新方案和硬件验收项目见 [docs/display-refresh.md](docs/display-refresh.md)；字体方案对比见 [docs/font-comparison.html](docs/font-comparison.html)。

## 当前限制

- JPEG 解码和媒体刷新仍在主任务中执行，故事切图的实际流畅度需要硬件验证。
- PSRAM 画布减少了中间态和无效传输，但不保证整屏切换完全无撕裂。
- 媒体索引不会自动发现新增文件，修改 SD 卡内容后必须手动刷新。
- 主页四个图标均已替换为用户提供的资源，图标素材和切图脚本随项目保存。
- 开机固定回主页，不再自动载入并暂停上次曲目；音量、上次音乐分类的光标仍会恢复。
- 童谣入口按分类名匹配，根目录散放的 MP3 不计入童谣；需要把儿歌放进带“童谣/儿歌”字样的分类文件夹。
- 故事页目前把 `/story` 的一级子文件夹视为故事文件夹（每个文件夹里放一个故事）；暂不支持“分类 / 故事”两级目录。
- 没有连接实体硬件时无法验证屏幕颜色、按键方向、音频爆音和真实帧率。
- PN532 驱动与上位机协议已完成编译和模拟验证；真实 I2C 时序、不同 NTAG 容量、Classic 密钥与写入回读需要现场硬件验收。
- PN532 目前只支持 ISO 14443A；MIFARE Classic 仅支持原始块读写，不支持 NDEF/MAD；DESFire 等 14443-4 卡片仅识别 UID。
- I2C 与 FRAM 共用总线，NFC 操作全部在主循环串行执行；写入时按页分步，避免长时间阻塞 UI。
