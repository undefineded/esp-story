/* 会话记忆: 上次的音乐分类 / 曲目 / 音量, 掉电保存到 I2C FRAM (U2 MB85RC16)
 *
 * FRAM 布局 (MB85RC16 共 2048 字节 @ 0x000..0x7FF):
 *   0x0000  9B   Settings 配置块 (亮度/旋转/EQ/开关, 见 settings.cpp)
 *   0x0010  3B   音量块: 'V' + 值 + 校验
 *   0x0020  变长 播放位置块: magic(4) + 分类名长度(1) + 分类名 + 曲目 uuid(4) + 校验(1)
 *
 * 曲目记的是"按文件名算出的 uuid", 不存路径也不存下标:
 *   增删歌、换卡、目录改名后下标会整体错位, 文件名算出的 uuid 保持不变。
 *   记录只有 11..46 字节 (原先存路径要 100+ 字节)。
 */
#pragma once
#include <Arduino.h>

namespace Session {
  const int SESSION_CAT_MAX = 40;   // 分类名上限 (含结尾 0)

  void    load();               // 读取音量与播放位置 (校验失败则保持默认)
  uint8_t volume();             // 已记住的音量 (0..VOL_MAX)
  bool    setVolume(uint8_t v); // 更新并落盘; 值未变则不写, 返回是否真的写入

  const char* category();       // 上次分类名 ("" = 全部歌曲, "童谣" = 童谣汇总列表)
  uint32_t    trackId();        // 上次曲目的 uuid (0 = 无记忆)
  bool        setPosition(const char* category, uint32_t trackId);
                                // 更新并落盘; 内容未变则不写, 返回是否真的写入

#ifdef SESSION_SELFTEST
  void        selfTest();       // 临时: 真机 FRAM 读写自检 (-DSESSION_SELFTEST)
#endif
}
