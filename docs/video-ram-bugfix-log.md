# 视频打不开与内部 RAM 排查记录

2026-11-25 一次上机的现场记录。用户报了两个症状：**进视频 tab 卡顿**、**视频打不开并显示「渲染资源不足」**。
两者根因完全不同——前者是目录扫描的复杂度问题，后者是内部 RAM 被耗尽，而且是 11-10 一次提交造成的回归。
保留过程是因为「一次改动压垮了不相干的功能」这类问题在这个项目里还会再出现。

## 一、视频 tab 卡顿：不是索引失效，是没有索引

### 症状

首页选中「视频」按 OK，旧画面停住一两秒才出列表；用户怀疑「索引没生效」。

### 根因

视频列表**没有落盘索引**。`/.esp-story-index.bin` 只存音乐和故事，视频清单是运行时扫描 + 内存缓存，
掉电即失效，所以开机后第一次进视频页必然要扫一遍卡。

扫描本身写得过重：

- 每个目录项都 `stat()` 一次判类型（目录 / 普通文件）；
- 每个 AVI 再 `stat()` 一次探 `/video/.fill` 镜像；
- FATFS 的 `f_stat` 每次都**从目录首项线性查找**，400+ 集素材就是 O(n²)；
- 扫描发生在 UI 任务里，期间不重绘，观感就是「卡死」。

### 修复

- `d_type` 判类型（IDF 的 FATFS 一定填 `DT_DIR`/`DT_REG`），只有 `DT_UNKNOWN` 才退回 `stat()`；
- 先判扩展名再判类型，非 AVI 不进 `stat()`；
- `/video/.fill` 镜像改为**一次 `collectMirror()` readdir 遍历 + 内存二分匹配**，逐文件 stat 全去掉；
- 需要扫描时先画「正在读取视频 / 请勿拔出 TF 卡」并 `Display::present(true)` 再阻塞；
- 扫完打一行 `video: list scan: N entries in ?ms`。

同一开机内再次进入直接命中内存缓存（按存储版本号判定）；USB 上传会让版本号 +1，下次进入重扫。

## 二、视频打不开：内部 RAM 被吃光

### 症状（真实日志）

```
E (41869) i2s_common: i2s_alloc_dma_desc(541): allocate DMA buffer failed
E (41869) i2s_std: i2s_std_set_slot(138): allocate memory for dma descriptor failed
E (41869) pcm: i2s setup: ESP_ERR_NO_MEM
E (41869) video: i2s begin: ESP_ERR_NO_MEM
E (41869) video: audio output failed: internal=10443 largest=10240 dma=10439 psram=7034440
```

内部 RAM 剩 **10.4 KB**、最大连续块 **10.0 KB**，而视频 I2S 环要 **12 × 1920 B = 23 KB** 连续块。
PSRAM 还有 7 MB 空着——但 I2S DMA 只能用内部 RAM，所以 PSRAM 多空都没用。

### 测量手段

1. **分阶段堆打点**（现在是常驻日志）：`app: <阶段>: internal=… largest=… psram=…`，
   在 `app task start / after display / after fonts / after ble / before media index / after media index /
   after session restore` 各打一行；视频打开路径另有
   `video: after decoders/pools/i2s/audio render/video render/player init`。
2. **临时 host 触发口**：在 `sys` 路由加一个 `video_test`（带 path 参数）直接调 `Video::open()`，
   这样没有按键也能复现「视频打不开」，改完再删掉。硬件问题能无人值守复现，比反复请人按键快得多。

### 现场数据（`build-usb-nfc`，开机各阶段 `internal free`）

| 阶段 | 修复前 | ①BLE→PSRAM | ②任务栈→PSRAM | ③目录→PSRAM |
|---|---|---|---|---|
| `app_main` 打印 | 180.2 KB | 180.2 KB | 190.5 KB | 190.5 KB |
| 显示/字体后 | 156.3 KB | 156.3 KB | 166.5 KB | 166.5 KB |
| BLE 初始化后 | **80.5 KB** | 108.8 KB | 119.1 KB | 119.1 KB |
| 载入索引前 | 58.7 KB | 87.0 KB | 105.5 KB | 105.5 KB |
| 285 曲 / 656 故事后 | **19.8 KB** | 47.9 KB | 66.4 KB | **105.4 KB** |
| 视频打开、I2S 之前 | **10.4 KB** | — | 41.9 KB | **≈60 KB** |
| 播放中最低点 | 失败 | — | 25.6 KB | **57.3 KB** |

两个大项一目了然：**BLE 一项 76 KB**（`156267 → 80527`），**媒体目录 39 KB**（`58739 → 19795`）。

### 排除项

