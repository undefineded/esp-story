# 小小故事盒

基于 ESP32-S3 和原生 ESP-IDF 的儿童音乐、故事与视频播放器。设备使用 320×240 ST7789 屏幕、TF 卡、I2S 功放、旋钮和按键；主页提供音乐、故事、视频、设置四个入口。

本分支的固件入口是 `main/app_main.cpp`，业务实现在 `components/`。仓库中的部分 `src/*.cpp`、PlatformIO 配置和旧工具保留作 Arduino 历史实现或被 IDF 组件复用；它们不代表当前播放器架构。

![视频剧场插画](docs/icons/video-theater.png)

## 功能与边界

- 音乐：扫描 `/music` 下的 MP3，按分类浏览、播放和调节音量/EQ。
- 故事：扫描 `/story` 下的音频故事和带 LRC 时间轴的图文故事，按进度显示 JPEG 插画。
- 视频：乐鑫 `esp_player` 播放 `/video` 下 320×240 MJPEG AVI，音频使用同一 AVI 内嵌 MP3。支持暂停、切换、返回和播放结束后重播。旧 QOI AVI、外挂音轨与 MP4/H.264 不可直接使用。
- 设备：NimBLE 控制、家长限制、设置、FRAM 持久化、Wi-Fi 和省电功能按编译配置和硬件条件启用。
- USB 与桌面端：产品 USB 配置使用 SBX4 协议，提供文件列表、会话、上传和下载等能力；不要假定旧版桌面端的全部文件操作均已迁移。PN532 原生服务支持状态查询，USB 原始读写与 NDEF 写入尚未接入。

编译通过不代表设备已验收；真实视频帧率、音画同步、SD/I2S/SPI 时序、USB/NFC 互操作和 OTA 回滚需要实机测试。CPU 负载目前未测量，USB 状态中的 `loadValid` 为 `false`。

## 硬件

| 部件 | 默认引脚 |
| --- | --- |
| ST7789 SPI | SCK 5、MOSI 4、DC 6、RST 7、CS 8、背光 9 |
| TF 卡 SDMMC 1-bit | CLK 12、CMD 11、DAT0 13 |
| I2S 功放 | BCLK 16、LRC 17、DOUT 18 |
| OK / NEXT / LAST | 47 / 48 / 39 |
| 编码器 A / B / SET | 14 / 15 / 21 |
| 霍尔 / I2C SDA / SCL | 40 / 1 / 2 |

板型选项和完整定义见 [src/config.h](src/config.h) 与 [components/board](components/board)。默认配置针对 16 MB Flash、八线 PSRAM；FRAM 和 PN532 共用 I2C，具体设备与引脚须按实际板卡确认。

## 构建与烧录

安装并启用 **ESP-IDF 5.5.3**，依照乐鑫的安装说明配置工具链和环境。首次构建需要访问组件注册表及 LovyanGFX Git 仓库；版本由 [main/idf_component.yml](main/idf_component.yml) 和 `dependencies.lock` 固定。

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

将 `PORT` 换成实际串口，例如 Linux 的 `/dev/ttyACM0` 或 Windows 的 `COM7`。默认配置使用 `sdkconfig.defaults` 和 `partitions.csv`；已有 `sdkconfig` 不会因为 defaults 更新而自动重置。

需要 USB 文件传输、OTA 和深睡时，使用独立构建目录。当前 OTA 双槽每槽 6 MiB（镜像约 2.6 MiB），另有 3.9 MiB `assets` 分区预留，尚未挂载：

```sh
idf.py -B build-product -D SDKCONFIG=build-product/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.usb-product;sdkconfig.ota;sdkconfig.power' build
idf.py -B build-product -p PORT flash
```

性能构建在上述 defaults 中加入 `sdkconfig.performance`（放在 `sdkconfig.defaults` 之后）；完整命令见 [播放器文档](docs/idf-video-player.md)。OTA 采用 `partitions.ota.csv` 双槽布局，转换分区布局前应备份 Flash 数据并按 [验收清单](docs/idf-hardware-acceptance.md) 检查烧录与恢复要求。深睡唤醒依赖 GPIO 14/15/21 的实际接线。USB 产品配置关闭普通 IDF 控制台，不能把二进制协议口当普通日志串口。

