/* 视频组件: 播放 TF 卡 /video 下的帧内编码视频 (QOI 或 MJPEG, 都在 AVI 里)
 *
 * 为什么是"帧内、无熵编码"的格式:
 *   ESP32-S3 没有 H.264/H.265 硬解, 320x240 的 MP4 软解不现实;
 *   软解耗时大头是 Huffman + IDCT + 色度上采样 (JPEG 约 20~35ms/帧)。
 *   QOI 每像素只有查表/差分/RLE, 约 3~6ms/帧, 体积约为 MJPEG 的 3~5 倍。
 *   因此默认用 QOI (tools/build_video.mjs), 同时保留 MJPEG 兼容旧文件。
 *   转换脚本会生成:
 *     /video/小星星.avi   QOI 或 MJPEG 视频, 320x240, 固定帧率
 *     /video/小星星.mp3   配套音轨 (可选; 缺省则静音播放)
 *
 * 分工:
 *   Video  - 目录扫描、AVI 索引、按时间轴取帧 (阻塞读取)
 *   Ui     - 真正的帧解码/绘制 (QOI/JPEG) 和控制条
 *   Main   - 音频起播 (Player) 与按键/页面切换
 */
#pragma once
#include <Arduino.h>

namespace Video {

  /* --- 目录 --- */
  int  scan();                       // 重新扫描 /video, 返回条目数 (进入视频页时调用)
  int  count();
  const char* name(int i);           // 文件名 (含扩展名, UTF-8)
  const char* title(int i);          // 去掉扩展名的显示名

  /* --- 播放 --- */
  bool open(int i);                  // 解析 AVI 索引 (阻塞, 先画加载页再调用)
  void start();                      // 起播: 解码第 0 帧, 等音频就绪后开始计时
  void stop();                       // 停止并关闭文件 (同时停掉音轨)
  void pause(bool on);               // 暂停/继续 (计时同步平移)
  bool paused();
  bool active();                     // open+start 之后、stop 之前
  bool takeFinished();               // 取走"播放完毕"标志 (一次性)

  void loop();                       // 主循环调用: 按时间轴取帧并绘制

  /* --- 状态 --- */
  int  index();                      // 当前视频下标 (-1 = 无)
  int  positionMs();
  int  durationMs();
  int  frameCount();
  int  fps();                        // 目标帧率 (按文件里的帧间隔)
  int  actualFps();                  // 最近 1 秒实际交付帧率 (界面角标)
  int  droppedFrames();              // 最近 1 秒因落后被跳过的帧数
  bool hasAudio();                   // 找到配套 mp3/wav
  const char* audioPath();           // 音轨路径 ("" = 无)
  const char* errorText();           // 最近一次 open() 失败的原因 ("" = 无)
}
