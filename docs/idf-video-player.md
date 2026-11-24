# IDF 官方视频播放器迁移

当前实现使用 ESP-IDF **5.5.3**、`espressif/esp_player` **1.1.1**，依赖锁定在
`dependencies.lock`。此前 5.5.1 的依赖冲突不能靠忽略版本约束解决；CI 同步升级补丁版本。
已有本地 `sdkconfig` 的使用者应在 menuconfig 核实下面的频率和播放器配置，defaults 不会覆盖已保存值。

## 数据通路和任务归属

| 工作 | CPU 核心 | 优先级 |
|---|---|---|
| SD / AVI 解复用 | 0 | 4 |
| MP3 解码 | 0 | 5 |
| 音频渲染 / I2S 写入 | 0 | 6 |
| MJPEG 解码 | 1 | 5 |
| 视频渲染 / 最新帧缓冲 | 1 | 4 |
| UI、按键、画面合成、LCD SPI 提交 | 0 | 3 |
| 视频控制：打开、关闭、暂停、音频交接 | 0 | 3 |

上述五个 GMF 任务使用内部 RAM 栈。默认 CPU 240 MHz、八线 PSRAM 80 MHz。
不改动屏幕接线、SPI 40 MHz 频率或功放使能引脚。

官方播放器负责文件读取、解复用、解码、暂停/恢复及音视频同步。播放器进度直接来自
官方时钟，不再用 UI 墙钟除以整数帧间隔。GMF 后端只写 RGB565 缓冲，**不会操作 LCD 或 UI 画布**。
一个有锁的最新帧缓冲隔离生产者和消费者：UI 慢时替换尚未消费的旧帧，不积压队列。
UI 在锁保护下直接把最新帧画入画布，取消原来页面独立的 150 KiB 消费缓冲。
后端帧缓冲和最新帧共约 300 KiB PSRAM，另有解码器、画布与官方内部缓冲开销。
显示使用两个各 5 KiB 的内部 DMA 暂存区，准备下一条带时允许上一条带继续发送。
视频模式按整帧提交，跳过像素差异比较和上一帧镜像复制；退出后首次普通 UI 提交重新建立镜像。

SPI 满屏 320×240 RGB565 的理论传输时间约 30.72 ms（尚未计入指令、拷贝和任务开销）。
分核可以减少竞争，但不意味着能稳定 30fps；先用 15fps 验证，再按实测提高。
画面右上角 `FPS` 是按 1 秒窗口统计的**实际画面帧率**（帧被页面取走并推送到面板才算一帧），`F` 为后端接收帧数，`D` 为最新帧缓冲被覆盖的次数；`FPS` 反映面板侧的真实节奏，`F`/`D` 反映后端与邮箱是否积压，两者都不包括官方解码器内部丢帧。

## 资源与控制

- 进入视频先停止音乐/故事音频，并关闭旧播放器的新播放请求；停止确认后才创建视频 I2S。
- 官方音频通过现有 `PcmOutput` 输出 44.1kHz / 16-bit / stereo，沿用 EQ、音量曲线和家长音量上限。
- MP3 注册由两个播放路径共享，不在退出页面时注销全局解码器。
- 整段播放持有 SD 访问租约，存储维护不能在文件还打开时卸载或更换卡。
- 退出、错误、结束时先销毁官方播放器和渲染任务，再释放缓冲、I2S、SD 租约，最后恢复旧音频入口。
  即使 I2S 释放失败也先恢复入口并保留下一次关闭重试，避免音频准入被永久关闭；未释放的频道会让下一次打开正常报错。
  `PcmOutput::stop()` 把 `i2s_channel_disable` 的 `ESP_ERR_INVALID_STATE`（已经停了）当作可继续删除，
  `begin()` 遇到残留通道先重试一次收尾；否则一次释放异常就会让当次开机的所有视频/音乐都拿不到 I2S 口。
- 打开失败按阶段区分：`渲染管线创建失败`（GMF 池/元素）、`音频输出初始化失败`（I2S 通道），
  串口同时打印 `esp_err_to_name` 与 `internal/largest/dma/psram`，便于判断是端口被占还是内存不足。
- 播放中计入家长使用时长并保持屏幕点亮；暂停/结束不计时。BLE 暂停、恢复、音量、停止和状态进度接入新路径。
- 播放完成留在当前页，OK 重播；Next/Prev 切换；长按 OK 返回。

打开、关闭、暂停 API 现在返回**请求是否接受**，不等待官方播放器完成操作。
只有视频控制任务能访问官方播放器句柄，UI 读取状态快照。
连续选择会合并为最后一个请求，旧请求完成后的状态不会覆盖新请求；列表路径快照用于播放和切换，
返回列表仅在存储版本变化后重新扫描。外部直接改卡而没有更新版本时，需要重启以重建列表。
扫描持有存储租约；USB 维护期间不更新缓存，读取失败保留旧列表，重新进入时重试。

