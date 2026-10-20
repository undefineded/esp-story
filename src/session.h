/* 会话记忆: 上次的音乐分类 / 曲目 / 音量, 掉电保存到 I2C FRAM (U2 MB85RC16)
 *
 * FRAM 布局 (MB85RC16 共 2048 字节 @ 0x000..0x7FF):
 *   0x0000  9B   Settings 配置块 (亮度/旋转/EQ, 见 settings.cpp)
 *   0x0010  3B   音量块: 'V' + 值 + 校验
 *   0x0020  变长 播放位置块: magic(4) + 曲目下标(2) + 分类名 + 曲目路径 + 校验
 *
 * 位置块最长 4+2+1+40+1+128+1 = 177B, 故整机 FRAM 用量约 210B / 2048B (10%)。
 * 分类与曲目都按"名称/路径"记忆而非下标: 换卡、加歌、改名后下标会错位, 名称不会。
 */
#pragma once
#include <Arduino.h>

namespace Session {
  const int SESSION_CAT_MAX  = 40;   // 分类名上限 (含结尾 0)
  const int SESSION_PATH_MAX = 128;  // 曲目完整路径上限 (含结尾 0)

  void    load();               // 读取音量与播放位置 (校验失败则保持默认)
  uint8_t volume();             // 已记住的音量 (0..VOL_MAX)
  bool    setVolume(uint8_t v); // 更新并落盘; 值未变则不写, 返回是否真的写入

  const char* category();       // 上次分类名 ("" = 全部歌曲)
  const char* trackPath();      // 上次曲目完整路径 ("" = 无)
  int         trackIndex();     // 上次曲目下标 (路径未命中时的后备)
  bool        setPosition(const char* category, const char* trackPath, int trackIndex);
                                // 更新并落盘; 内容未变则不写, 返回是否真的写入

#ifdef SESSION_SELFTEST
  void        selfTest();       // 临时: 真机 FRAM 读写自检 (-DSESSION_SELFTEST)
#endif
}
