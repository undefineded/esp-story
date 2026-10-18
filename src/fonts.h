/* 字体组件: 从 flash 加载 VLW 抗锯齿圆体 (幼圆), 失败时回退内置点阵字体
   字形数据由 tools/gen_vlw_font.ps1 生成, 经 tools/embed_fonts.py 嵌入固件 */
#pragma once
#include <LovyanGFX.hpp>

namespace Fonts {
  void begin();                 // 加载圆体 (开机调用一次, 内部自带回退)
  bool ok();                    // true = 圆体加载成功

  const lgfx::IFont* title();   // 16px  歌名/大标题 (GB2312 全集)
  const lgfx::IFont* story();   // 14px  故事标题 (控制条/加载页, GB2312 全集)
  const lgfx::IFont* body();    // 15px  标签与正文
  const lgfx::IFont* small();   // 11px  时间/数字 (仅 ASCII)
}