# 真机验收清单（迁移分支）

本文件是**可执行**的验收步骤。迁移分支（`feature/esp-idf-migration`，领先 `main` 28 个提交）
目前**只有编译层证据**，下面每一项都还没有真机结论。建议按顺序做，每完成一组把结论填进
[验收记录](idf-acceptance-log.md)，再回填到 `docs/idf-migration-progress.md` 对应批次。

先决条件：一块确认过板型的 S3 板（N16R8）、PN532 模块、已格式化的 TF 卡、一根能供电的 USB 线。

## 0. 环境与烧录

本机唯一可用的构建入口是 `tools/idf/idf.ps1`：直接调 `idf.py` 会缺 IDF 环境，`-D` 短选项会被
当成构建目标（必须用 `--define-cache-entry`），脚本带 Zone.Identifier 所以必须
`-ExecutionPolicy Bypass`。以下命令都在仓库根目录执行。

### 0.1 镜像（2026-11-04 第二轮，含 FRAM / UI / BLE 修复）

| 配置 | 构建目录 | `storybox.bin` | SHA256 前 16 位 | 用于 |
|---|---|---|---|---|
| 默认产品（BLE + NFC，单槽，**FRAM 已开**） | `build-ui` | `0x1FA310` | `A4E636111788868E` | 第 0/1/2/3/4/5/7/8 组，**已在板子上** |
| 产品 + USB 协议 | `build-usb-product` | `0x203540` | `96D65A0BA0D0BB67` | 第 6 组；也用于 FRAM 写读复测 |
| 产品 + USB + OTA 双槽 | `build-ota-usb` | 待重建 | — | 第 10 组，**改分区表，整片擦除** |
| 深睡 | `build-power` | 待重建 | — | 第 9 组 |
| 确认 FRAM（编译覆盖） | `build-fram` | 历史产物 | — | 默认已开 FRAM，这套只剩历史意义 |

默认镜像现在会真的写 FRAM（设置/音量/收藏/家长记录）。`FramDevice` 启动时会先做
8 bank 写读探测，探测不通过会退回 RAM-only 并打 `FRAM` 警告，不会写坏记录。

### 0.2 烧录与监视

```powershell
# 插上板子后找串口
Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Description

# 烧录并监视（COMx 换成实际端口）
powershell -ExecutionPolicy Bypass -File tools\idf\idf.ps1 -B build-ui --define-cache-entry SDKCONFIG=build-ui/sdkconfig -p COMx flash monitor
```

`Ctrl+]` 退出 monitor。要留档就另开一个窗口跑 monitor，把关键日志粘进
[idf-acceptance-log.md](idf-acceptance-log.md)（长日志放在仓库外的 `logs/`，不要提交）。

### 0.3 第 0 组：上电冒烟（5 分钟，先做这个）

1. 烧 `build-ui`，看启动日志。
2. 预期：板型 / IDF 版本 / 复位原因 / Flash / PSRAM / 内部堆，随后出现首页四张车票。
3. 顺手确认：屏幕不花、旋钮能移动焦点、喇叭无底噪或无啸叫。
4. **失败即停**：如果连启动都不过（看门狗复位、PSRAM 报错、全白屏），先别往下做，
   把整段启动日志抄进记录——后面 9 组都建立在这一步之上。

## 1. 显示、背光与输入

1. 旋钮左右旋转：首页焦点在四张车票间移动，无跳步/反向。
2. NEXT/PREV 短按：与本机 180° 安装方向一致（物理上键位是互换的）。
3. OK 进入设置 → 亮度 ±5 生效，最低 10%、最高 100% 不黑屏。
4. 设置里切旋转 0°/180°：整屏立即可读，图片不撕裂。
5. 背光 25 kHz：贴近耳朵听功放/电感，应无 5 kHz 啸叫（若有，换 20/30/35 kHz 再试）。

**失败信号**：焦点反向 → `src/input.cpp` 的 NEXT/LAST 对调；亮度跳变 → LEDC 占空比/分辨率。

## 2. 音乐播放

1. TF 卡放 `/music/<分类>/xxx.mp3`，进入「音乐」→ 出现分类与计数。
2. 选分类 → 歌曲列表 → OK 播放：有声音、进度秒数递增、音量旋钮 0..21 生效。
3. 切歌：NEXT/PREV 正常，播完自动下一首。
4. 长按 OK：停止并回列表；SET 长按：回主页但音乐继续。
5. 收藏：长按 OK 收藏，重启后仍显示 ♥（现在默认镜像已开 FRAM）。

**失败信号**：无声 → 核对 I2S 引脚与功放使能（新板 `amplifier_enable=38` 尚未驱动）；
卡顿 → 检查 `Playback` 的 4608 字节 PCM 缓冲与 SD 读速率。

## 3. 故事（图文 + 音频）

1. `/story/分类/故事目录/` 放 `audio.mp3` + `timeline.lrc` + 图片：进入「故事」→ 目录浏览 → 播放。
2. 图文故事：按 LRC 时间轴切图，控制条显示页码与已播时间。
3. 音频故事（目录只有 mp3）：显示标题与时间，无插画。
4. 长按 OK 停止回列表；SET 长按停止回主页。