## TF 卡素材

```text
/music/儿歌/小星星.mp3
/story/音频故事/睡前童话/01-故事.mp3
/story/图文故事/月亮邮递员/月亮邮递员.mp3
/story/图文故事/月亮邮递员/01.jpg
/story/图文故事/月亮邮递员/timeline.lrc
/video/小星星.avi
```

`timeline.lrc` 示例：`[00:08.50]01.jpg | 故事文字`。故事素材可用 `tools/build_story.mjs` 转换。

视频转换需要 Node.js、ffmpeg 和 ffprobe：

```sh
node tools/build_video.mjs input.mp4
node tools/build_video.mjs --all-in ./素材 --out ./out-video --fps 15
node tools/verify_video.mjs ./out-video
```

默认输出 320×240、15 fps 的 MJPEG AVI 和内嵌 MP3；无声源生成静音音轨。旧素材需从原视频重新转码。转换通过不代表实机达到目标帧率。注意码率：1-bit TF 卡实测约 150–200 KB/s，`15 fps / quality 6` 约 150 KB/s 只适合快卡；慢卡用 `--fps 10 --quality 12`（约 75 KB/s），否则音频会被读卡拖出间歇断供。工具会打印平均码率并在超过 100 KB/s 时警告。

音乐和故事使用 SD 根目录的媒体索引；视频列表独立扫描并按存储版本缓存。USB 写入会使索引失效；直接在电脑改卡后，音乐/故事可在设置中刷新，视频列表的外部改动未改变设备内版本时需重启。容量限制和索引恢复见 [媒体索引](docs/media-index.md)。

## 架构

```text
main/                 IDF 启动、配置选择与诊断入口
components/app/       页面、输入事件、业务协调与家长控制
components/audio/     MP3、EQ、I2S 输出和音频任务
components/video/     esp_player 控制任务、帧邮箱和视频目录
components/display/   显示适配；src/display.cpp 实现画布和 DMA 提交
components/storage/   SD 访问租约、文件会话与上传事务
components/media/     音乐/故事目录与索引缓存
components/ble/       NimBLE 控制；components/hostlink/ 为 USB 协议
components/nfc/       原生 PN532 服务；board/settings/power/ota 管理外设与状态
desktop/              Electron 桌面端，具体可用功能取决于固件能力
tools/                素材转换、资源生成和回归脚本
docs/                 当前指南、真机验收、性能待办和历史迁移记录
```

视频控制任务独占官方播放器；视频/音频交接通过异步请求与存储租约协调。UI 拥有画布和 LCD，视频解码结果经最新帧邮箱交给 UI。具体任务分核、资源释放和统计口径见 [IDF 视频播放器](docs/idf-video-player.md)。

## 测试与桌面端

```sh
bash tools/test_host.sh
IDF_PATH=/path/to/esp-idf bash tools/test_mp3_stream.sh
IDF_PATH=/path/to/esp-idf bash tools/test_native_protocol.sh
bash tools/test_video_conversion.sh
```

音频测试依赖已下载的组件和已生成的 `build/config`；转换测试依赖 ffmpeg/ffprobe。CI 还编译默认、产品、性能与诊断配置，见 [.github/workflows/esp-idf.yml](.github/workflows/esp-idf.yml)。

桌面端要求 Node.js 20+：`cd desktop && npm install && npm run dev`。演示模式和打包见 [desktop/README.md](desktop/README.md)；演示功能不能证明当前 IDF 固件已支持相同操作。

## 文档导航

- [视频播放与性能指标](docs/idf-video-player.md)、[性能待办](docs/PERFORMANCE-TODO.md)
- [显示刷新](docs/display-refresh.md)、[主页与插画资源](docs/home-design.md)、[媒体索引](docs/media-index.md)
- [USB 文件传输](docs/file-transfer-refactor.md)、[PN532 与桌面端](docs/pn532-i2c.md)
- [真机验收清单](docs/idf-hardware-acceptance.md)、[验收记录](docs/idf-acceptance-log.md)
- [文档索引与历史记录](docs/README.md)
