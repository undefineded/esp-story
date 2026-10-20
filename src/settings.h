/* 设置组件: 亮度 / 旋转 / EQ / 自动息屏, 掉电保存到 I2C FRAM (U2 MB85RC16) */
#pragma once
#include <Arduino.h>
#include "player.h"

struct SettingsData {
  uint8_t  brightness;   // 0..100 (%)
  uint8_t  rotation;     // 1 = 横屏 0° / 3 = 横屏 180°
  EqPreset eq;
  uint8_t  sleepTimeout; // 0=关闭, 1=1分, 2=5分, 3=10分, 4=30分
};

enum class SleepTimeout : uint8_t { Off = 0, Min1, Min5, Min10, Min30, COUNT };

namespace Settings {
  void         load();          // 从 FRAM 读取 (无记录/校验失败则用默认值)
  void         save();          // 写入 FRAM
  SettingsData& data();
  uint32_t     sleepTimeoutMs(); // 0 = 不自动息屏
  const char*  sleepTimeoutLabel();
}
