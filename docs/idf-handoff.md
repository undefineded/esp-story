# 交接文档：esp-story ESP-IDF 迁移

> 写于 2026-11-04，用于在新会话继续。**先读完这页再看 `docs/idf-migration-progress.md` 的批次记录。**

## 1. 一句话状态

`feature/esp-idf-migration` 分支上 P1–P6 主体 + BLE 家长命令尾巴都已完成，**已 push 到 origin**
（工作区干净时先推了 31 个提交，本轮 BLE 改动另提交），五套配置的编译层回归全部通过；
**仍然没有任何真机验收结论**，另有 5 类功能尾巴未收。

## 2. 环境（本机）

| 项 | 值 |
|---|---|
| ESP-IDF | 5.5.1，`C:\esp\v5.5.1\esp-idf`（commit `fcae3288`） |
| 工具链 | `xtensa-esp-elf 14.2.0`、cmake 3.30.2、ninja 1.12.1，均在 `C:\Users\a1509\.espressif\tools` |
| Python | 没有系统 Python；IDF 环境手工建在 `~/.espressif/python_env/idf5.5_py3.11_env`（base 是 PlatformIO 的 venv） |
| 组件依赖 | LovyanGFX 1.2.7、esp_audio_codec 2.6.2，`managed_components/` |
| 主机编译器 | w64devkit GCC 16.2：`C:\tools\w64devkit\w64devkit\bin`（用于 `tools/test_host.sh`） |
| 项目路径 | `C:\Users\a1509\Documents\PlatformIO\Projects\music-demo` |

### 构建入口（必须用这两个脚本，`idf.py -D` 在本机不可用）

```powershell
# 单次构建（本机脚本带 Zone.Identifier，必须显式跳过执行策略）
powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 -B build-ui --define-cache-entry SDKCONFIG=build-ui/sdkconfig build

# 全配置回归（默认 / usb-product / ota+usb / power / fram）
powershell -ExecutionPolicy Bypass -File tools\idf\build-all.ps1

# 主机测试（IDF_PATH 指向 esp-idf，PATH 需要 w64devkit 的 bin）
$env:IDF_PATH='C:\esp\v5.5.1\esp-idf'
$env:PATH='C:\tools\w64devkit\w64devkit\bin;'+$env:PATH
& 'C:\Program Files\Git\bin\bash.exe' -c "cd /c/Users/a1509/Documents/PlatformIO/Projects/music-demo && bash tools/test_host.sh"
```

`tools/idf/` 在 `.gitignore` 里（含本机绝对路径），**换机或清理后需要重建**，脚本内容见本文件附录。

## 3. 目录结构

```
components/
  app/         UI 任务、页面栈、主题、所有页面（home/settings/music/story/video/parental/pin/bind）
  audio/       MP3 解码 + I2S + EQ（Playback 服务）
  ble/         NimBLE GATT + JSON 协议 + 命令队列
  board/       板型/FRAM/I2C 总线
  display/     LovyanGFX 封装 + JPEG 故事图（复用 src/display.cpp）
  hostlink/    USB v4 传输、JSON 路由、文件会话、可插拔 Route
  media/       目录扫描、索引 v3、Playlist、LRC 时间轴
  nfc/         PN532 轮询服务（复用 src/pn532.cpp、src/nfc.cpp）
  ota/         esp_ota 封装（begin/write/finish/abort/status）
  power/       空闲策略（纯函数 + 主机测试）与熄屏/深睡服务
  settings/    MPL3/4、音量、MPS2、FAV、PCTL 记录、绑定核心（parental_binding.h）
               与 BLE 家长命令的补丁/状态视图（parental_config.h）
  storage/     SD 挂载、AccessGate、FileReader、上传事务
  video/       AVI 索引、视频目录扫描
src/           Arduino 双构建源码（display/input/fonts/hall/nfc/pn532 被 IDF 组件复用）
main/          app_main、Kconfig、诊断入口
docs/          idf-migration-progress.md（批次记录）、idf-hardware-acceptance.md（验收清单）
sdkconfig.*    默认 / usb-product / usb-diagnostics / diagnostics / audio / power / ota 片段
```

## 4. 已完成（按 P 序列）

