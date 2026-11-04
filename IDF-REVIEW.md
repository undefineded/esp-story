# ESP-IDF 迁移审阅

审阅提交：991dee9283a3850fc00b0d4a1b6938e00c3bf15a；比较基线：origin/main（16c2fab67debe775a706e19513f5bd6685443f2f）。

范围：迁移新增 components、main、共用驱动改动、构建/配置、主机测试，以及桌面文件管理与固件协议调用链。Arduino 实现作为行为对照。生成图片、字体和像素数组不作逐字节审计。小程序源码在交接文档所指的另一个 Windows 仓库，不在本工作区，无法宣称已审阅其实现。

以下是静态调用链可确认的问题；标注“主机复现”的另有运行证据。P1 为核心功能/权限/数据一致性问题，P2 为局部功能或特定配置问题。没有修改业务代码。

## P1

1. **普通停止永久关闭播放准入** — `components/app/src/app.cpp:1099`、`components/audio/playback.cpp:49`。
   `stopPlayback()` 调用 `stopAndWait()`，后者设 `inhibited_=true`；产品代码只有刷新目录时恢复准入。长按停止、退出故事、结束故事、退出带音频的视频、家长限制停止后，重新播放都会失败，直到刷新目录或重启。应区分普通停止与维护停止，在正常重新播放且策略允许时恢复准入。

2. **缓存加载成功后应用仍持有空目录** — `components/app/src/app.cpp:875`。
   先保存 `g_library.snapshot()`，再 `loadCache()` 替换库内快照，却没有重新赋给 `s_catalog`。同时把 `catalog_ready` 设为 true，列表不再自动扫描。仅含音乐/绘本的有效缓存重启后会出现空列表，手动刷新才恢复。应在缓存加载之后发布应用快照。

3. **音乐自然结束不会自动下一首** — `components/app/src/pages_music.cpp:434`。
   EOF 处理被放在 generation 发生变化的分支内；解码器在开始 Play 时增加 generation，进入 Ended 不增加。因此普通曲目播放期间已记录 generation，到 EOF 不再执行切歌。还应将自动续播放到全局播放管理层，否则回主页/打开歌曲列表后顶层页面不再执行播放器 tick，后台播放也不会续播。

4. **本地故事播放绕过白名单** — `components/app/src/pages_story.cpp:507`。
   `start()` 直接调用音频 play，未调用 `storyAllowed()` 或 `playbackAllowed()`。本地列表选择和 NEXT/PREV 均走此入口；主循环只检查全局锁，不检查当前故事白名单，因此被禁止的故事可以完整播放。应在最终播放入口统一授权，并在白名单变化时检查当前内容。

5. **视频没有完整的家长策略执行** — `components/app/src/pages_video.cpp:156`、`components/app/src/app.cpp:928`。
   视频打开及 tick 均不检查全局锁，主循环仅根据音频 Playing 计时/停止。无旁白的视频不计用量，锁定时画面继续播放；有旁白的也仅会停音频。应按视频真实播放状态计时，并同时停止画面和声音。

6. **USB worker 越过应用任务直接修改共享状态** — `components/app/src/app.cpp:549`、`:591`，`components/hostlink/usb_transport.cpp:67`。
   USB sys set/rescan 直接修改 Context、显示旋转、SettingsStore、s_catalog，而 UI 同时绘制和访问这些对象。库内部互斥不保护外部的同一个 shared_ptr 变量或显示对象。存在 C++ 数据竞争及显示/设置一致性风险。应通过应用命令队列处理修改，统计读取使用快照。

7. **本地家长设置覆盖最新用量和远程配置** — `components/app/src/pages_parental.cpp:202`、`:313`。
   进入页面时复制完整 PCTL 记录；之后播放计时、BLE 修改仍更新全局记录，但页面保存整份旧副本。进入设置后等待一段时间再调整任一选项，会把 used 回退；期间的远程锁、绑定或 PIN 修改也可能被覆盖。应仅提交所编辑字段，基于最新 policy record 合并。

