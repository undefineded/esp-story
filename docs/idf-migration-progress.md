# ESP-IDF 迁移实施记录

> 新会话接手请先读 [交接文档](idf-handoff.md)：环境、构建入口、已完成/剩余、已知坑都在那里。

## 当前状态（后面的批次记录为历史过程）

USB 上传诊断已贯通协议、队列原始数据、独占会话和事务后端；上传默认关闭。
提交检查实际落盘大小，未完成/短写不发布；同会话提交应答丢失可重试。
显式恢复保留隔离文件，冲突返回 RecoveryRequired，不自动删除新旧版本。
主机故障注入覆盖写入、flush、fsync、close、重命名、回滚和假成功短写。
仍不支持完整桌面文件管理。上传/恢复会持久化索引脏标记并递增存储变更序号；
会话释放后诊断循环重新扫描、更新播放列表并保存索引，曲目尽量按旧 ID 保留，保持暂停。
刷新失败保留旧快照并限速重试；重启遇到脏标记会跳过所有缓存，重新扫描。
索引保存会比对快照对应的存储序号，禁止把上传前的旧目录重新写成有效缓存。
主机集成测试覆盖独占会话、脏标记跨重启、旧快照保存拒绝和新索引重新加载；
该流程的 ASan/UBSan 检查通过（关闭运行环境不支持的泄漏检测）。
FAT 文件与目录项在突然断电时的实际持久化顺序仍须真机验证，不宣称绝对原子性。

本轮 USB 只读迁移验证：默认、外设、音频、USB 联合诊断和确认 FRAM 五种配置本地构建通过；
主机回归、原生协议和 MP3 适配器故障测试通过。文件读取/上传事务的 ASan/UBSan 检查通过
（运行环境不支持 LeakSanitizer，本次关闭泄漏检查）。没有烧录设备或执行真实 USB/SD 验收。

- 已加入原生音乐目录、童谣分类、三层故事扫描、稳定目录快照和旧 v2 媒体索引读取/备份回退。
  扫描使用有界工作列表；失败不发布半成品。成功扫描后已接入旧 v2 格式事务保存。
  保存分两遍流式编码，不缓存完整索引；先关闭存储准入，写临时文件并同步，再替换主索引。
  启动读取依次尝试主索引、事务备份、旧 `.bak`。遗留临时/备份文件不自动清除，
  后续保存返回 RecoveryRequired，保留恢复现场；断电后的自动修复策略仍待补齐。
  当前音乐深度上限为 16、待扫描目录上限 300；超限返回错误并保留旧快照。
- 已接入上一曲/下一曲、音乐 EOF 自动下一曲、Vocal/Pop/Bass/Flat 三段 EQ。
  故事不自动切入音乐。EQ 频率按低采样率的 Nyquist 上限调整，输出最后统一限幅。
- 故事 LRC 时间轴、JPEG/VFS 读取已接入；图片读取全程持有存储许可。
  时间轴保持整数秒更新，但仍使用提交给 I2S 的样本数估算，实际输出延迟未校准。
- 设置 MPL3/MPL4、音量、MPS2 会话位置、FAV 位图已实现原格式读写及回读校验。
  未确认板型时不访问 FRAM；确认后可恢复曲目并暂停、保存音量与音乐位置。
  家长 PCTL v1 130 字节记录和策略核心已迁移；绑定/鉴权和完整设置页面尚未迁移。
  默认音量已与旧版会话记录对齐为 4；FRAM 读取失败时停用会话自动保存。
- 原生 Wi-Fi 支持 STA/AP/APSTA 开关及有上限的退避重连；诊断默认无线关闭。
  NVS 初始化失败不擦除数据。原生无线已参加实际链接，尚未做射频实测。
- USB 按当前源码实际的 **v4 / SBX4** 帧迁移，支持 hello/ping。
  音频诊断挂载 SD 后发布 `fs_readonly`，支持目录分页、只读会话、4 KiB 原始二进制
  分块下载、取消与超时释放；没有发布完整 `fs` 或写入能力。
  RX、文件工作任务、TX 分离，队列有界；日志经同一 TX 队列串行输出。
  主机不读或队列满时丢弃，需要客户端超时处理。文件工作任务执行 SD I/O，不阻塞 RX。
  主机协议测试覆盖版本、超长/嵌套输入、重复字段、转义 NUL、非法数值与 CRC 二进制帧。
  USB 写入已作为默认关闭的诊断选项接入：开始/分块/提交/取消/超时清理及显式恢复。
  开启后声明 `fs_transfer` 而非完整 `fs`，尚不支持删除、重命名、自动建目录或重扫。
  尚未接入 NFC 命令路由及异步写卡完成事件。
- PN532 与原 NFC 状态机已加入原生构建；共享 I2C 由板级服务拥有，NFC 不重建总线。
  独立 NFC 任务提供状态快照和轮询；修正了响应 LCS、响应命令号、空 SAM 响应解析，
  并改成单事务读取响应帧。固定帧测试通过，真实 PN532/卡片兼容性仍待验证。
  另有模拟 Wire 的完整命令往返测试，覆盖固件查询、无卡、坏帧、拔出和恢复。
- `app_main()` 仅初始化诊断与任务入口，UI/媒体诊断移至 12 KiB 栈的独立任务。
- 确认 FRAM 后，诊断播放入口执行家长额度、音量、内容白名单、童锁和远程锁标志；
  PCTL 无效/读取失败时阻止播放，写入失败后停止播放，不自动初始化或覆盖旧记录。
  用量每分钟写后回读校验；仍有断电最多约一分钟的未持久化窗口。
  策略核心支持跨日历史、回拨不重置额度；首次校时保留未校时用量。
  尚未接入可信校时入口，因此启用睡眠时间限制的记录会保守阻止播放。
  未确认 FRAM 的构建仍是无家长保护的硬件诊断模式，不能作为产品固件。