- **不是 I2S 端口被占**：`ESP_ERR_NO_MEM` 而不是 `ESP_ERR_NOT_FOUND`，且 `dma=10439` 与 `internal` 几乎相等。
- **不是 PSRAM 不足**：`psram=7034440`，四块 150 KB 帧缓冲都分配成功。
- **不是代码路径问题**：同一份代码在 11-06 还能播，见下文回归溯源。

### 修复（三层，缺一不可）

1. **NimBLE 主机堆 → PSRAM**：`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y`（写进 `sdkconfig.defaults`）。
   宿主内存不需要 DMA，控制器自己的缓冲仍在内部。净省 **28 KB**。
2. **不写 flash 的任务栈 → PSRAM**：`audio`、`usb-tx`、`usb-rx`、`video-control` 共 **26 KB**，
   用 `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`。
   **`app` 任务必须留内部 RAM**——它初始化 BLE 时写 NVS，会直接 panic：
   `assert failed: spi_flash_disable_interrupts_caches_and_other_cpu cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())`。
   **`usb-files` 也必须留内部 RAM**，它是写 OTA 分区的任务。
3. **媒体目录 → PSRAM**（`components/media/include/catalog_extmem.h`）：见下一节。

只做前两步时，视频往后退了一步、卡在更后面：

```
I (86029) ESP_PLAYER_PIPELINE: Starting video renderer
E (86029) ESP_GMF_TASK: esp_gmf_task.c:482 (esp_gmf_task_init): Memory exhausted   ← 连信号量都建不出来
E (86029) ESP_PLAYER_STATE: Failed to start video renderer
```

说明解码器拿完缓冲后内部 RAM 归零，还差最后一截。

### 第③层：目录为什么必须进 PSRAM