- **P1** UI 任务（16 KiB 栈、prio 3、20 ms 帧）、页面栈（最深 6 层）、首页四车票 + 烘焙插画、
  设置页亮度/旋转/EQ/熄屏/休眠、FRAM 持久化（未确认时只改 RAM）。
- **P2** 音乐分类→歌曲→播放；故事递归 6 层、有 `.lrc` 的目录算图文故事、否则每 MP3 一个音频故事；
  索引 v3（读 v2/v3）、故事目录逐级浏览、图文按 LRC 切图、音频故事共用播放视图。
- **P3** AVI（RIFF/hdrl/movi）索引解析、视频列表与播放页、QOI/JPEG 解码、25 kHz 背光默认值。
- **P4** NimBLE：相同 128 位 UUID、hello/bind/auth/sync/get state/ctrl、1 Hz `evt:state`；
  USB 可写 FileReader（声明 `fs_transfer`）、上传后按存储序号自动重扫、`sys rescan`；
  `sys reboot`；NFC `status` 路由。
- **P5** 绑定码/token（FNV）、PIN（盐 + FNV）、播放门禁、`parentalApply()` 先校验后落盘、
  PIN 输入页（旋钮输数字）、家长设置页（时长/音量上限/就寝/远程锁/改 PIN/解绑/恢复默认）。
- **P6** `sys stats`（对齐桌面端 `DeviceSystemStats`）、`sys set`（音量/亮度/EQ/旋转）、
  熄屏 + 可选深睡（EXT1 唤醒，默认关闭）、双槽 OTA 分区 + 上传通道 + 回滚确认。
- **P4 第五批（最新）** BLE 家长配置尾巴：`set` / `pin set` / `reset` / `unbind`、
  `get stories/music/cats/report`、`ctrl fav`、`ctrl` 门禁（`LOCKED`/`NOT_ALLOWED`/`RANGE`）、
  `get state`/`get config`/`evt:state` 补全旧固件字段。新增
  `components/settings/include/parental_config.h`（补丁 + 状态视图 + 锁编号），
  `ParentalPolicy::rebind()` 让鉴权后的编辑不再丢可信时钟。

最近 8 个提交（本轮的 BLE 提交不列在这里，用 `git log` 查）：

```
0035e71 docs: add a handoff document for continuing the migration in a new session
ed83d0a sys reboot 免拔电应用 OTA
e4fbe5e docs: sys rescan 记录
641f7af sys rescan
5790aea 真机验收清单
8533ecb 跨配置回归矩阵
c8b5a71 sys set 支持 EQ/旋转
d7eafd7 OTA 回滚确认
```

## 5. 下一步（按建议顺序）

1. **真机验收**（现在就可以开始）：照 `docs/idf-hardware-acceptance.md` 十组跑，结论回填
   `idf-migration-progress.md`。这一步会暴露最可能的真问题：BLE 广告字段是否超 31 字节、
   I2S 与功放使能、JPEG/QOI 解码、视频帧率、深睡唤醒、OTA 三种路径。
2. 功能尾巴（按价值排序）：
   - BLE / 小程序联调：固件侧命令已齐（`set`/`pin set`/`reset`/`unbind`/列表/收藏/门禁），
     **没有任何真机或协议实测**；联调时重点看 `evt:state` 的 1 Hz 报文长度与 MTU 协商；
   - NFC 卡读写 API + 事件通道（`Nfc::Service` 现在只出快照，事件被丢弃）；
   - USB `mkdir`/`remove`/`rename` 与容量字段（`FileReader` 已有写事务基础）；
   - 桌面端 OTA 上传界面（固件侧通道已就绪：`sys ota_begin/ota_chunk/ota_end/reboot`）；
   - 音视频漂移校正、视频丢帧统计；
   - 白名单逐项 UI、独立锁定页。

## 6. 已知坑（都踩过，别再踩）

1. **`sdkconfig.defaults` / 片段的改动对已存在的 `build-*/sdkconfig` 不生效**。改片段后必须删除
   对应 `build-xxx/sdkconfig` 再构建，否则会得到“改了配置但二进制没变”的假象（OTA 回滚、BLE
   都踩过）。