**失败信号**：图片不切 → 看 `Timeline::atSeconds` 的秒级粒度是否与 LRC 一致；
JPEG 解码失败 → `Display::drawStoryImage` 返回 false，控制台会打 “插画读取失败”。

## 4. 视频

1. 用 `tools/build_video.mjs` 生成 `/video/xxx.avi`（+ 同名 mp3）。
2. 进入「视频」→ 列表 → 播放：画面连续、底部时间/帧号/解码耗时可见。
3. 观察角标里的 `xx ms`（单帧解码耗时）与是否有明显掉帧。
4. 同名 mp3 存在时应有旁白；暂停时音视频同时停。

**失败信号**：花屏 → QOI/JPEG 解码或 AVI 帧偏移；卡顿 → PSRAM 带宽或 SPI 推屏。
记录：实测帧率、单帧耗时、是否发热明显。

## 5. BLE 控制（小程序/桌面端）

1. 手机搜到 `StoryBox`，服务 UUID 应为 `6b7a0001-...`。
2. 连接后 0.5 s 内收到 `evt:"hello"`（含 `bound`/`code`）。
3. 未绑定时用首页设置里的 6 位码执行 `bind` → 拿到 token。
4. `ctrl volume/play/pause/next/prev` 生效；每秒收到 `evt:"state"`。
5. 断开重连：`hello` 再次下发，广告恢复。
6. 本轮新增的写命令（都先 `auth`）逐个试：
   - `set`：改 `dailyLimitMin` → 回包 `data.dailyLimitMin` 是新值；改 `volMax` 后当前音量被夹住。
   - `pin set`：4..6 位成功、`pin verify` 为 true；3 位应回 `PIN_FORMAT`。
   - `reset` / `unbind`：回包 `bound:false` 且带新的 `code`，并且连接被降级（后续命令回 `AUTH`）。
   - `get stories|music|cats|report`：`total` 与设备上的实际条目数一致；带 `limit:10` 时应被夹到 4。
   - `ctrl fav`：回 `f:true` 且 `n` 递增；重启后 `get music` 里该曲目 `f` 仍为 true
     （默认镜像已开 FRAM）。
   - 远程锁打开时：`ctrl play` 回 `LOCKED`，而 `ctrl fav` 仍应成功。

**失败信号**：连上但无 hello → 先怀疑广告字段超 31 字节（flags 3 + 128 位 UUID 18 + 名称 10 = 31，
刚好卡满）；`evt:state` 收不全 → MTU 协商或分片问题；`set` 回 `UNAVAILABLE` → PCTL 记录没读到，
看启动日志里的 `parental ready` 一行。

## 6. USB 文件管理（桌面端）

1. 用 `sdkconfig.usb-product` 构建并烧录，打开桌面端文件管理器。
2. `hello` 应声明 `fs_transfer`；列出 `/music`、`/story`。
3. 上传一个小 mp3 到已有目录 → 成功后设备提示“媒体已更新”，列表出现新曲目。
4. 传输中断开 USB：设备侧会话应在 60 s 内释放（`AUDIO_BUSY` 消失）。

**失败信号**：桌面端页面不开 → features 不含 `fs*`；上传失败 → 看 `FileReader` 返回的 `err` 码。

## 7. NFC 状态

1. 桌面端或 `sys` 调试口发 `{"cmd":"nfc","act":"status"}`。
2. 无卡：`module:true, card:false`；放卡：`card:true` + `kind/uid`。
3. 拔掉 PN532：应返回 `NFC_UNAVAILABLE`，设备其余功能不受影响。

## 8. 家长控制

1. 设置 → 家长设置：首次进入直接可用（无 PIN）；设 4 位 PIN 后退出。
2. 再次进入要求 PIN；连续错 3 次只提示、不进页面。
3. 设每日时长 5 分钟，播放超过后自动停止并提示。
4. 就寝时段：设一个当前时间窗口，播放应被拒绝；**未校时**时应返回 `ClockRequired`（保守禁止播放）。
5. 解绑后 `hello` 重新给出绑定码。

## 9. 电源

1. 播放中空闲 5 分钟：背光灭、音乐继续；按任意键亮屏且不触发页面动作。
2. 无播放空闲到休眠时间：设备进入深睡（**只在 `CONFIG_STORYBOX_POWER_SLEEP=y` 的镜像上测**）。
3. 转动编码器/按 SET 能唤醒；唤醒后亮度恢复。
4. 记录：熄屏电流、深睡电流、唤醒成功率。

## 10. OTA

**这一步会整片擦除**（分区表从单槽变双槽），先备份 FRAM 与 TF 卡以外的数据。

1. 烧 `sdkconfig.ota` 布局的镜像，确认启动后日志有 “OTA image confirmed”。
2. 用 `sys ota_begin/ota_chunk/ota_end` 上传新镜像，重启后确认进入新版本。
3. 故障演练：上传到一半断电 → 重启应仍在旧版本、可再次升级。
4. 故障演练：上传一个启动即 panic 的镜像 → 下次复位应自动回滚旧槽。

## 回填

结论写进 [验收记录](idf-acceptance-log.md)（每次上机一段，模板在那边），
然后再挑一条汇总回 `docs/idf-migration-progress.md` 的对应批次。
