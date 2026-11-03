# 真机验收清单（迁移分支）

本文件是**可执行**的验收步骤。当前 26 个提交全部只有编译层证据，下面每一项都还没有真机结论。
建议按顺序做，每项完成后把结论回填到 `docs/idf-migration-progress.md`。

先决条件：一块确认过板型的 S3 板（N16R8）、PN532 模块、已格式化的 TF 卡、一根能供电的 USB 线。

## 0. 环境与烧录

```sh
# 默认产品镜像（BLE + NFC，单槽分区）
idf.py -B build-ui -D SDKCONFIG=build-ui/sdkconfig build
idf.py -B build-ui -D SDKCONFIG=build-ui/sdkconfig -p COMx flash monitor
```

- 预期：串口打印板型/IDF 版本/PSRAM/Flash，随后出现首页（四张车票）。
- 记录：启动耗时、内部堆与 PSRAM 空闲量（`sys stats` 或日志）。

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
5. 收藏：长按 OK 收藏，重启后仍显示 ♥（需要确认 FRAM 的构建）。

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

**失败信号**：连上但无 hello → 检查 `startAdvertising` 的字段长度（128 位 UUID + 名称 = 31 字节，超了就发不出去）。

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

## 回填模板

```
日期 / 板型 / 固件 commit：
1 显示输入: pass/fail 备注
2 音乐: ...
3 故事: ...
4 视频: fps=?, decode=?ms, 发热=?
5 BLE: ...
6 USB: ...
7 NFC: ...
8 家长: ...
9 电源: 熄屏=?mA 深睡=?mA
10 OTA: 成功/断电/panic 三种路径结论
```