诊断控制：OK 单击播放/暂停，双击切换 EQ；编码器调整音量；NEXT/PREV 短按切曲；
NEXT 长按切换音乐/童谣/故事；PREV 长按循环无线模式（启用网络诊断时）；
SET 短按重新载入暂停、长按停止。确认 FRAM 后 OK 长按保存 EQ，并报告写后验证结果。
这些是迁移诊断控制，不代表旧产品页面和所有交互已经恢复。
USB 下载前须长按 SET 停止播放并释放文件；暂停仍持有文件，会返回 `AUDIO_BUSY`。
会话独占存储期间，新的音频/图片读取会被拒绝；结束后不自动恢复播放。
详见 [USB 只读迁移接口](idf-usb-readonly.md)。

完整联合诊断构建（**不等于实机验收**）：

```sh
idf.py -B build-usb -D SDKCONFIG=build-usb/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.usb-diagnostics' build
```

该配置关闭 IDF 普通控制台，将 USB 输出交给 v4 transport；启动显示、音频、网络与 NFC
诊断服务。FRAM 确认开关默认关闭。不能作为已完成家长策略和鉴权的产品固件发布。

剩余主要工作：完整产品页面与应用授权入口、家长策略和绑定、BLE、USB 文件/NFC 路由、
索引中断后的自动恢复、准确音频元数据/时间、OTA/生命周期/电源以及全部真机验收。

产品 UI 骨架已接入（`components/app`，见 2026-11-03 批次）：首页、设置页、
音乐分类/歌曲列表/播放页可用；故事与视频页仍是占位。构建入口由
`CONFIG_STORYBOX_APP_UI` 控制，启用外设诊断时仍优先进入诊断镜像。

### 家长控制本轮验证

- 主机回归、MP3 适配器故障测试和 USB bootstrap 协议测试通过。
- PCTL 测试覆盖固定偏移、校验损坏、I/O 失败、写后读取失败、音量范围、
  白名单边界、睡眠时间边界、童锁/远程锁、用量饱和、暂停累计和跨日/回拨。
- ESP-IDF 5.5.1 USB 联合诊断及确认 FRAM 分支本地构建通过；没有烧录设备。
- CI 增加 `test/sdkconfig.fram-compile-only` 编译覆盖；该产物不上传，
  配置文件不是物理板型确认凭据。CI 尚未远端执行。
- 绑定/PIN/token 仍未迁移或开放；旧格式中的 FNV hash 不被宣称为安全鉴权。

## 2026-11-03：P1 应用骨架与首页 / 设置页

先把主线 Arduino 功能整体合入迁移分支（视频播放器、故事分类、递归故事目录、
音频目录展开等），再在原生侧建立产品 UI 层。合并后的 `src/` 同时服务
PlatformIO 与 IDF 双构建，冲突文件逐个保留两套路径：

- `src/display.cpp`：背光保留 `#ifdef ARDUINO` 的 LEDC 路径与 IDF 原生
  `ledc_timer_config` 路径；新增 `setBacklightFreq()` 的 IDF 实现
  （`ledc_set_freq` + 重写占空比）。合并残留的多余右花括号会让
  `Display` 命名空间提前闭合、把 `canvas()/present()` 挤到全局作用域，已修正。
- `src/input.cpp`：编码器累加器统一走 `portMUX` 临界区（Arduino 的
  `noInterrupts()` 在 IDF 下不可用），ISR 与任务共用同一把锁。
- `src/pn532.cpp`：新增 `SB_PN532_LOG` 宏（Arduino=`Serial`，IDF=`ESP_LOGI`），
  `logBusScan()` 在 IDF 下改用 `i2c_master_probe` 扫描共享总线，
  `begin()` 用 `gpio_set_pull_mode` 只改上拉、不动开漏方向。

新增 `components/app`（产品 UI 层）：

- `app.cpp`：UI 任务（16384 字节栈、prio 3、20 ms 帧间隔）串起初始化与主循环：
  Display → Fonts → Input → Hall →（确认 FRAM 时）Settings/MediaRecords →
  SD 挂载 → AccessGate/Library/Playback；随后每帧采样输入、分发页面、
  只在页面标脏时重绘并 `Display::present()`。音量折中写回 FRAM（1 s 去抖）。
- 页面栈与 `Page` 接口（`enter/leave/handle/tick/draw/invalidate`），最深 6 层，
  页面为进程级单例、不占堆；导航失败不会打断音频任务。
- `app_theme`：调色板、UTF-8 安全截断（按码点回退，不再切断汉字）、品牌笑脸、
  霍尔卡片与童锁角标（角标底色跟随当前顶栏）。
- 首页：复刻 Arduino `drawHome()`——四张车票、主题底色、`home_scenes.h` /
  `home_video_scene.h` 烘焙插画直接编入（无解码）、音量药丸、页码圆点、
  旋钮/上下换焦点、OK 进入。音乐/故事/视频入口暂时只提示后续批次。
- 设置页：亮度（10..100，步进 5）、旋转 0°/180°（切向即整屏重绘）、
  EQ 四档含频响示意、播放中熄屏与无播放休眠的分钟循环、刷新列表/蓝牙/绑定码
  占位项。旋钮调值、NEXT/PREV 换行、双击 OK 保存返回、长按 SET 保存回主页。
  FRAM 未确认的构建只改 RAM，不伪装成已保存。

构建配置：新增 `CONFIG_STORYBOX_APP_UI`（默认 y）。诊断入口优先级更高，
既有 `sdkconfig.*-diagnostics` 语义不变。

验证（本机新装 IDF 5.5.1，源码 commit
`fcae32885b0296b32044cb99ecbdc50d98dddb83`）：默认配置构建通过，
`storybox.bin` = `0x161890` 字节，app 分区剩余 83%；**未烧录、未做真机验收**。
本机没有系统 Python，IDF 的 Python 环境建在
`~/.espressif/python_env/idf5.5_py3.11_env`；组件管理器需要用
`GIT_CONFIG_*` 临时关闭 `core.autocrlf`，否则 LovyanGFX 的检出内容哈希不匹配。

已知限制：音乐/故事/视频页面仍是占位提示；熄屏、深睡、BLE、家长绑定/鉴权、
USB 文件写与 NFC 路由、视频解码、OTA 均未接入。

### P2（第一批）：音乐列表与播放页

- 首页「音乐」入口接通：分类列表（全部歌曲 + 各分类及计数）→ 歌曲列表
  （`Media::Playlist` 视图，长按 OK 收藏）→ 播放页（封面卡、曲名、采样率/码率、
  已播时间、播放/暂停、上一曲/下一曲、长按 OK 停止、长按 SET 回主页继续播放）。
