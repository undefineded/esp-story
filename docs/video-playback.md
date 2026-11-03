# 视频播放（AVI + QOI / MJPEG）

主页第三个入口是“视频”，播放 TF 卡 `/video` 下的帧内编码视频。本文说明为什么选 QOI、固件怎么索引和播放、怎么把普通视频转过来，以及已知限制和实测数据。

## 软解最优格式：QOI，而不是 MJPEG

ESP32-S3 只有 240MHz 双核、没有 H.264/H.265 硬解，所以关键不是“压缩比”，而是**每帧软件解码要花多少 CPU**：

| 格式 | 每帧解码工作 | 320×240 估算 | 体积（卡通素材） |
| --- | --- | --- | --- |
| 原始 RGB565 | 纯 memcpy | ~1–2ms | 150KB/帧，SD 带宽吃不下 |
| **QOI** | 查表/差分/RLE，无熵编码 | **~3–6ms** | ~31KB/帧 |
| PNG | zlib 解压 + 滤波 | ~15–30ms | ~10–30KB/帧 |
| MJPEG (4:2:0) | Huffman + IDCT + 色度上采样 | ~20–35ms | ~7KB/帧 |

JPEG 的耗时大头是 Huffman 和 IDCT：一帧 320×240 要解 1800 个 8×8 块；QOI 每个像素只有一次查表或几次加减，没有变长码表，也没有浮点/定点 IDCT。体积优先级放低之后，QOI 是唯一既"近乎零解码"、又能让 1-bit SD（约 1.5–2MB/s）跟得上的选择：

- 12fps × 31KB/帧 ≈ 0.37MB/s
- 20fps × 31KB/帧 ≈ 0.62MB/s
- 变成长片也不会把卡读爆

代价是体积：同一段卡通素材 MJPEG 平均 6.9KB/帧，QOI 31.3KB/帧（约 4.5 倍），6 秒片段 0.5MB → 2.2MB。转换脚本默认 QOI，`--codec jpeg` 可以退回 MJPEG。

