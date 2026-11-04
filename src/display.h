/* LovyanGFX 显示驱动组件: ST7789 240x320 SPI 屏 */
#pragma once
#include <LovyanGFX.hpp>

namespace Display {
  struct Stats { uint32_t submissions, video_submissions, bytes, last_us, max_us; };
  Stats stats(); // Actual LCD submissions; not decoder/backend FPS.
  void setVideoMode(bool enabled); // UI owner only. Bypass pixel comparison for video.
  void begin();                          // 初始化面板 + 背光 PWM
  void setBrightness(uint8_t percent);   // 背光亮度 0..100
  void setBacklightFreq(uint32_t hz);    // 改调光频率 (排查 IP5306 啸叫用, 保持当前亮度)
  void setRotation(uint8_t r);           // 1 = 横屏 0° / 3 = 横屏 180°
  lgfx::LGFXBase& canvas();              // PSRAM 合成画布；分配失败回退直绘
  void present(bool force = false);      // 比较已提交画面，只发送变化的条带
}

lgfx::LGFX_Device& lcd();                // 全局显示对象