- 首次进入且没有索引时先画扫描页再 `Library::scan()`，完成后写回 v2 索引；
  扫描前先 `Playback::stopAndWait()`（维护需要释放全部文件租约），结束后
  `allowPlayback()`，不自动续播。
- 文件句柄只通过 `AccessGate` 持有；列表浏览用 `Playlist` 副本，不扰动播放位置；
  收藏按目录全量下标读写 FAV v1 位图，FRAM 不可用时只提示不写入。
- 仍缺：总时长/VBR 元数据（进度条只显示已播时间与活动点）、童谣分类入口、
  故事页、视频页、BLE 点播。

构建验证：默认配置通过，`storybox.bin` = `0x166400` 字节（分区剩余 82%）；
USB 联合诊断配置（`sdkconfig.usb-diagnostics`，含 USB v4 + NFC + 音频/网络诊断）
同样构建通过，`storybox.bin` = `0x192630` 字节，说明诊断入口的优先级分支未被产品入口破坏。

注意：本机 `idf.py -D` 短选项会被当成构建目标，需用
`--define-cache-entry SDKCONFIG=... --define-cache-entry "SDKCONFIG_DEFAULTS=..."`。

### P2（第二批）：故事目录与故事播放页

- 故事扫描改为递归 6 层（对齐 `MAX_STORY_DEPTH`）：目录里同时有 `.mp3` 和 `.lrc`
  才算一个图文故事；否则该目录下每个 MP3 都是一个独立音频故事（对齐
  主线 `b89a62f` 的“音频故事按 MP3 展开”）。故事上限从 32 提到 128。
- 索引格式升到 **v3**（故事上限 128），读取端同时接受 v2（上限 32）与 v3；
  标题、`kind`、`category` 由 `normalizeStory()` 从路径推导，缓存不新增字段，
  因此旧 v2 索引不需要迁移即可继续加载。
- 新增故事列表页（标题 + 图文/音频标签）与故事播放页：
  - 图文故事按 LRC 时间轴切图，`Display::drawStoryImage()` 全程持有存储许可；
  - 音频故事显示标题/已播时间，走同一播放服务；
  - OK 暂停/继续，NEXT/PREV 切上一个/下一个故事，长按 OK 停止返回列表，
    长按 SET 停止并回主页；音量由旋钮调整。
- 主机测试：新增 `test/host_compat.h`（Windows/MinGW 的 `fsync`/`lstat`/`symlink`
  shim，POSIX 下为空）并由 `tools/test_host.sh` 统一 `-include`；两处 symlink
  断言在 Windows 下跳过（本机没有创建符号链接的权限）。`media_library` /
  `media_cache` 测试更新到新语义：两个故事（图文+音频）、v2 索引可读、写出 v3。
- 本机验证：媒体冒烟测试（临时脚本，未入库）通过——递归扫描、6 层深度限制、
  分类与标题推导、v3 索引读取、v2 索引兼容。完整 `tools/test_host.sh` 在
  Windows 上仍受 POSIX `rename` 覆盖语义与 UTF-8 路径限制，未跑通；以 Linux/CI 为准。
- 构建：默认配置通过，`storybox.bin` = `0x169a00` 字节。

### P2（第三批）：故事目录逐级浏览

- 故事列表改为真正的目录浏览：从 `/story` 开始，目录行由扁平索引按路径**即时推导**
  （不进索引格式），目录优先排序、故事在后；OK 打开目录/播放故事，长按 OK 返回
  上一级（到根再回主页），长按 SET 直接回主页。
- 顶栏标题显示当前目录名，目录行画文件夹图标、故事行显示 `图文/音频`。
- 构建：`storybox.bin` = `0x16ad00` 字节。

### P3（第一批）：视频索引与视频页

- 存储层给 `FileStream` 增加 `seek()/tell()`（容器格式需要随机访问；租约语义不变）。
- 新增 `components/video`：
  - `AviIndex`：解析 RIFF/`hdrl`（`avih` 取宽高与帧间隔、`strl` 取 `vids`
    处理器的 fourcc 与 `dwScale/dwRate`），顺序扫描 `movi` 记录 `00dc/00db`
    视频块偏移，跳过音频块；帧数上限 8000、单帧上限 192 KiB；
    编码从 `strh` 处理器判定，缺失时用首帧 `qoif`/JPEG SOI 兜底。
  - `scanLibrary()`：列 `/video` 下的 AVI（不递归、按名排序），`audioPathFor()`
    求同名 `.mp3` 旁白。
- UI：视频列表页（胶片图标、AVI 标签）+ 播放页（按 `frameInterval` 取帧、
  `drawQoi`/`drawJpg` 解码、可选同名 MP3 旁白、播放/暂停、上下切换、
  控制条每帧重绘、解码耗时角标）。背光沿用 25 kHz 默认值，未额外切换。
- 验证：
  - 新增 `test/avi_index_test.cpp`（主机测试，已并入 `tools/test_host.sh`）：
    合成 MJPG/QOI 容器、帧偏移与内容、帧间隔/时长、超限拒绝、缺失文件与
    无 `movi` 容器的拒绝；本机 MinGW 通过。
  - 用 `tools/build_video.mjs` 生成的真实 108 MB AVI 实测解析：
    `QOI 320x240 / 1966 帧 / 83 ms 间隔 / 163 s 时长`，抽查首帧、中间帧、
    末帧均读出且首字节为 `qoif`。
  - 固件构建通过：`storybox.bin` = `0x16e3c0` 字节。
- 已知限制：音视频仅“同时起播”，没有漂移校正；解码超时靠跳帧而不是丢帧统计；
  未做真机帧率/发热验收（PSRAM 带宽与 SPI 推屏是主要风险点）。

### P5（第一批）：家长绑定与播放门禁

原计划把打 BLE 放在 P5 之前，但 BLE 的 `bind`/`auth` 依赖绑定核心，所以先把
绑定与策略门禁做掉，BLE 复用同一套记录与校验。

