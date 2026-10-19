/* 全界面使用 LovyanGFX 内置 efont 中文点阵字体，按原生字号绘制。 */
#include "fonts.h"

namespace Fonts {

void begin() {
  Serial.println("[SYS] efontCN: title/body=16px, story=14px, small=12px");
}

const lgfx::IFont* title() { return &lgfx::fonts::efontCN_16; }
const lgfx::IFont* body()  { return &lgfx::fonts::efontCN_16; }
const lgfx::IFont* story() { return &lgfx::fonts::efontCN_14; }
const lgfx::IFont* small() { return &lgfx::fonts::efontCN_12; }

} // namespace Fonts