8. **本地降低音量上限不限制当前输出** — `components/app/src/pages_parental.cpp:283`、`components/app/src/app.cpp:1193`。
   本地保存只 rebind policy，没有重新 clamp 当前音量；BLE apply_config 则有 `setVolume(s_context.volume)`。正在播放时从高音量降低上限，输出维持原值直到下一次音量操作。启动恢复音量同样直接调用 g_playback.setVolume（app.cpp:872），没有应用已加载的家长范围。应统一在策略更新/初始化后夹紧音量。

9. **未绑定且未设 PIN 时，任意 PIN 可以完成 BLE auth** — `components/ble/ble_service.cpp` 的 processLine/auth。
   新实现只要请求含 pin 就调用 verify_pin；parentalVerifyPin 在没有 PIN 时返回 true。于是未绑定设备发送 auth + 任意 pin 即可执行受保护命令，而旧 src/ble.cpp 只有 hasPin 时才走 PIN 验证。应仅在实际配置 PIN 时允许此认证路径。另：旧版已有“已绑定但无 PIN 时放行”规则，这是继承问题，不能算成此次新增。

## P2

10. **普通音频故事的缓存无法重载（主机复现）** — `components/media/media_cache.cpp:72`。
    扫描器把音频故事的 directory 与 audio 都设为 MP3 文件路径，读取器却要求 audio 位于 directory + '/' 下。scan/save 都成功，但下一次 loadCache 返回 false。应按 StoryKind/有无 timeline 校验两种表示。

11. **超限扫描被 UI 当作成功（主机复现底层行为）** — `components/app/src/app.cpp:1055`、`components/media/media_library.cpp:160`。
    301 首歌或超过其他限制时，Library::scan 返回 LimitExceeded，且不发布新目录；refreshCatalog 却接受该结果，标记 ready 并尝试保存旧缓存。首次扫描显示空目录，后续刷新仍显示旧目录且报成功。应明确报超限，或设计可发布的截断目录。

12. **第 65 个及以后的故事，即使关闭白名单仍被拒绝（主机复现）** — `components/settings/include/parental_policy.h:93`。
    allowed() 在检查 allowlist 开关前就拒绝 index>=64，而目录允许 128 个故事。BLE 播放/允许状态因此错误。关闭白名单时应先放行合法目录索引；开启时要明确 64 位掩码与 128 项目录的策略。

13. **长按收藏作用于正在播放的歌曲而非选中行** — `components/app/src/pages_music.cpp:360`。
    toggleFavorite 固定 globalIndex(0)，忽略 selected_ 与 playlist.position() 的差值。浏览 B 歌曲并长按时，实际收藏 A。应根据选中行计算全局索引。

14. **同名文件的远程点播可能播放另一首** — `components/app/src/app.cpp` 的 PlayMusic 分支、`components/media/media_library.cpp` 的 nameId/seekId。
    收到的是精确全局索引，却转换为仅按 basename 哈希的 id，再在全部歌曲中 seekId。不同分类存在同名 MP3 时会匹配第一首。收藏 globalIndex 同样按 id 反查，也会指向错误歌曲。应以路径或实际目录索引定位。

15. **播放位置持久化没有接入产品路径** — `components/app/src/app.cpp`、`components/settings/include/media_records.h:49`。
    产品只恢复 volume；从未调用 savePosition，也不恢复 session.track_id/category。相关代码只存在 diagnostics。相比 Arduino 的 Session::setPosition/恢复流程，产品重启后失去上次曲目记忆。

16. **页面已标记 clean，但帧可能从未提交** — `components/app/src/app.cpp:998`、`src/display.cpp:173`。
    UI 20ms 循环，present 有 33ms 节流。若本帧 draw 之后 present 提前返回，页面已 clean，后续静态页面不会再调用 present。快速旋转/页面切换可能留下旧画面，直到下一个事件。应每轮尝试 flush pending display，或仅在成功提交后清除页面脏标记。

17. **熄屏唤醒旋钮事件先执行页面动作** — `components/app/src/app.cpp:912`、`:982`。
    sampleInput/handle 早于 Power::loop；屏幕关闭时转一格旋钮，先改变选项/音量，然后 screenOn 才 swallowFor。应在熄屏状态只收集唤醒活动并丢弃本轮操作，或在事件分发前完成唤醒屏蔽。