- 新增 `components/settings/include/parental_binding.h`（纯头文件、可主机测试）：
  - 绑定码 6 位数字；`bind` 校验设备显示的码，生成 32 位十六进制 token，
    记录 `token_hash`（FNV-1a，与旧固件同格式，不宣称为安全鉴权）。
  - `verify_token`、`set_pin`/`verify_pin`（4~6 位数字、盐 + FNV）、`unbind`、
    `factory_defaults`（保留 PIN/绑定标志位以外的配额与掩码复位，并重新生成绑定码）、
    标志位读写（BOUND/HAS_PIN/BEDTIME/ALLOWLIST/REMOTE/CHILD_LOCK）。
  - 随机源以函数指针注入：设备用 `esp_random()`，主机测试用确定性序列。
- 应用层接入：
  - 启动时读 PCTL：**无效记录**按旧固件语义生成宽松默认值并写回一次（含绑定码）；
    **读取 I/O 失败**不写、不伪装，策略标记为不可用。
  - 每次进入播放（音乐/故事音频/视频旁白）都过 `playbackAllowed()` 与
    `categoryAllowed()`；锁定期间正在播放的会被停止。
  - 音量经 `clampVolume()`；使用时长按秒累计、`parentalSaveIfDirty()` 每 60 s 落盘一次。
  - 设置页“显示绑定码”打开新页面，显示 6 位码 + 是否已绑定；家长记录不可用时显示提示。
- 验证：新增 `test/parental_binding_test.cpp`（并入 `tools/test_host.sh`）覆盖
  绑定/错误码/重复绑定、token 校验、PIN 规则与校验、解绑、恢复默认、标志位、
  以及每次修改后 `validParentalRecord()` 仍然通过；本机 MinGW 通过。
  默认配置 `0x16e8a0` 与确认 FRAM 配置 `0x174640` 均构建通过（后者编译了 FRAM 分支；
  首次构建曾遇到 `esp_lcd_panel_rgb.c` 的编译器段错误，重跑即通过，疑为并行内存压力）。
  这两个尺寸是**加入 BLE 之前**的；当前树（含 NimBLE）为默认 `0x1be250`、
  FRAM `0x1c4510`、USB 联合诊断 `0x1942a0`。

### P4（第一批）：BLE 控制通道（NimBLE）

- `sdkconfig.defaults` 打开 NimBLE 外设模式（1 连接、MTU 247、host 任务栈 4 KiB）。
- 新增 `components/ble`：与旧固件**相同的 128 位 UUID**（service/RX/TX），
  RX 累积字节流按 `\n` 拆行入队，TX 按 MTU-3 分片 notify；JSON 收发沿用
  `{"v":1,"id":..,"ok":..}` 形状，握手/状态字段与小程序一致。
- 协议：`hello`（含 fw/name/bound/hasPin/volMin/volMax，未绑定时给 bind code）、
  `bind`（校验绑定码 → 返回 token）、`auth`（token 或 PIN，含“已绑定但无 PIN 放行”的
  旧规则）、`sync`（校时进策略核）、`get state/config`、`ctrl play|pause|toggle|next|prev|
  stop|volume|story|music`、`pin verify`；每秒向已认证连接推一条 `evt:"state"`。
- 应用层：`Ble::begin()` 在显示/输入之后启动；主循环里 `Ble::loop()` 拆包、
  应用消费命令队列（音量/播放/切曲/点播故事或音乐），并每秒发布一次状态快照
  （播放态、音量、当前曲名、已用时长、锁定原因）。设置页「蓝牙广播」开关已接通。
- 尚未迁移（**已由 P4 第五批补齐**）：`get stories/music` 列表（返回空列表占位，列表浏览仍走 USB）、
  `set` 家长配置项、`pin set`、`reset`/`unbind` 命令。
- 验证：打开 BT 后重新生成 sdkconfig 并构建通过，`storybox.bin` = `0x1be250`
  （比无 BT 版本多约 320 KiB 的协议栈）；USB 联合诊断配置在开启 BT 后同样构建通过
  （`0x1942a0`）。目前只能在编译层验证，**没有真机射频/配对验收**。

### P4（第二批）：USB 文件写入路由

- 产品应用在 SD 挂载后创建**可写** `Storage::FileReader` 并 `Host::attachFiles()`，
  于是 `hello` 声明 `fs_transfer`（读 + 上传到已存在目录），桌面端文件管理器可用；
  删除/重命名/新建目录仍未实现，因此不声明完整的 `fs`。
- 主机写入会调用 `AccessGate::changed()` 递增存储序号；应用主循环检测到
  “索引序号 ≠ 当前序号”时，在**播放空闲**（Stopped/Ended/Failed）且距上次尝试 ≥2 s
  的条件下重扫并回写索引，成功后提示“媒体已更新”。播放中不打断，等空闲再做。
- 桌面端 `FileManagerPage` 的入口判断同时接受 `fs` / `fs_transfer` / `fs_readonly`
  （固件保持诚实的能力声明，客户端按能力展示）。
- 新增 `sdkconfig.usb-product`：产品 UI + USB 协议（不开诊断），用于验证
  “界面照常运行 + 桌面端传文件”的组合；该配置构建通过（`storybox.bin` = `0x1c3950`），
  联合诊断配置 `0x1942b0`、默认产品配置 `0x1be250` 亦通过。
- 仍缺：`mkdir/remove/rename/rescan` 命令、容量/已用字节、上传进度事件；
  真机 FAT 断电一致性与桌面端联调未做。

### P4（第三批）：NFC 命令路由

- hostlink 新增可插拔路由点 `Host::attachRoutes(route, context, feature)`
  （`components/hostlink/include/route.h`，不依赖 `esp_err`，主机测试可包含）：
  `hello`/`fs`/`sys` 之外的命令交给路由；路由认领则填响应，否则仍回
  `UNSUPPORTED_FEATURE`；`hello` 的 `features` 追加注册时的能力名。
- 应用层把共享 I2C 总线提前到 FRAM 之前创建（FRAM 与 PN532 共用），
  新增 `CONFIG_STORYBOX_NFC`（默认 y）：PN532 初始化成功才设置轮询，
  失败只记一条日志并让 `nfc` 命令回 `NFC_UNAVAILABLE`，不影响启动。