2. **本机 `idf.py -D` 短选项会被当成构建目标**，必须用 `--define-cache-entry`（`tools/idf/idf.ps1` 已封装）。
3. **组件管理器哈希**：本机 `core.autocrlf=true` 会让 LovyanGFX 检出内容哈希不匹配 →
   构建脚本用 `GIT_CONFIG_*` 临时关掉；删 `managed_components/` 重拉即可恢复。
4. **hostlink 路由签名改过两次**（加了 request、raw 指针）；`route.h` 特意不含 `esp_err.h`，
   否则主机协议测试编不过。
5. **主机测试是 POSIX 语义**：Windows 上 `file_reader`/`native_protocol` 会在 symlink、带引号文件名、
   写会话（rename 覆盖）处失败；`test/host_compat.h` 只补了 `fsync/lstat/symlink`。
   能跑通的子集：`storage_access`、`media_cache`（前半）、`avi_index`、`parental_binding`、`power_policy`。
6. **`sdkconfig.ota` 会整片擦除**（分区表从单槽 factory 变 ota_0/ota_1），FRAM 与 TF 卡不受影响。
7. **`Pn532::errorName` 只在 .cpp 内定义**，头文件没有声明；NFC 路由里不要用它。
8. **中文源码**：用 `write`/`edit` 工具改文件；不要用 PowerShell `Get-Content -Raw` + `Set-Content`
   重写（会按 ANSI 解码而损坏 UTF-8）。
9. **`tools/idf/*.ps1` 带 Zone.Identifier**，`powershell -File` 会被执行策略拦下
   （报 “is not digitally signed”）。必须写成
   `powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 ...`。
10. **`build-all.ps1` 把编译输出丢进 Out-Null**，失败时只有一行 `FAILED`。要看编译错误就单独跑
   某一套配置，或改成把输出落到日志再读尾巴。
11. **主机测试在本机只能跑子集**：`tools/test_host.sh` 会在 `file_reader` 处断言失败（POSIX 语义），
   而 `transfer_frame` 会以访问冲突（0xC0000005）退出。逐个编译/运行能过的那些：
   `board_profile storage_access settings_record media_records parental_record parental_binding
   parental_config story_timeline pn532_frame render_math transfer_crc`。

## 6.1 BLE 协议要点（本轮新增）

- 线协议照旧固件 `src/ble.cpp` 对照迁移，小程序源码不在本仓库。
- `set` 是**部分补丁**：只带想改的字段；无识别字段回 `NO_FIELDS`。
- `lock` 是数字：None=0 / DailyLimit=1 / Bedtime=2 / Remote=3，原生新增
  ClockRequired=4、Unavailable=5；中文原因在 `reason` 字符串里。
- `reset` 保留 PIN 与用量历史（旧固件会清 PIN 和收藏），并强制重新配对；
  `unbind` 只清 token。
- 应用侧同步落盘（FRAM 写 + 回读校验），所以 BLE 没有单独的配置命令队列。

## 7. 事实边界

- 所有尺寸数据来自编译产物（BLE 家长命令之后：默认 `0x1C8390`、usb-product `0x1D1FB0`、
  OTA+usb `0x1D24E0`、深睡 `0x1CAE10`、确认 FRAM `0x1CAB40`；**以重新构建为准**）。
- **没有任何真机数据**：电流、帧率、配对、BLE 报文、NFC、USB 传输、OTA 升级/回滚均未实测。
- 桌面端（`desktop/`）只改过一处：`FileManagerPage` 接受 `fs/fs_transfer/fs_readonly` 三种能力。
- 小程序（控制器）源码不在本仓库；BLE 的线协议兼容性只做了源码级对照，没有联调。

## 附录：`tools/idf/` 脚本（未入库，换机需重建）

`idf.ps1` 要点：设置 `IDF_PATH`/`IDF_TOOLS_PATH`/代理与 `GIT_CONFIG_*`，调用
`python tools/idf_tools.py export --format key-value` 解析环境变量，再执行
`python tools/idf.py @args`。

`build-all.ps1` 要点：定义 `Build(name, dir, defaults)`，依次构建
`build-ui`（defaults）、`build-usb-product`（+usb-product）、`build-ota-usb`（+usb-product;ota）、
`build-power`（+power）、`build-fram`（+fram-compile-only），最后打印各 `storybox.bin` 大小。

（可直接按本文件第 2 节的命令重建，或让新会话的 agent 重新生成这两个脚本。）