> **MP4 / H.264 为什么不能直接播**
>
> MP4 是容器，真正决定解码开销的是里面的 H.264。它在手机/电脑上是"首选"，是因为那些设备有硬件解码器；ESP32-S3 没有。Espressif 自己的 [`esp_h264`](https://developer.espressif.com/blog/2025/07/esp-h264-use-tips/) 软件解码器在 S3 上的官方上限是 **320×240@19fps**（约 52ms/帧，纯解码，不含 MP4 解封装、YUV→RGB565 和推屏），比 MJPEG（约 30ms）还慢，比 QOI（约 5ms）慢一个数量级。
>
> 而且 H.264 有参考帧依赖：落后时不能像帧内格式那样直接跳到当前时刻，只能按顺序解，越落越远；还需要 2~3 帧 YUV420 参考帧（约 350KB PSRAM）、MP4 解封装（moov/stbl）和 AAC 音轨解码。真机上大概只能到 8~10fps，还得多写一个 IDF 组件和容器解析，收益为负。
>
> 所以 MP4 留在电脑上做**素材源**（`tools/build_video.mjs` 直接吃 mp4/mov/mkv），设备端只存转好的帧内文件——这也是绝大多数 MCU 播放器的做法（类似把 FLAC 转成 MP3 再放）。要原生播 MP4，正确做法是换带硬件 H.264 的芯片（ESP32-P4 可 1080P@30 硬解），而不是在 S3 上加软解。

## 目录与文件

```text
/video/
  小星星.avi      QOI 或 MJPEG 帧, 320x240, 固定帧率
  小星星.mp3      音轨（同名前缀，可选 .wav）
```

- 视频列表只认 `.avi` / `.mjpg` / `.mjpeg`，按文件名排序，最多 64 个（`MAX_VIDEOS`）。
- 音轨按同名查找：`小星星.avi` 找 `小星星.mp3`，没有就找 `.wav`；都没有则静音播放。
- `/video` 不进 `/.esp-story-index.bin`：进入视频列表时重新扫描目录，换视频不用“刷新列表”。

## AVI 结构与固件的索引方式

```text
RIFF('AVI ')
  LIST('hdrl')
    avih            总帧数 / 每帧微秒数 / 宽高
    LIST('strl')
      strh          fccType='vids', fccHandler='QOI ' 或 'MJPG', dwScale/dwRate = 帧率
      strf          BITMAPINFOHEADER, biCompression='QOI ' 或 'MJPG'
    LIST('strl')    （可选）音轨 'auds'
  LIST('movi')
    '00dc' 帧1 | '00dc' 帧2 | ...           每帧一个 QOI 或 JPEG 数据块
    '01wb' ...                              内嵌音轨（固件忽略）
  idx1              帧索引（固件不用）
  SBXI              本项目转码时写入的帧索引（固件优先用它）
```

打开视频时（`Video::open()`）：

1. 校验 `RIFF/AVI`，遍历顶层 chunk，读 `hdrl` 里的帧率、宽高和 `biCompression`，只接受 `QOI `/`MJPG`；
2. **优先读 `SBXI` 索引**：转码时已把每帧的块头偏移和长度写进文件，一次 15KB 的顺序读就能建好索引；没有这个 chunk（旧文件、ffmpeg 直接生成的文件）才退回去逐帧扫 `movi`——1966 帧就要 1966 次 seek+读，正是之前"准备阶段要等几秒"的原因；
3. 不信任 `idx1`：不同封装器的偏移基准不一致，自己扫一遍更稳，顺带算出真实帧数和时长；
4. 单帧超过 192KB（`MAX_VIDEO_BYTES`）会读不进帧缓冲，转换脚本会提前拦住。

`tools/build_video.mjs` 的 QOI 模式会自己写 AVI（Node 逐帧写 `movi`，收尾回填尺寸和帧数），所以不需要 ffmpeg 支持 QOI 封装；MJPEG 模式则直接交给 ffmpeg。

## 播放与同步

按固件坐标绘制的界面示意（2× 放大，非实机截图）：320×240 全屏画面，左上角是起播/暂停时显示的片名，底部 30px 是控制条（暂停键、进度、时间），右上角是实际交付帧率角标（落后丢帧时显示 `-N`，暂停时归零）。

![视频播放界面示意](video-player-preview.png)

`Video::loop()` 每轮主循环调用一次：

- **时间轴**：第 i 帧计划在 `i × frameUs` 显示，`VideoTiming::frameAt(elapsed)` 直接算出当前应显示的帧号。解码/推屏慢了就**丢帧跳到目标帧**，不会越播越慢（`src/video_timing.h` 有主机端单测）。
- **A/V 同步**：起播时先解出第 0 帧显示，等音轨真正开始（`Player::connected() && !paused`）再启动计时；音轨打不开时最多等 1.5 秒后静音开始。
- **暂停**：画面计时和音轨同时暂停/恢复，`Video::pause()` 会把计时起点整体后移，恢复后不会“跳一段”。
- **绘制**：QOI/JPEG 解码直接画进 PSRAM 画布，再叠底部控制条，由 `Display::present()` 差分推送。控制条在暂停时或操作后 4 秒内显示，片名只在起播 3 秒/暂停时显示。
- **结束**：帧用完即结束，列表里自动播下一个；最后一个放完回列表。

流水线预算（320×240）：读帧 5–10ms + QOI 解码 3–6ms + 推屏约 30–40ms（40MHz SPI）→ 每帧 45–55ms，理论上 18–22fps；MJPEG 则要多 20–35ms 解码，实际约 10–13fps。最终帧率以硬件实测为准。

换到 QOI 之后，下一道瓶颈就变成推屏和 SD 读取了：

- 推屏：`TFT_SPI_HZ`（`src/config.h`）默认 40MHz。模块和走线允许时可以试 `-DTFT_SPI_HZ=80000000`，全屏推送从约 30ms 降到约 16ms——比继续换编码的收益更大；花屏/丢像素就退回 40MHz。
- 读取：12fps 约 0.37MB/s、20fps 约 0.62MB/s（QOI 31KB/帧），1-bit SD 还有余量；但和音轨共用同一张卡，长片需要实机确认。
- 内存：帧缓冲 192KB + 帧索引 64KB 都在 PSRAM，静态 RAM 不受影响。

## 追求最高帧率时的取舍

先把"天花板"算清楚（320×240 RGB565 = 153,600 字节/帧）：

| SPI 频率 | 整屏传输 | 整屏更新的理论上限 | 现实目标（含开销） |
| --- | --- | --- | --- |
| 40MHz | 30.7ms | 32fps | 24–28fps |
| 80MHz | 15.4ms | 65fps | 40–50fps |

也就是说，**格式/编码只能把"读卡 + 解码"压到几毫秒；决定上限的是推屏**。按这个顺序优化收益最大：

1. **SPI 提到 80MHz**（`-DTFT_SPI_HZ=80000000`）：30ms → 16ms，单项最大收益。
2. **整帧 DMA + 双缓冲**：推第 N 帧的同时解码第 N+1 帧，把解码藏进 SPI 传输里；去掉现在 8px 条带逐条 `waitDMA` 的开销。
3. **QOI 直写画布**：跳过 LovyanGFX 的 BGRA 行缓冲和 `pixelcopy` 转换，直接写 sprite 的 swap565 缓冲，解码从 ~5ms 降到 ~2ms。
4. **只推变化区域**：`Display::present()` 已经按 8 行条带做差分，静态背景的动画实际只传变化部分，这是"超过 30fps"还能成立的原因；可以让条带更粗、合并相邻区域，减少 `pushImageDMA` 调用次数。

编码选择上：

- **QOI565（推荐）**：解码 3–6ms、31KB/帧，20fps 约 0.6MB/s。在 80MHz + 双缓冲下读卡+解码（~20ms）会略高于推屏（16ms），改动 2、3 之后能到 40–50fps。
- **差值/分块格式（最后一步）**：只存变化块（静态背景的动画常只有 10–30%），读卡降到几毫秒，解码就是 `memcpy` 到画布，能真正贴住 60fps 的 SPI 上限；代价是要自己写编码器和脏矩形合并。
- **不要用 MJPEG/H.264**：前者每帧 20–35ms 解码，后者官方上限 52ms/帧，都会让推屏优化白做。

所以"最高 fps"的答案是：**编码上选 QOI565（或更进一步的差值块），把余下的精力全投到 SPI 频率、整帧 DMA 和双缓冲上**；在这块板子上，320×240 整屏更新的物理上限是 40MHz 32fps / 80MHz 65fps，再往上只能换并行 RGB 屏或 ESP32-P4。

## 转码

需要电脑装好 Node.js 和 ffmpeg（`ffmpeg`/`ffprobe` 在 PATH 里）。

```powershell
node tools/build_video.mjs 小星星.mp4                  # 默认 QOI
node tools/build_video.mjs --all-in .\素材 --out .\out-video --fps 20
node tools/build_video.mjs 小星星.mp4 --codec jpeg     # 体积优先
node tools/verify_video.mjs .\out-video
```

脚本做的事：

1. `fps=N,scale=320:240:force_original_aspect_ratio=decrease,pad=...` 等比缩放并加黑边（`--scale-crop` 改成裁切填满）；
2. QOI 模式：ffmpeg 输出 `rgb24` 原始帧，Node 先量化到 RGB565（与屏幕完全一致，更省字节）再 QOI 编码，写入自建 AVI；
3. MJPEG 模式：`-c:v mjpeg -q:v 6 -pix_fmt yuvj420p -f avi`；
4. 源视频有音轨时抽成单声道 22.05kHz 48kbps MP3，没有音轨就删掉同名旧文件；
5. 用 `tools/lib/avi.mjs` 重新解析产物，核对编码、分辨率、帧数、单帧大小和音轨。

参数建议：

| 参数 | 建议 | 说明 |
| --- | --- | --- |
| `--codec` | `qoi`（默认） | 软解最快；`jpeg` 体积小但解码慢 |
| `--fps` | QOI 15~20 / JPEG 10~12 | 再高会被推屏或 SD 读取卡住 |
| `--quality` | 仅 JPEG，5~8 | ffmpeg `-q:v`，越小越清晰、文件越大 |
| `--width/--height` | 320×240 | 屏幕原生分辨率；其它尺寸会被裁切 |
| `--scale-crop` | 可选 | 竖屏素材用裁切填满更好看 |

## 自测

```powershell
node tools/selftest_video.mjs
```

覆盖 QOI 编解码往返（渐变覆盖 INDEX/DIFF/LUMA/RUN/RGB 分支，噪声测最坏体积）和 AVI 封装→解析往返（编码、帧数、帧率、每帧内容）。

## 已知限制

- 只支持帧内编码 AVI：QOI 或 MJPEG；MP4/H.264、HEVC、VP9 都必须先转码。
- QOI 是 3 通道无损格式，体积约为 MJPEG 的 3~5 倍；一张 32GB 卡按 20fps 大约能放 2~4 小时动画。
- 不支持拖动进度条、快进快退和字幕；只有暂停、上下一个、停止。
- 音轨必须是独立文件；AVI 内嵌音轨会被忽略（脚本默认不封装音轨）。
- 视频不参与家长白名单/故事时长统计，只看全局“播放中”状态（用于熄屏/休眠判断）。
- 帧缓冲和帧索引在 PSRAM：单帧 192KB、8000 帧，超限的素材需要降帧率或剪短。
- 与音频共用 SD 卡：视频读帧和音轨读取会交替访问同一张卡（故事插画的读图也是这个模式），高帧率长片需要实机确认是否卡顿。