- `cmd:"nfc", act:"status"` 返回：模块在线、轮询开关、固件版本、卡片是否存在、
  卡片 `kind/kindName/writable/formatted/capacity/uid`；`read/write` 仍回
  `NOT_SUPPORTED`（原生服务还没有读写 API，不假装支持）。
- 构建：产品配置（含 BLE/NFC）`0x1c5e00`，产品 + USB + NFC `0x1cbad0`。
- 主机测试脚本：`tools/test_native_protocol.sh` 也加上了 Windows 的 POSIX shim；
  该测试在 MinGW 下能编译并跑到写会话步骤，写会话之后依赖 POSIX 语义，
  仍需 Linux/CI 执行。

### P5（第二批）：PIN 门禁与家长设置页

- 设置页新增「家长设置」入口（第 8 项，绑定码后移到第 9 项）：
  - 已设 PIN 且本次未解锁时先进 PIN 页；解锁状态在应用内保留 5 分钟；
  - 家长记录不可用时入口显示「不可用」并拒绝进入。
- 新增 PIN 输入页（无数字键盘的兜底交互）：旋钮改当前位、OK 追加一位、
  SET 短按退格、OK 长按提交（≥4 位）、SET 长按取消。用于「进入家长设置」
  （校验）与「修改密码」（设置）两种用途。
- 新增家长设置页（4 行窗口 + 滚动条）：
  - 每日时长（0/15/30/45/60/90/120 分钟）、音量上限（3..21，自动夹住下限）、
    就寝时段开关、就寝开始/结束（15 分钟步进）、远程锁开关；
  - 修改密码、解绑、恢复默认（恢复默认会重新生成绑定码，保留 PIN 标志位语义）。
  - 每次调整立即经 `ParentalStore::save()` 写回并重新 `ParentalPolicy::begin()`，
    保存失败会提示且不改变内存状态（`parentalApply()` 先校验再落盘）。
- 语义说明：PIN 是本地门禁（盐 + FNV），不是安全边界；真正的配对秘密仍是 BLE token。
- 构建：产品配置 `storybox.bin` = `0x1c7030`。
- 尚未迁移：白名单逐项勾选 UI、就寝时段到点后的锁页（目前锁定只阻止播放并提示）。
  BLE 的 `set`/`pin set`/`reset`/`unbind` 见 P4 第五批。

### P6（第一批）：系统监控 `sys stats` / `sys set`

桌面端「设备中心」读取的是 `cmd:"sys", act:"stats"`，字段见
`desktop/src/shared/types.ts` 的 `DeviceSystemStats`。本轮把这条链路接上：

- hostlink 路由扩展：核心路由之外的命令（含 `sys` 的 `stats`/`set`）交给注册路由，
  `sys ping`/`sys hello` 行为保持不变；路由拿到解析后的请求对象，可读 `set` 的补丁字段。
- 固件返回真实的：`uptime`、CPU 核数/频率/型号/修订/IDF 版本、
  内部堆总量/空闲/历史最低、PSRAM 总量/空闲、Flash 容量、运行分区的
  `appSize`（`esp_image_get_metadata`）/`appTotal`/`appFree`/分区名、
  TF 卡总量/已用/空闲（`esp_vfs_fat_info`）与曲目/故事数、
  当前设置（音量/亮度/旋转/EQ/熄屏/休眠/蓝牙/WiFi/AP/童锁/绑定）、
  播放器快照（播放态/曲名/码率/采样率/进度/音量）。
- `sys set` 支持桌面端下发 `volume` 与 `brightness`（亮度写入设置并在 10..100 内夹取）。
- 明确未实现：CPU 负载（需要 FreeRTOS run-time stats，目前固定 0）、
  网络状态（产品还没启动 WiFi，固定 false/空串）、播放器总时长（解码器不提供）。
- 构建：产品 + USB + NFC `storybox.bin` = `0x1ce970`；
  主机协议测试跑过 hello/ping/fs 读的既有断言，之后仍卡在 POSIX-only 的写会话夹具。

### P6（第二批）：熄屏与深睡

- 新增 `components/power`：
  - `power_policy.h`：纯函数空闲策略（主机可测）——主机传输/BLE 连接保持全醒；
    视频播放不熄屏不休眠；播放音频只熄屏；静音且两个窗口都到点才深睡；
    0 分钟表示禁用该项。
  - `power_service.cpp`：背光 0 熄屏、任意输入边沿亮屏并 `Input::swallowFor(600)`
    吞掉唤醒那一下的按键；深睡用 EXT1（ENC_A/ENC_B/ENC_SW）低电平唤醒，
    休眠前开 `ESP_PD_DOMAIN_RTC_PERIPH` 保上拉，避免引脚浮空立刻唤醒。
- 应用接入：主循环每帧 `Power::loop()`，熄屏时跳过绘制/推屏但继续采样输入；
  从熄屏恢复时强制整页重绘；视频页在进入/离开时置 `videoActive`，
  保证看视频时不会中途熄屏。
- **深睡默认关闭**（`CONFIG_STORYBOX_POWER_SLEEP=n`）：唤醒依赖编码器三个引脚，
  未确认接线就开启会有“只能重新烧录才能唤醒”的风险。新增 `sdkconfig.power`
  用于验证该分支。
- 验证：`test/power_policy_test.cpp`（并入 `tools/test_host.sh`）覆盖窗口边界、
  播放/视频/主机活跃、0 分钟禁用；产品配置 `0x1c73d0`，深睡配置 `0x1c9e60`
  （`sdkconfig.power`）。真机熄屏电流、唤醒灵敏度与深睡电流未测。

### P6（第三批）：OTA 双槽分区

- 新增 [partitions.ota.csv](partitions.ota.csv)：16 MB Flash 的双槽布局 ——
  `nvs` 0x9000、`otadata` 0xf000（IDF 引导器要求这个位置）、`phy_init` 0x11000、
  `ota_0` 0x20000 / 3 MiB、`ota_1` 0x320000 / 3 MiB。当前镜像 1.9 MB，
  每槽余量 41%。
- 新增 `sdkconfig.ota` 选择该分区表（`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`）。
  构建已验证：`storybox.bin` = `0x1c73d0`，最小 app 分区 `0x300000`，
  且生成 `ota_data_initial.bin`（烧录地址 0xf000）。
