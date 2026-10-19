/* 统一使用内置 efont 中文点阵字体，不缩放，不依赖外部字库。 */
#pragma once
#include <LovyanGFX.hpp>

namespace Fonts {
  void begin();                 // 开机打印字体配置，无动态加载

  const lgfx::IFont* title();   // efontCN_16：歌名/标题
  const lgfx::IFont* story();   // efontCN_14：故事标题（控制条/加载页）
  const lgfx::IFont* body();    // efontCN_16：标签与正文
  const lgfx::IFont* small();   // efontCN_12：时间/数字/辅助中文
}
