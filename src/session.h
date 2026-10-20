/* 会话记忆: 上次曲目 / 音量, 掉电保存到 I2C FRAM (U2 MB85RC16)
 *
 * FRAM 布局 (MB85RC16 共 2048 字节 @ 0x000..0x7FF):
 *   0x0000  9B   Settings 配置块 (亮度/旋转/EQ, 见 settings.cpp)
 *   0x0010  3B   音量块: 'V' + 值 + 校验
 *   0x0020  21B  播放位置块: magic(4) + 分类/完整路径 FNV-1a 64 位哈希(各 8) + 校验
 *
 * 整机 FRAM 用量约 33B / 2048B。哈希基于完整路径，既没有路径长度上限，
 * 也不会在换卡、删歌或重新排序后把旧下标误认为同一首歌。
 */
#pragma once
#include <Arduino.h>

namespace Session {
  void    load();               // 读取音量与播放位置 (校验失败则保持默认)
  uint8_t volume();             // 已记住的音量 (0..VOL_MAX)
  bool    setVolume(uint8_t v); // 更新并落盘; 值未变则不写, 返回是否真的写入

  uint64_t hashPath(const char* path);  // 完整路径 FNV-1a 64 位哈希; nullptr/空串为 0
  uint64_t categoryHash();              // 上次分类哈希; 0 = 全部歌曲
  uint64_t trackHash();                 // 上次曲目哈希; 0 = 无有效曲目
  bool     setPosition(const char* category, const char* trackPath);
                                      // 更新并落盘; 内容未变则不写

#ifdef SESSION_SELFTEST
  void        selfTest();       // 临时: 真机 FRAM 读写自检 (-DSESSION_SELFTEST)
#endif
}