- **刷写风险已写明**：这是分区表变更，需要整片擦除并重新初始化的 NVS；
  FRAM 与 TF 卡内容不受影响。默认分区表仍是单槽 `factory`，不影响现有设备。
- 尚未实现：升级数据通道（USB 的 raw 通道目前只允许 `fs write_chunk`，
  需要给 OTA 单开一条通道）、`esp_ota` 起停封装、回滚策略
  （`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 需要固件启动后调用
  `esp_ota_mark_app_valid_cancel_rollback()`，本轮未开启）。

### P6（第四批）：OTA 升级通道

- 新增 `components/ota`：`begin(expected)/write/finish/abort/status`，封装
  `esp_ota_begin/write/end/set_boot_partition`；单槽布局下 `begin` 返回
  `ESP_ERR_NOT_SUPPORTED`，不会假装能升级。
- hostlink 的 raw 通道从“只允许 fs write_chunk”扩展为也允许
  `sys ota_chunk`；路由回调新增 raw 指针/长度参数，OTA 分块直接吃二进制帧。
- `sys` 路由新增动作：`ota_begin`（带 `size`）、`ota_chunk`（raw 分块）、
  `ota_end`（切槽，等重启生效）、`ota_abort`；每次 `sys` 响应都附
  `ota: {supported, active, written, expected}`，客户端可显示进度。
- 验证：`sdkconfig.defaults + sdkconfig.usb-product + sdkconfig.ota` 组合构建通过，
  `storybox.bin` = `0x1d0f40`，槽位 3 MiB 余量 39%，`Ota::status` 等符号确认被链接。
- 未做：回滚（未开 `APP_ROLLBACK_ENABLE`）、桌面端 OTA 上传界面、真机升级与断电失败演练。

### P6（第五批）：OTA 回滚确认

- `sdkconfig.ota` 打开 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`；`app_main`
  在入口自检（Flash/PSRAM/USB）通过后，若运行分区处于 `PENDING_VERIFY`
  就调用 `esp_ota_mark_app_valid_cancel_rollback()` 确认镜像。
- 语义：新镜像启动若在确认前 panic，下次复位由引导器回滚到旧槽；
  确认后不再回滚。日志会打 `OTA image confirmed` 或确认失败告警。
