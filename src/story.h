/* 故事组件: 扫描 /story 下的故事目录
   两种类型 (按目录内容自动区分, 不靠文件夹名):
     图文故事 : mp3 + NN.jpg + timeline.lrc   -> 全屏插画 + 底部控制条
     音频故事 : 只有 mp3                      -> 单独的音频播放页
   目录可以平铺, 也可以分组:
     /story/<故事名>/...
     /story/图文故事/<故事名>/...
     /story/音频故事/<故事名>/...
   timeline.lrc 格式: [mm:ss.xx]NN.jpg | 摘要 */
#pragma once
#include <Arduino.h>
#include "catalog.h"

namespace Story {
  enum class Kind : uint8_t { Illustrated, Audio };

  bool rescan();
  void clear();
  bool index(CatalogArchive& archive, bool reading, bool apply = true);
  int  count();                    // 故事数量
  const char* title(int i);        // 故事名 (文件夹名, UTF-8; 越界返回 "")
  Kind kind(int i);                // 图文 / 音频 (按是否有时间轴判断)

  /* 家长内容白名单视图 */
  void setStoryFilter(uint64_t mask, bool enabled);
  int  viewCount();
  const char* viewTitle(int view);
  int  viewActual(int view);

  int  currentIndex();             // 当前打开的故事实际下标 (-1 = 无)
  const char* currentTitle();      // 当前故事名 ("" = 无)
  Kind currentKind();              // 当前故事类型

  bool open(int i);                // 载入故事 i 的音频与时间轴, 失败返回 false
  void start();                    // 从第一幅插画开始播放
  void stop();                     // 停止播放并恢复音乐模式行为

  void loop();                     // 持续调用: 按播放进度切换插画

  bool active();                   // 已 open() 且未 stop()
  bool takeFinished();             // 取走"播放完毕"标志 (一次性)

  int  panelCount();               // 当前故事插画数
  int  panelIndex();               // 当前插画下标 (0 基)
  const char* panelPath();         // 当前插画完整路径 (无插画返回 nullptr)
}
