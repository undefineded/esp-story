/* 播放器组件: 封装 ESP32-audioI2S, 提供播放控制与人声 EQ 预设
   解码运行在独立 FreeRTOS 任务 (Player::begin 里创建), 因此:
   所有控制接口都是"下发命令", 真正的动作由音频任务异步执行;
   PlayerStatus 是一份异步快照, 无需在主 loop() 里调用解码。 */
#pragma once
#include <Arduino.h>

/* 3 段 IIR EQ 预设 (解码后应用)
   low  : 低架滤波 @500Hz
   peak : 峰值滤波 @3kHz (Q=2.5) —— 人声清晰度/存在感区间
   high : 高架滤波 @6kHz
   取值范围 -40 ... +6 dB */
enum class EqPreset : uint8_t { Vocal, Pop, Bass, Flat, COUNT };

struct EqBands { int8_t low, peak, high; };

inline EqBands eqBands(EqPreset p) {
  switch (p) {
    case EqPreset::Vocal: return { -2, +4, -1 };   // 人声突出: 削弱低频掩蔽 + 提升 3kHz + 抑制齿音刺耳
    case EqPreset::Pop:   return { +2, +2, +1 };   // 流行: 略强低频与亮度
    case EqPreset::Bass:  return { +5,  0, -1 };   // 低音: 强化低频
    default:              return {  0,  0,  0 };   // 平直
  }
}

inline const char* eqName(EqPreset p) {
  switch (p) {
    case EqPreset::Vocal: return "VOCAL";
    case EqPreset::Pop:   return "POP";
    case EqPreset::Bass:  return "BASS";
    default:              return "FLAT";
  }
}

/* UI 展示用的播放状态快照 */
struct PlayerStatus {
  bool        hasTrack;
  bool        playing;      // 解码器正在输出
  bool        paused;       // 用户暂停标志
  int         index;        // 当前曲目 (0 基)
  int         count;        // 曲目总数
  const char* name;         // 文件名 (UTF-8)
  const char* codec;        // 编解码器名
  uint32_t    curSec;
  uint32_t    durSec;
  uint32_t    bitrateKbps;
  uint32_t    sampleRate;
  uint8_t     bits;
  uint8_t     volume;       // 0..VOL_MAX
  EqPreset    eq;
};

namespace Player {
  void        begin();                      // 启动音频任务 (只需调用一次)

  void        playIndex(int i, bool autoStart = true);
                                 // autoStart=false: 只载入并暂停 (开机恢复上次曲目用)
  void        playPath(const char* path, bool autoStart = true);  // 播放任意文件 (故事模式)
  void        next();
  void        prev();
  void        toggle();                     // 播放/暂停
  void        stop();                       // 停止当前播放
  void        inhibitPlayback(bool on);     // 文件传输期间拒绝已入队和新的播放命令
  bool        stopAndWait(uint32_t timeoutMs = 3000); // 确认音频任务停止后才可重建列表

  void        setAutoNext(bool on);         // 播放结束是否自动下一曲 (音乐模式 true)
  bool        takeEof();                    // 取走"播放结束"标志 (一次性)
  bool        connected();                  // 解码器已连接 (暂停时仍为 true)
  bool        paused();

  void        setVolume(int v);
  void        adjustVolume(int delta);

  void        setEq(EqPreset p);
  void        cycleEq();

  PlayerStatus status();
}