- 验证：`defaults + usb-product + ota` 组合构建通过 —— 引导器 `0x5780`（含回滚逻辑）、
  `storybox.bin` = `0x1d1470`（3 MiB 槽余量 39%），配置中
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` 已生效；默认单槽配置仍为 `0x1c73d0`。
- 仍需真机演练：升级成功、故意断电、启动 panic 三种路径的回滚行为。

### P6（第六批）：`sys set` 补字段

- `sys set` 在原有 `volume`/`brightness` 之外支持 `eq`（0..3，越界忽略）与
  `rotation`（1 或 3，其它值忽略），后者立即 `Display::setRotation()` 并落盘；
  两处都走与设置页相同的 `saveSettings()` 路径。
- 构建：产品 + USB + NFC `storybox.bin` = `0x1d0fb0`。

### 跨配置回归构建（2026-11-04）

本机（IDF 5.5.1，`build-all.ps1` 本地脚本，未入库）一次性构建了当前全部配置：

| 配置 | 片段 | `storybox.bin` |
|---|---|---|
| 默认产品（BLE+NFC） | `sdkconfig.defaults` | `0x1C73D0` (1.86 MB) |
| 产品 + USB 协议 | `+sdkconfig.usb-product` | `0x1D0FB0` (1.90 MB) |
| 产品 + USB + OTA 双槽 | `+sdkconfig.ota` | `0x1D14D0` (1.91 MB) |
| 深睡 | `+sdkconfig.power` | `0x1C9E60` (1.88 MB) |
| 确认 FRAM | `+test/sdkconfig.fram-compile-only` | `0x1C95E0` (1.87 MB) |

全部链接通过；OTA 配置的最小 app 分区 3 MiB，余量约 39%。

### 真机验收清单

迁移的功能面已经铺开，但**没有任何真机结论**。新增
[真机验收清单](idf-hardware-acceptance.md)，按「显示/输入 → 音乐 → 故事 → 视频 →
BLE → USB → NFC → 家长 → 电源 → OTA」十组给出可执行步骤、预期结果、
失败信号和回填模板。后续每完成一组，把结论回填到本文件对应批次。

### P4（第四批）：`sys rescan`

- `sys` 路由新增 `rescan`：SD 已挂载时重扫媒体、回写索引并刷新快照；
  未挂载返回 `NO_SD`，扫描失败返回 `RESCAN_FAILED`。用于「卡被 PC 改过」或
  上位机外部写入后的强制刷新（正常上传路径已有基于存储序号的自动重扫）。
- 构建：产品 + USB + NFC `storybox.bin` = `0x1d1020`。

### P6（第七批）：`sys reboot`

- `sys` 新增 `reboot`：先用 500 ms 一次性定时器把响应送出，再 `esp_restart()`；
  配合 `ota_end` 切槽后免拔电即可应用新固件。
- 构建：产品 + USB + NFC `storybox.bin` = `0x1d1090`。

### P4（第五批）：BLE 家长配置、列表与门禁

把控制器小程序剩下的命令补齐，让 BLE 协议面与旧固件（`src/ble.cpp`）对齐。
小程序源码不在本仓库，线协议是照旧固件逐字段对照迁移的。

- 新增 `components/settings/include/parental_config.h`（纯头文件，可主机测试）：
  - `ParentalPatch`：`set` 的部分补丁语义，`has_*` 区分“没带这个字段”和“显式设为 0/false”；
    `applyParentalPatch()` 用旧固件的夹取规则（时长 0 或 5..240 分钟、音量 0..21 且自动排序、
    就寝分钟越界归零、白名单掩码、远程锁/童锁标志）。
  - `ParentalStatus`：手机读到的扁平状态；`parentalLockCode()` 保留旧固件的数字编号
    （None=0 / DailyLimit=1 / Bedtime=2 / Remote=3），原生新增的两个状态取后位
    （ClockRequired=4、Unavailable=5），老客户端只认 0..3 并显示 `reason` 文本。
  - `parentalLockText()`：把中文锁定原因从 app 层搬进来，界面和 BLE 共用一份。
- `parental_policy.h` 新增 `rebind()` 与 `timeSynced()`：鉴权后的编辑在落盘后**保留可信时钟
  和用量累加器**。此前 `parentalApply()` 走 `begin()`，改一次设置就清掉 `synced_`，
  于是「开了就寝时段 + 已校时」的机器改完设置立刻变成 `ClockRequired` 并阻止播放
  （设备上原本没有第二个校时入口）。本轮顺手修掉这个真实缺陷；`begin()` 语义未变。
- BLE 新命令（都在 `auth` 之后，与旧固件一致）：
  - `set`：每日时长、音量下限/上限、就寝开关与起止、白名单开关、故事/分类白名单数组、
    远程锁、童锁。无识别字段回 `NO_FIELDS`，落盘失败回 `SAVE`，记录不可用回 `UNAVAILABLE`，
    成功时返回**落盘后重新读出**的 `data` 配置，而不是手机发来的值。
  - `pin set`：4..6 位数字，格式错误回 `PIN_FORMAT`。
  - `reset`：恢复默认（配额/音量范围/就寝窗口/掩码/远程锁/童锁，重新生成绑定码），
    并强制重新配对。**与旧固件的差异**：保留 PIN 与用量历史（旧固件 `factoryReset` 会清掉
    PIN 和收藏），因为本机没有「重置后如何重新设 PIN」的入口；收藏也不清。
  - `unbind`：只解除绑定，保留配额/掩码/PIN。
  - `get state` / `get config` / 每秒 `evt:state` 补全旧固件字段：`used/limit/rem/
    lock(数字)/reason/bedtime/remote/childLock/bound/hasPin/timeSynced/dayKey`，
    config 另含 `dailyLimitMin/bedStartMin/bedEndMin/allowlistEnabled/vol`。
  - `ctrl` 增加门禁：`play/toggle/next/prev/story/music` 受锁定与白名单约束，分别回
    `LOCKED`/`NOT_ALLOWED`/`RANGE`；`ctrl fav` 返回 `f`（新状态）与 `n`（总数），
    锁定时仍允许（收藏属于元数据）。
  - `get stories|music|cats|report` 由应用侧填页（`limit` 上限 4，与旧固件一致），
    不再返回空列表占位。
- 分层：BLE 只做协议，`Ble::Hooks` 新增 `status/apply_config/set_pin/factory_reset/unbind/
  gate/list/toggle_favorite`，动作类回调返回错误码字符串（`SAVE`/`UNAVAILABLE`/`PIN_FORMAT`），
  不再需要「给 `Ble::Command` 加字符串载荷」——家长配置在应用任务里同步落盘（FRAM 写 +
  回读校验约几毫秒），和设置页走同一条 `parentalApply()` 路径。
- 验证：新增 `test/parental_config_test.cpp` 并并入 `tools/test_host.sh`，覆盖夹取、掩码、
  锁编号、剩余时长、`rebind()` 保留时钟与用量、非法记录不被采纳；本机 MinGW 通过。
  五套配置重新构建通过（下表）。`transfer_frame` 主机测试在本机 MinGW 下仍以访问冲突退出
  （与本轮改动无关，测试源码未动），需在 Linux/CI 上执行。
- **仍未做**：真机与小程序联调、广播字段长度实测、白名单逐项勾选 UI、就寝到点的独立锁页。

### 回归构建（BLE 家长命令之后，2026-11-04）

| 配置 | 片段 | `storybox.bin` |
|---|---|---|
| 默认产品（BLE+NFC） | `sdkconfig.defaults` | `0x1C8390` (1.86 MB) |
| 产品 + USB 协议 | `+sdkconfig.usb-product` | `0x1D1FB0` (1.90 MB) |
| 产品 + USB + OTA 双槽 | `+sdkconfig.ota` | `0x1D24E0` (1.91 MB) |
| 深睡 | `+sdkconfig.power` | `0x1CAE10` (1.88 MB) |
| 确认 FRAM | `+test/sdkconfig.fram-compile-only` | `0x1CAB40` (1.87 MB) |

全部链接通过；OTA 配置最小 app 分区 3 MiB，余量约 39%。本轮比上一轮每套配置约 +4 KiB
（确认 FRAM 那套 +5.5 KiB，因为家长命令的真实分支只有它编译）。

## 2026-10-29：M0 启动 / M1 诊断骨架

迁移分支：`feature/esp-idf-migration`。
旧源码基线：`988ed5f845ebc2f1fc6f01db2fc75c437577ec2b`。
该提交保存旧源码、PlatformIO 配置和 factory 分区表；不等于已有可恢复的固件备份。
原计划所述“没有 Git 仓库”已不适用于当前工程。

新增根 CMake、原生 `app_main()`、板型选择和 N16R8 配置；产品功能尚未接入。
启动只报告板型、IDF、复位原因、Flash、PSRAM 和内部内存，不访问 FRAM 或 TF 卡。
深睡未启用。USB 目前仅为诊断日志，不实现 JSON v2 或宣称设备能力。
旧 PlatformIO 工程继续使用 `src/`；原生工程只编译 `main/`。

## 构建

当前验证版本为 IDF 5.5.1（源码 commit
`fcae32885b0296b32044cb99ecbdc50d98dddb83`），CI 使用相同版本。
组件管理器生成 dependencies.lock；第三方显示、解码、AFE 尚未加入联合验证。

在安装并导出 IDF 5.5.x 的 shell 中，从仓库根目录执行：

```sh
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

在 StoryBox migration 中按实物选择板型。默认 legacy 仅为候选，不代表已确认。
当前沿用旧 factory 分区布局，OTA 双槽布局在容量预检后确定。
首次烧录前备份旧固件、TF 卡和非易失数据，核对 Flash/PSRAM 与恢复入口。

## 阶段退出条件（未完成）

- M0：确认首块验收板、FRAM 丝印及地址帧；保存旧固件和真实数据/协议样本。
- M0：记录旧固件启动、播放、切图、传输和内存基线。
- M1：已在工作区安装 IDF 5.5.1，原生构建通过；诊断镜像为 203648 字节。
- M1：锁定补丁版本，验证 LovyanGFX、解码器、原生 I2S 与 AFE 共同链接。
- M1：完成真板 USB、PSRAM、显示、PCM 与资源测量。

