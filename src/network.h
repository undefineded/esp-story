/* 无线网络: WiFi STA + SoftAP 开关
 *
 * - WiFi STA 需要 config.h 里的 WIFI_STA_SSID/WIFI_STA_PASS;
 * - SoftAP 使用 StoryBox-XXXX / WIFI_AP_PASSWORD;
 * - 两个开关都关闭时调用 WiFi.mode(WIFI_OFF), 降低功耗.
 */
#pragma once
#include <Arduino.h>

namespace Network {
  void begin(bool wifiEnabled, bool apEnabled);
  void apply(bool wifiEnabled, bool apEnabled);
  void loop();                       // 记录 STA 连接状态变化
  bool wifiEnabled();
  bool apEnabled();
  const char* apSsid();
}
