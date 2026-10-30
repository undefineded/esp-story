/* 全界面使用 LovyanGFX 内置 efont 中文点阵字体，按原生字号绘制。 */
#include "fonts.h"
#ifndef ARDUINO
#include "esp_log.h"
#endif

namespace Fonts {

void begin() {
#ifdef ARDUINO
  Serial.println("[SYS] efontCN: title/body=16px, story=14px, small=12px");
#else
  ESP_LOGI("fonts", "efontCN: title/body=16px, story=14px, small=12px");
#endif
}

const lgfx::IFont* title() { return &lgfx::fonts::efontCN_16; }
const lgfx::IFont* body()  { return &lgfx::fonts::efontCN_16; }
const lgfx::IFont* story() { return &lgfx::fonts::efontCN_14; }
const lgfx::IFont* small() { return &lgfx::fonts::efontCN_12; }

} // namespace Fonts
