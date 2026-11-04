# 官方播放栈（esp_player）可行性探测

> 历史探测记录。当前实现已改用 IDF 5.5.3 + 官方 esp_player 1.1.1，依赖解析和完整固件编译通过。
> 下面关于 5.5.1 / 临时 2.0.0 工程的结论不能作为当前构建结论；以 [迁移说明](idf-video-player.md) 和依赖锁文件为准。

`esp_player` 是乐鑫的官方音视频播放组件，能补上我们现在的两块空缺：**带内嵌音轨的 AVI 解复用**
和**音频主时钟同步**。这份记录只写本机实测到的事实，不含真机结论。

探测日期：2026-11-04。环境：本机 ESP-IDF 5.5.1（`C:\esp\v5.5.1\esp-idf`），
临时工程 `C:\esp\esp-player-spike`（**不在本仓库**，空 `app_main`，只声明组件依赖）。

## 结论

| 版本 | 结果 |
|---|---|
| `espressif/esp_player: "1.1.1"` | ❌ **装不上**：依赖求解失败 |
| `espressif/esp_player: "*"` → 解析到 **2.0.0** | ✅ 配置 + 完整编译通过（`esp_player_spike.bin` = 214,416 B，空 main 只链了 1.2 KB 媒体代码） |

1.1.1 失败的确切链路（组件管理器原文）：

```
esp_player (1.1.1) -> gmf_io (~1.1) -> esp_codec_dev (~2.0.0-beta1)
  -> idf (>=5.4.4,<5.5.0 || >5.5.0,<5.5.1 || >5.5.1,<5.5.2 || >5.5.2)
  -> "no versions of idf match ..."
```

也就是说 `esp_codec_dev 2.0.0-betaX` **显式排除了 IDF 5.5.0 / 5.5.1 / 5.5.2**，只接受
`>=5.4.4,<5.5.0` 或 `>5.5.2`。本机 5.5.1 正好落在缺口里，所以论坛/示例里那句「5.5.2+」还不够，
**要 5.5.3+ 或退到 5.4.x**。改用 2.0.0 则不再受这个约束（2.0.0 的依赖链放开了 IDF 上限）。

## 关键 API 事实（从 2.0.0 头文件读到，未运行）

- `esp_player_init(esp_player_config_t*, ...)` 要求**调用方提供两个渲染句柄**：

  ```c
  typedef struct {
      void *audio_render_hd;  // esp_audio_render_stream_handle_t
      void *video_render_hd;  // esp_video_render_handle_t
  } esp_player_config_t;
  ```

  播放器只管解复用/解码/同步，**不接管屏幕和 I2S**——这正好是我们需要的形状。
- 音频：`esp_audio_render_cfg_t.out_writer` 是 `esp_audio_render_write_cb_t`，
  **可以自定义 PCM 输出**。所以视频内音轨能继续走我们自己的 I2S + EQ + 音量 + 家长门禁，
  不必把 I2S 所有权交出去。
- 视频：`esp_video_render` 支持「可扩展的自定义后端接口」（`backend_cfg.ops`）和
  「面向帧缓冲」`stream_acquire_fb/write_fb/release_fb` 两条路，
  见 [README_CN](https://github.com/espressif/esp-gmf/blob/5cd04a502dd00aa4c70db046f9509f04b89cc8d9/packages/esp_video_render/README_CN.md)。
  我们的 LovyanGFX 屏（SPI2 @ 40 MHz，8 行 strip）可以做成自定义 backend。

## 仍然未知 / 风险

- **体积**：2.0.0 会拉进 15 个组件（esp_player、esp_extractor、esp_video_render、
  esp_video_codec、esp_h264、esp_new_jpeg、esp_audio_render、esp_audio_effects、
  esp_codec_dev、esp_asrc、esp_muxer、esp_hls_stream、gmf_core/audio/video/io/fft、
  media_lib_sal）。空 main 不链这些代码，**真实增量必须等接上调用后量 `size-components`**。
  OTA 双槽每槽 3 MiB、当前镜像 1.91 MB，只剩约 1.19 MB。
- **帧率**：屏是 SPI2 @ 40 MHz，全屏 320×240×2 = 153.6 KB → 理论 32 fps 上限（实际更低）。
  官方 28 fps 是纯解码成绩，不含推屏。换栈不会提高帧率上限，收益是同步与异步流水线。
- **格式**：官方只吃 H.264 / MJPEG，我们现有的 **QOI** 视频（`tools/build_video.mjs` 产物）
  需要转码成 MJPEG。S3 没有 H.264 硬解，走软件解码能否 15 fps 未验证。
- **真机**：以上全部是编译层事实，没有一块板跑过。

## 建议顺序

1. 先用现镜像做完真机验收（尤其第 4 组视频），拿到**现有路径**的解码耗时/帧率基线；
2. 再单开一条分支接 `esp_player 2.0.0`：`esp_video_render` 自定义 backend 接 LovyanGFX、
   `esp_audio_render.out_writer` 接现有 I2S/门禁、文件 IO 走 `Storage::AccessGate` 的 `FileStream`；
3. 只在「≥15 fps + 音画偏差可接受 + 固件塞得进 3 MiB 槽」三条都过时才替换主路径，
   并且保留现有 `components/video` 做 A/B。
