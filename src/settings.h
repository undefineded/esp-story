/* 设置组件: 亮度 / 旋转 / EQ / 蓝牙广播 / WiFi / AP, 掉电保存到 I2C FRAM (U2 MB85RC16) */
#pragma once
#include <Arduino.h>
#include "player.h"

struct SettingsData {
  uint8_t  brightness;   // 0..100 (%)
  uint8_t  rotation;     // 1 = 横屏 0° / 3 = 横屏 180°
  EqPreset eq;
  uint8_t  reserved;     // 保留字节 (维持 FRAM 布局, 勿删)
  uint8_t  bleEnabled;    // 蓝牙广播 1=开 0=关
  uint8_t  wifiEnabled;   // WiFi STA 1=开 0=关
  uint8_t  apEnabled;     // SoftAP 1=开 0=关
};

namespace Settings {
  void         load();          // 从 FRAM 读取 (无记录/校验失败则用默认值)
  void         save();          // 写入 FRAM
  SettingsData& data();
}