视频列表**没有落盘索引**：`/.esp-story-index.bin` 只存音乐和故事，视频清单是运行时扫描 + 内存缓存，
掉电即失效，所以开机后第一次进视频页必须扫一遍卡（之后同一次开机内直接命中缓存）。
扫描本身不再逐文件 `stat`：FATFS 的 `d_type` 已给出目录/普通文件，`/video/.fill` 镜像改为**一次 readdir 遍历**
后在内存里二分匹配——`f_stat` 每次都会从目录首项线性查找，400 集素材会退化成 O(n²)。
需要扫描时页面先画「正在读取视频」再阻塞，避免旧画面停在原地像卡死。

EOF/错误在页面 tick 中提交关闭，不依赖绘制；保留最后已绘制的画布，关闭完成前不进入深睡。
视频到音乐/故事的切换也交给同一任务处理，避免旧 I2S 尚未释放便启动新音频。
取消可立即返回界面；若官方 API 正在阻塞，资源清理须等待该调用返回，不能安全地强杀任务。

## 内部 RAM 预算（16 MB flash / 8 MB PSRAM 板）

视频栈是整机最大的内部 RAM 消费者，PSRAM 再空也没用——I2S DMA 环必须落在内部 RAM。
2026-11-25 实测（`build-usb-nfc`，开机后各阶段 `internal free`）：

| 阶段 | 修复前 | 只搬 BLE/任务栈 | 再把目录搬进 PSRAM |
|---|---|---|---|
| `app_main` 打印 | 180 KB | 180 KB | 190 KB |
| BLE 初始化后 | 80.5 KB | 108.8 KB | 119.1 KB |
| 载入媒体索引前 | 58.7 KB | 105.5 KB | 105.5 KB |
| 载入 285 曲 / 656 故事后 | 19.8 KB | 66.4 KB | **105.4 KB** |
| **视频打开、I2S 之前** | **10.4 KB** | **41.9 KB** | **≈60 KB（播放中实测）** |
| 播放中最低点 | 失败 | 25.6 KB | **57.3 KB** |

三条规则：

- **NimBLE 主机堆放 PSRAM**（`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`）。它是单项最大占用，
  实测 76 KB；宿主内存不需要 DMA，控制器自己的缓冲仍在内部。改完内部 RAM +28 KB。
- **只把不写 flash 的任务栈放 PSRAM**（`xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`）：
  `audio`、`usb-tx`、`usb-rx`、`video-control` 共 26 KB。
  **`app` 任务必须留在内部 RAM**——它初始化 BLE 时会写 NVS，
  `spi_flash_disable_interrupts_caches_and_other_cpu` 会断言 `esp_task_stack_is_sane_cache_disabled()` 直接 panic；
  **`usb-files` 也必须留在内部 RAM**，它是写 OTA 分区的那个任务。
- **媒体目录是批量数据，必须落在 PSRAM**（`components/media/include/catalog_extmem.h`）。
  656 个故事 = 3280 个 `std::string`，40~60 字节一块，按 `SPIRAM_MALLOC_ALWAYSINTERNAL=8192`
  全进内部 DRAM，实测 39 KB——这就是 11-10 那次把故事上限从 128 提到 1024 之后视频打不开的原因
  （之前 656 > 128 让整个索引被拒，内存里 0 条故事，等于用“列表不可用”换来的内存）。
  现在构建目录期间把 malloc 的 PSRAM 阈值临时降到 16 字节（`std::string` 的 SSO 边界），
  目录内部占用从 39 KB 变成 **88 字节**，290 KB 数据进 PSRAM；
  窗口只覆盖读索引/扫目录，全程只用文件与字符串，不分配 DMA 缓冲。
- 官方的 5 个 GMF 任务（extractor/aud_dec/aud_render/vid_dec/vid_render）本来就用 PSRAM 栈
  （`player_defaults_cfg.h` 的 `stack_in_ext=1`），不占内部 RAM。

现场数据、回归溯源（哪次提交压垮的）和可复用的排查方法见
[视频打不开与内部 RAM 排查记录](video-ram-bugfix-log.md)。

排障入口：开机日志的 `app: <阶段>: internal=… largest=… psram=…` 与视频打开时的
`video: after decoders/pools/i2s/…`。失败信息也分阶段：`音频输出初始化失败` 是 I2S 拿不到连续块，
`渲染管线创建失败` 是 GMF 池；两者都不再笼统报「渲染资源不足」。

## 性能配置与统计

`sdkconfig.performance` 启用性能编译优化、保留断言并降低默认日志级别。普通构建保留调试配置。
新配置应使用独立构建目录；已生成的 sdkconfig 不会自动被 defaults 覆盖。

```sh
idf.py -B build-player-performance -D SDKCONFIG=build-player-performance/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.performance;sdkconfig.usb-product;sdkconfig.ota;sdkconfig.power' build
```

USB `sys` 状态的 `performance` 对象包含：

