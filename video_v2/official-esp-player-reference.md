# 官方 ESP Player 示例参考与 Arduino 适配

## 文档定位

本文核对 `espressif/esp_player` 1.1.1 的官方说明与示例，作为现有 Arduino 工程实现单文件音视频播放的参考。没有下载、编译或运行官方示例，以下明确区分官方信息与本项目建议。

本项目当前目标：保留 ESP32-S3 与现有屏幕、SD 接线，优先验证单文件 AVI、MJPEG 视频加内嵌音轨能否稳定达到 15～20fps。此前 `README.md` 和 `transcoding-tool.md` 中的自定义差分格式保留为备选设计；其中独立 MP3 的交付方式不符合后来明确的单文件要求，不能直接作为最终实现规格。

## 官方来源

- [ESP Player 1.1.1 组件说明](https://components.espressif.com/components/espressif/esp_player/versions/1.1.1/readme)
- [官方示例列表](https://components.espressif.com/components/espressif/esp_player/versions/1.1.1/examples?language=en)
- [video_player 示例说明](https://components.espressif.com/components/espressif/esp_player/versions/1.1.1/examples/video_player?language=en)
- [audio_player 示例说明](https://components.espressif.com/components/espressif/esp_player/versions/1.1.1/examples/audio_player?language=en)
- [组件页面指向的固定版本源码](https://github.com/espressif/esp-gmf/tree/255fb531ddc97ff5e1e113dad97c88f70d6297be/packages/esp_player)

最后一个链接记录来源版本，本次未进一步审查其源码。后续核对解码器、任务实现和格式组合时应以固定版本为准。

## 官方性能信息及边界

组件说明列出的 ESP32-S3 环境为 CPU 240MHz、PSRAM 80MHz、Flash QIO；MJPEG 使用软件解码，320×240 最大性能为 28fps。组件支持 AVI、MJPEG、MP3，以及音频主时钟同步；最低要求 ESP-IDF 5.3。

来源：组件 README 的 Requirements、Key Features 和 Performance。

该页面没有明确说明视频成绩对应的显示接口、SD 总线模式、JPEG 编码参数，以及是否同步播放音频。因此：

- 28fps 不能直接视为本工程的整机播放保证。
- 也不能擅自将其解释为纯 JPEG 解码成绩。
- 组件分别支持容器和编码，不等于所有容器/音轨组合均已在本项目验证。
- 官方成绩支持优先验证 MJPEG 的决策，不足以替代真机测量。

## video_player 示例

### 官方说明摘要

示例通过板级管理初始化 SD、LCD 和音频硬件，由播放器负责解复用、解码和同步，再分别输出音频和视频。

默认媒体为 `/sdcard/test.mp4`，配置启用 H.264 与 AAC。测试 AVI/MJPEG 时，需要在 `menuconfig` 中启用 AVI extractor、软件 MJPEG 解码及匹配的音频解码器，并修改 `video_player_setup.h` 中的播放路径。

示例参考板是 ESP32-S3-Korvo-2。其文档列出的支持环境为 IDF 5.4.3+ 的 release/v5.4 分支或 5.5.2+ 的 release/v5.5 分支，不能与组件最低 IDF 版本混为一谈。

来源：video_player 示例说明的 Supported Media Formats、Environment Setup 和 Project Configuration。

### 独立验证入口

以下命令用于已配置好 ESP-IDF 的独立验证环境，不是在当前 Arduino 工程中直接运行的迁移步骤：

```bash
idf.py create-project-from-example "espressif/esp_player=1.1.1:video_player"
```

进入生成的示例目录，在已激活的 IDF Python 环境中安装板级辅助工具，再列出和选择板卡：

```bash
pip install esp-bmgr-assist
idf.py bmgr -l
idf.py bmgr -b esp32_s3_korvo_2_3
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

板卡选择命令仅适用于对应参考板。现有自制板不能直接套用该配置，需要匹配 LCD、SD 和音频设备。示例源码目录为 `packages/esp_player/examples/video_player`。

来源：video_player 示例说明的 Build and Flash。

## audio_player 示例的用途

[audio_player](https://components.espressif.com/components/espressif/esp_player/versions/1.1.1/examples/audio_player?language=en) 可作为单独验证音频输出的入口：默认从 SD 播放 MP3，默认启用 MP3/AAC 解码，并通过音频渲染层输出 PCM。

对本项目的建议：先确认音频输出稳定，再做音视频并发测试，便于区分音轨解析、解码与 I2S 输出问题。该步骤是验证策略，不代表已确认官方音频层可直接替换当前库。

## 当前 Arduino 工程的适配范围

以下为基于现有工程的设计判断，不是官方提供的 Arduino 接入教程。

当前 `platformio.ini` 使用 `framework = arduino`、LovyanGFX 和 ESP32-audioI2S 2.0.6。配置没有固定平台版本，仅凭该文件不能确定实际 Arduino Core 和底层 IDF 版本；接入前必须检查真实安装版本与组件依赖。

| 范围 | 当前状态 | 实现单文件播放需要补齐 |
| --- | --- | --- |
| AVI 解析 | 已读取视频帧，忽略内嵌音轨 | 按实际流编号识别音视频块，解析音频格式与时间信息 |
| 文件读取 | 视频与同名音轨分别读取 | 一个读取入口解复用到有界音视频队列 |
| JPEG 解码 | LovyanGFX `drawJpg()` | 测量实际速度，必要时对比其他解码路径 |
| 音频 | `connecttoFS()` 自行读文件 | 核对外部压缩数据输入能力，适配或替换输入层 |
| 显示 | 8 行暂存缓冲，每次 DMA 后等待 | 验证两个暂存缓冲交替发送与异步生命周期 |
| 同步 | 视频独立计时，起播检查文件连接状态 | 明确音频起播确认、播放进度及暂停恢复协议 |

不能把完整 AVI 交给只期望 MP3 字节流的音频接口。文件解复用后，才能把连续的音频压缩数据交给对应解码器。

## 两条可选接入路线

### 路线 A：保留 Arduino，借鉴官方流水线

沿用当前应用层与硬件驱动，新增或改造 AVI 解复用、缓冲队列、同步及显示调度。JPEG 解码器与音频输入适配分别验证，不预设当前库足够快或支持所需接口。

建议先用该路线评估改动范围。它不需要先把整个工程迁移到 IDF，但也不能宣称复制官方示例即可获得官方性能。

### 路线 B：兼容的 IDF 工程承载 Arduino 与 ESP Player

评估 Arduino 作为 ESP-IDF 组件运行，使用满足官方依赖的 IDF 与匹配的 Arduino 版本。需要处理构建系统、板级初始化、显示后端、音频输出及 I2S 驱动冲突。

这条路线可以保留部分 Arduino 业务代码，但属于构建和媒体栈调整。未经编译验证，不能承诺修改 `lib_deps` 就能接入。

## 单文件 MJPEG 测试素材

下面是本项目建议的 ffmpeg 测试命令，不是官方给出的性能测试素材，也未在本次执行：

```bash
ffmpeg -i input.mp4 -map 0:v:0 -map '0:a:0?' \
  -vf "fps=20,scale=320:240:force_original_aspect_ratio=decrease,pad=320:240:(ow-iw)/2:(oh-ih)/2" \
  -c:v mjpeg -q:v 6 -pix_fmt yuvj420p \
  -c:a libmp3lame -ac 1 -ar 22050 -b:a 48k \
  -f avi output.avi
```

输出一个 AVI，包含 MJPEG 视频和可选 MP3 音轨；没有源音轨时只含视频。15fps 档位修改滤镜中的帧率。先用 ffprobe 核对输出，再验证目标解析器是否正确处理 AVI 内的 MP3、交错布局和索引。

旧固件会忽略这里的内嵌音轨，因此不能用旧播放器播放有画面无声音来判断文件封装失败。

## 建议验证顺序

1. 固定工具链版本，确认 CPU、PSRAM、SD 和 SPI 配置。
2. 在当前 Arduino 工程测量 JPEG 解码到 RGB565 的耗时，暂不推屏。
3. 测量整屏 DMA 传输及真实 AVI 顺序读取速度。
4. 接入内嵌音频，验证输入接口、缓冲容量和音频欠载。
5. 联合测试 15/20fps，记录实际显示帧间隔 P95/P99、跳帧、音画偏差和内存峰值。
6. 用静态动画、全屏运动和频繁切镜素材持续播放，比较串行与流水线方案。

目标是确认整机稳定性。平均 FPS 或官方最大帧率不能替代长时间音视频同步播放的结果。

## 本次范围

仅新增官方参考与适配说明，未更改框架、依赖、固件或现有转码工具。后续若选择 MJPEG 单文件路线，应再统一修订总体方案与转码工具设计。