656 个故事 = 3280 个 `std::string`，路径 40~60 字节一块。按 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192`，
只有 ≥8 KB 的块才进 PSRAM，所以这些字符串全落在内部 DRAM，实测 **39 KB**。

没有改数据模型（把 `Track`/`Story` 的 `std::string` 换成自定义分配器会牵动
`pages_music.cpp`、`pages_story.cpp` 和主机协议的全部接口），而是用 IDF 的运行时阈值：

```cpp
// 构建目录期间把 malloc 的 PSRAM 阈值降到 std::string 的 SSO 边界
class ExtmemWindow {
    explicit ExtmemWindow(std::size_t limit = 16) { heap_caps_malloc_extmem_enable(limit); }
    ~ExtmemWindow() { heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL); }
};
```

`Library::scan()` 和 `Library::loadCache()` 各放一个实例。效果：

```
boot: before media index: internal=105515
after media index:        internal=105427    ← 目录只吃 88 字节
psram: 7984460 → 7695032                     ← 290 KB 数据进 PSRAM
```

窗口只覆盖读索引 / 扫目录，全程只有文件 IO 与字符串操作；所有 DMA 缓冲都用显式 `heap_caps_*` caps，
不受这个阈值影响。**约定：不要在这个窗口里分配 DMA 缓冲。**

### 验证

```
video: after i2s: internal=42015 largest=40960
video-perf: request=1 stop=0ms setup=10ms run=3ms heap=39239 err=-
ESP_PLAYER_STATE: Handling cmd: QUIT in state: PLAYING      ← 确实在播
videoFirstBackendFrameMs=491  videoBackendFrames=24
播放中 heapFree=60387  heapMin=57259
```

## 三、回归溯源：哪次改动导致的

视频在 11-06 还能播（见 [音频/视频卡顿排查记录](audio-video-bugfix-log.md) 的验收数据），之后没人再测，直到这次。

| 提交 | 日期 | 改动 | 对内部 RAM 的影响 |
|---|---|---|---|
| `27d0912` | 11-03 | 引入 BLE 控制通道（NimBLE） | **−76 KB**，且一直用默认 `MEM_ALLOC_MODE_INTERNAL` |
| `a93c72b` | 11-05 | 音频按描述符分片写，视频环 3×480 | 环 11.5 KB |
| `833c394` | 11-06 | 视频环 3×480 → **12×480**（修 100 ms PCM 块的断音） | **−11.5 KB**，I2S 环变 23 KB |
| `2732053` | 11-10 | `kMaxStories` 128 → **1024** | **−39 KB**，压垮 |

关键在**改之前那 656 个故事根本没进内存**：

```cpp
// media_cache.cpp, readCatalog()
const std::size_t story_limit = version == 2 ? 32 : kMaxStories;
if (!archive.number(stories) || stories > story_limit) return false;   // 656 > 128 → 整份索引被拒
```

`kMaxStories` 当时是 128，于是开机 `loadCache()` 直接拒绝索引、`Library::scan()` 返回 `LimitExceeded`
丢掉整个目录——内存里 **0 条故事**。等于用「故事列表不可用」换来了 39 KB 内部 RAM，视频因此还能播
（当时视频打开约 49 KB，够 23 KB 的环）。

11-10 把上限提到 1024（为了修列表不可用），656 条故事连同字符串真正驻留 → 视频打开掉到 10.4 KB → 打不开。

**结论：`2732053` 是直接触发，`833c394` 提前吃掉了余量的一半，`27d0912` 是一直存在的底噪。**

## 四、顺带修掉的真 bug

1. **I2S 通道泄漏**（`pcm_output.cpp`）：`i2s_channel_disable` 返回非 OK 时直接 `return`，
   通道既没 disable 也没 delete，`channel_` 一直挂着 → 之后每次 `begin()` 都 `ESP_ERR_INVALID_STATE`，
   **当次开机再也播不了任何视频**（用户之前说的「重启才好」）。现在把 `INVALID_STATE`（已经停了）
   视为可继续删除，`begin()` 遇到残留通道先重试收尾。
2. **播放结束后屏幕一直黑**（`power_service.cpp`）：熄屏后只有「用户按键」才亮屏，
   如果歌自己播完或家长用手机按了停止，屏幕就一直黑着。现在策略不再要求熄屏时自动亮屏，且不吞下一次按键。
3. **深睡唤醒引脚未检查**（`power_service.cpp`）：`esp_sleep_enable_ext1_wakeup()` 失败时仍会
   `esp_deep_sleep_start()`，等于睡下去没有任何唤醒源、只能拔电。现在 arm 失败就不休眠并打
   `wake pins cannot arm ext1 (...)`，本次开机不再重试。

## 五、新增：设置 → 资源占用

`pages_settings.cpp` 增加一页只读诊断：内部 RAM / PSRAM / Flash 的「已用 / 总量 + 占用条 + 明细
（余量、最大块、最低点、分区名）」；数值口径与 USB `sys stats` 的 `ram`/`rom` 一致，可在两个入口互相核对。
每秒刷新只重绘，不分配内存；进入时读一次镜像元数据。

## 六、可复用的排查方法

1. **先量再改**：一次上机同时打内部/PSRAM/DMA 三个池的阶段快照，比逐次猜参数快得多。
   这个项目的三件套是：`MALLOC_CAP_INTERNAL`（I2S DMA、驱动缓冲、内部任务栈）、
   `MALLOC_CAP_SPIRAM`（画布、帧邮箱、目录、播放器 GMF 缓冲）、`MALLOC_CAP_DMA`（I2S 环）。
2. **看 errno 别猜**：`ESP_ERR_NO_MEM` = 内存不够，`ESP_ERR_NOT_FOUND` = 端口被占，
   `ESP_ERR_INVALID_STATE` = 上一手没释放。三种的修法完全不同。
3. **PSRAM 栈有硬边界**：只要任务可能写 flash（NVS/OTA），栈就不能放 PSRAM，否则
   `esp_task_stack_is_sane_cache_disabled()` 直接断言。
4. **FATFS 的 `f_stat` 是 O(n)**：逐文件 stat 会把目录扫描变成 O(n²)，用 `d_type` 或单次 readdir。
5. **临时 host 触发口**：给硬件路径加一条 `sys` 动作，让「必须按键才能触发的故障」可以脚本复现，
   验证完删掉。
6. **离线改 Kconfig 的绕法**：改 `build-*/sdkconfig` 会触发 CMake 重配置 → 组件管理器联网校验
   registry/git，无网络时 300 s 超时。可改 `build-*/config/sdkconfig.h` 并把 `sdkconfig` 的 mtime
   回拨到 `build.ninja` 之前，然后 `ninja -C build-…` 增量编译。

## 相关文件

| 文件 | 内容 |
|---|---|
| `components/video/video_library.cpp` | 目录扫描：`d_type` + 镜像单次遍历 |
| `components/app/src/pages_video.cpp` | 扫描页、扫描耗时日志 |
| `components/audio/pcm_output.cpp` | I2S 收尾与自愈 |
| `components/video/player.cpp` | 错误分阶段、PSRAM 控制栈、堆快照 |
| `components/media/include/catalog_extmem.h` | 目录构建期的 PSRAM 阈值窗口 |
| `components/power/power_service.cpp` | 自动亮屏、唤醒引脚检查 |
| `sdkconfig.defaults` | NimBLE 主机堆进 PSRAM |
| `docs/idf-video-player.md` | 「内部 RAM 预算」一节（口径与三条规则） |