18. **USB 文件传输/OTA 未阻止深睡** — `components/app/src/app.cpp:977`。
    host_active 只取 BLE 连接，USB session 和 OTA active 不参与。在启用 STORYBOX_POWER_SLEEP 的组合配置中，空闲时间达到阈值可中断正在进行的传输/升级。

19. **视频结束、暂停或打开失败仍永久阻止休眠** — `components/app/src/pages_video.cpp:158`、`:239`。
    enter 即 setVideoActive(true)，只有 leave 清除；finished_/paused_/error_ 不影响全局活动标记。停留在播完的末帧或错误页时屏幕与设备均不会按设置休眠。应传递实际播放状态。

20. **OTA 过早取消回滚** — `main/app_main.cpp:61`。
    在 App::start 和显示/音频/存储等应用初始化前确认镜像。新版本若在这些阶段崩溃，下次启动不会回滚。应在应用完成必要自检并进入稳定运行后确认。

21. **桌面端未根据部分 FS 能力禁用写操作** — `desktop/src/renderer/src/pages/FileManagerPage.tsx:255`、`:1029`。
    新增 fs_transfer/fs_readonly 支持后，所有修改按钮仍仅根据 busy 等条件启用。只读固件仍可点上传；fs_transfer 仍显示 mkdir/rename/remove/移动，目录上传还必然调用 mkdir，即使目标目录已存在也只接受 EXISTS 而非 UNSUPPORTED_FEATURE。应分别判断浏览、上传、目录修改能力。

22. **窗口上传重试与固件去重范围不兼容** — `desktop/src/main/serial.ts:788`、`components/storage/include/upload_transaction.h:62`。
    桌面并发窗口为 4，固件仅记住最近一个块。较早块已写入但响应丢失、后续块成功时，桌面重发较早块会得到 RANGE，无法按其超时重试策略恢复。USB TX 明确允许背压时丢响应，所以不是只有虚构故障才能触发。应支持窗口内幂等重试或协商窗口为 1。

23. **主机测试入口有两个确定编译失败** — `tools/test_host.sh`、`test/catalog_host/Arduino.h`、`test/pn532_host/Wire.h`。
    catalog 测试包含 story.h，但 Arduino 桩没有 String；绕过该编译阶段后，最后 PN532 测试又因 FakeWire 没有 setTimeOut 失败。CI host job 当前无法通过。需要补齐桩和对应测试，而不是跳过整个 host job。

## 验证结果与限制

- 原样执行 bash tools/test_host.sh：在 catalog 测试编译失败。
- 在不修改仓库脚本的情况下排除上述 catalog 编译段补跑：前面 14 个基础测试及 media_library、media_cache、avi_index、parental_binding、parental_config、power_policy 均通过；最后 PN532 测试编译失败。中间一次临时过滤命令范围错误，已更正，以上结果来自更正后的执行。
- 新增 /tmp/idf-review-repro.cpp，仅作审阅复现：scan=0 / save=0 / audio-story cache load=0；301-track scan=3（LimitExceeded）且目录未发布；白名单关闭时 story63=1、story64=0。
- 环境无 ESP-IDF 工具链、managed codec 依赖或实际开发板。未完成目标编译、真实 MP3 解码/I2S、NimBLE 并发、休眠唤醒及 FRAM/PN532 电气行为验证。文档中的历史构建/上板记录不能当成本次验证结果。
- USB 协议测试脚本要求 IDF 内 cJSON；MP3 适配器测试要求 IDF/codec 头文件；本环境未执行这两组。桌面依赖未安装，本轮桌面侧为静态协议对照。
- 默认产品配置未启用 USB；涉及 USB 的发现针对 usb-product/相关组合配置。深睡项仅在启用相应配置时生效。
- NFC 在 IDF 产品中明确只有 status，Wi-Fi 控制主要在 diagnostics，完整桌面 FS 修改也明确未实现。这些属于迁移能力缺口，未将每个未实现命令重复列为 bug。
- 当前 CI 矩阵没有覆盖 usb-product、OTA 和 power 产品组合；建议将这些组合及上述状态机回归测试加入验证。

优先顺序：先修停止/续播、缓存发布及授权执行，再修任务所有权、家长记录合并，随后处理 UI、文件传输与电源/OTA，最后恢复可用的 CI 回归矩阵。