- `videoQueueMs`、`videoCloseMs`、`videoStopAudioMs`、`videoSetupMs`、`videoRunMs`：最近一次视频打开的各阶段耗时。
- `videoCommandQueueMs`、`videoCommandCloseMs`：最近一次控制命令的排队与资源关闭耗时，与视频会话统计分开。
- `videoFirstBackendFrameMs`：请求提交至第一个后端画面的耗时；0 表示尚未到达，不代表瞬间完成。
- `videoBackendCopyUs`、`videoBackendFrames`、`videoReplacedFrames`：最近一次后端拷贝及累计帧数。
- `lcdSubmissions`、`lcdVideoSubmissions`、`lcdBytes`：实际提交到 LCD 的次数与字节数。
- `lcdLastUs`、`lcdMaxUs`：最近及最长一次 LCD 提交耗时，含条带准备与 DMA 等待。
- `audioDecodeUs`、`audioWriteUs`、`audioBudgetYields`：普通音乐路径最近的解码步骤（含可能的读卡）、I2S 写入耗时和主动让出次数。
- `videoControlStackMinBytes`：控制任务的最小剩余栈；内部堆和 PSRAM 仍在原 RAM 字段中。

计数为 32 位累计值，长时间运行会回绕。两次采样的 LCD 提交增量可用于计算提交速率，
不代表面板扫描频率。CPU `load` 保留协议兼容值，并增加 `loadValid=false`，不能当作真实负载。
视频会话的打开耗时、首帧耗时、后端帧数及替换帧数在关闭后保留，下一次打开时清零。
官方视频解码内部耗时与 SD 独立读延迟尚未拆分，需结合真机分析。

普通音频循环取消每个解码步骤后固定等待一个 tick：I2S 写入形成背压时自然阻塞；
没有明显写入等待且连续处理达到 4 ms 后，主动让出一个 tick，保障低优先级任务和看门狗。

视频 UI 以 33.334 ms 为循环预算，绘制和 SPI 发送计入预算，使用 esp_timer 通知等待剩余时间。
超预算仍至少阻塞 1 ms 让出 CPU；定时器不可用时回退 tick 调度。该调整不代表实机已达到 30 fps。

## 素材迁移

```sh
node tools/build_video.mjs 原始视频.mp4 --out out-video --fps 15
node tools/verify_video.mjs out-video
```

生成单个 320×240 MJPEG AVI，内含 MP3 音轨。无声源和 `--no-audio` 生成静音轨。
只复制 AVI 到 `/video`，不要再依赖同名 `.mp3` / `.wav`。旧 QOI 必须从原始素材重新转换；
转换工具拒绝 QOI 参数，避免生成官方栈不能播放的文件。保留 `avi_index` 及其测试作为旧格式工具，
但原生视频播放页不再使用它。素材校验不代表对损坏文件、硬件性能或所有 AVI 变种的兼容保证。

**码率上限**：视频画面和内嵌音轨由同一张 1-bit TF 卡供给，实测只有 ~150-200 KB/s。
超过约 100 KB/s 时画面还能维持，但音频会间歇断供（20 ms 级 I2S 缓冲被读卡拖穿）。
`15 fps / quality 6` 约 150 KB/s，适合快速卡；慢卡请用 `--fps 10 --quality 12`（约 75 KB/s）。
转换和校验都会打印平均码率，超过 100 KB/s 时给出警告。

## 验证与上板验收

2026-11-04 本地验证：默认固件与 USB + OTA + 省电组合固件均编译通过；
主机测试、MP3 适配故障测试、原生 USB 协议测试、视频转码/格式拒绝测试通过。
最新帧缓冲额外通过 ASan/UBSan 检查（未启用泄漏检测），覆盖替换、边界、解绑及并发无撕裂。
Docker 镜像解包受容量限制后，改用本地 IDF 5.5.3 工具链完成编译。
固件约 2.6 MB；当前 OTA 分区表为每槽 6 MiB，另有 3.9 MiB `assets` 分区预留（未挂载），镜像大小和剩余空间以当前构建输出为准。
其他独立诊断配置未在本次本地逐个重编，保留在 CI 构建矩阵中。

构建命令：

```sh
idf.py build
idf.py -B build-player-product -D SDKCONFIG=build-player-product/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.usb-product;sdkconfig.ota;sdkconfig.power' build
bash tools/test_host.sh
bash tools/test_mp3_stream.sh
bash tools/test_native_protocol.sh
bash tools/test_video_conversion.sh
```

没有在此环境连接实物，以下项目仍必须上板验证：

1. 同一源视频转换为 12/15/20fps，连续播放至少 10 分钟，观察音画同步、F/D、声音与看门狗。
2. 播放音乐 → 视频 → 音乐，循环 50 次；暂停/恢复、结束重播、快速切换、返回主页。
3. BLE 音量/静音/暂停/停止，家长锁和每日限额在播放期间生效，核对手机进度。
4. 缺失/损坏 AVI、旧 QOI、无声 AVI、SD 维护忙、PSRAM 不可用，确认报错后可恢复音乐。
5. 开启 USB、BLE 与省电设置测试；检查内部空闲堆、最小堆、任务栈高水位与功耗。
6. OTA 镜像空间和实机回滚流程另验；编译成功不等于已经确认新固件可安全回滚。