新板 FRAM 候选为 FM24CL64B / 0x51 / 两字节地址 / 8192 字节；
legacy 寻址与新板不可混用。当前入口不启用持久化驱动，不通过 ACK 猜测器件。

## 首批基础组件实施

- `components/board`：明确两类板型和 FRAM 地址编码；驱动使用原生 I2C
  master device API，返回读写错误，按旧板 256 字节边界和 64 字节事务拆分。
  必须传入已确认硬件状态才能初始化。总线由调用方管理；没有自动探测板型。
- `components/input`：原有按键逻辑增加原生 GPIO/单调时钟路径；编码器 ISR
  和任务共用跨核临界区。业务仍要求由单一输入任务调用。
- `components/storage`：SDMMC 1-bit/VFS 挂载底层，不自动格式化，不使用 DAT3
  判断插卡。文件许可、维护排他与媒体适配尚未实现。
- `components/audio`：16-bit stereo PCM 原生 I2S TX；支持有界等待、部分写入
  和关闭资源。尚未实现解码、EQ、播放状态机、功放使能及停止确认协议。
- 新驱动参与构建，但诊断入口尚未启动这些外设，避免把编译通过等同功能恢复。
- `bash tools/test_host.sh`：板型边界、渲染、CRC、传输帧、目录故障注入均通过。
- `.github/workflows/esp-idf.yml`：固定 IDF 5.5.1 构建及主机测试；尚未推送运行。

旧 Arduino 构建未重新验证。真机、数据兼容、显示、完整播放和协议验收仍待完成。

## 第二批：显示、共享总线、文件生命周期

LovyanGFX 固定为 1.2.7 / `5438181440c71cf30bbdc347b0b1597ae3ebf77d`。
原生构建保留 SPI、PSRAM 画布、RGB565 类型、条带 DMA 完成等待和中文字体；
背光改用原生 LEDC，支持 0% 熄屏。霍尔改用 GPIO。
该版本未使用的 S3 RGB 后端引用 IDF 5.5 已删除的
`gpio_hal_iomux_func_sel`，根 CMake 显式排除该源文件；未修改生成的依赖。
今后使用 RGB 屏必须重新评估该限制。

共享 I2C 总线由独立 owner 创建和恢复；恢复前仍要求上层暂停所有设备访问。
文件访问许可随文件句柄保留，维护开始时禁止新增访问，等待已有访问归还，超时恢复准入。
标准 C++ 文件适配可直接用于 IndexIO，主机测试验证中文标题、校验和及完整文件消费。

上传事务底层使用独占临时文件、4096 字节块和完成后替换；取消仅删除本次临时文件。
覆盖前将旧文件移至备份，提交失败尝试恢复；发现残留临时文件/备份返回恢复要求。
尚未接入 USB 协议、重启恢复决策、媒体重建和音频停止确认，不能视为文件管理功能完成。
FAT 断电一致性及 fsync 行为须真机验证。

诊断版本（默认关闭）可实际初始化屏幕、输入、霍尔及 SD，显示中文诊断页：

```sh
idf.py -B build-diagnostics -D SDKCONFIG=build-diagnostics/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.diagnostics' build
```

启用前核对板卡接线。该镜像不会启动完整播放器或 FRAM 写入。
主机测试增加文件维护互斥、上传取消、重复块、不完整提交和残留备份检测。

验证结果：默认固件与启用外设的诊断固件均通过 IDF 5.5.1 编译链接，
诊断镜像大小为 `0xa8280` 字节；7 组主机测试通过，`git diff --check` 通过。
尚未烧录或进行硬件验收。完整音频、原有业务页面和通信功能仍未迁移完成。

## 第三批：MP3 解码与播放任务

固定 `espressif/esp_audio_codec` 为 2.6.2，使用 MP3 simple decoder 解析任意输入块；
独立解码器不接管 I2S。依赖含预编译芯片库，许可证为 Espressif Modified MIT，
仅用于 Espressif 产品；保留组件许可证，未将库用于主机解码测试。

新增 `Mp3Stream` 和应用生命周期的 `Playback` 服务：

- 文件句柄全程持有存储许可；保留尚未消费的压缩输入。
- 固定 4096 字节输入和 4608 字节 PCM 缓冲；单声道扩展为双声道。
- 单音频任务持有解码器和 I2S；队列满向调用方返回超时，不静默丢命令。
- 请求序号、播放代次及自有路径缓冲进入状态快照；不暴露解码器内存指针。
- 支持载入暂停、播放、暂停、重新载入、停止和错误状态。
- 停止先禁止新播放，已排队播放也被拦截；关闭 I2S 和文件后才确认。
- 支持 PCM 部分写入，持续无进展两秒报错；EOF 先等待 DMA 尾部再释放。
- 音量范围 0～21，0 精确静音，初始 5；当前增益曲线为待实机标定版本。

播放进度字段为 `submitted_frames`，不是已校准的扬声器播放时刻。
暂停停止继续提交 PCM，但已进入 DMA 的数据仍会播放；四个 240-frame 描述符
的实际暂停/EOF 延迟需上板测量。当前不支持流内采样率切换，不宣称已完成
EQ、总时长/VBR 元数据、自动下一曲、故事同步或音频路线完整验收。

音频诊断构建：

```sh
idf.py -B build-audio -D SDKCONFIG=build-audio/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.audio-diagnostics' build
```

默认载入 `/music/test.mp3` 并暂停；OK 切换播放/暂停，编码器调音量，SET 长按停止，
SET 短按重新载入。文件路径可在 menuconfig 修改。新板功放使能极性未确认，
不驱动 GPIO38；烧录前须核对板型及功放线路。

验证：含显示、SD、MP3 解码与 I2S 的音频诊断镜像已链接成功（`0xc03c0` 字节）。
`tools/test_mp3_stream.sh` 使用实际组件头文件和生产适配源码，替换 codec C API
注入无进展、越界消费和非法通道数，验证输入保留、单声道转换及许可释放。
该测试不执行真实解码算法。8 组通用主机测试及该适配测试通过；真机验证待执行。
