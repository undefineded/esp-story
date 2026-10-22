/* 收藏: 按"全量曲目下标"记录一首歌是否被收藏 (FRAM 位图)
 *
 * 下标与 BLE `get what=music` 返回的 `i` 一致, 小程序和故事机共用同一套编号。
 * 断电保存, 容量 MAX_TRACKS (300) 位 = 38 字节。
 */
#pragma once
#include <Arduino.h>

namespace Favorites {
  void begin();                 // 从 FRAM 读取 (放在 Fram::begin 之后调用)
  bool isFav(int allIndex);     // 是否已收藏
  bool toggle(int allIndex);    // 切换并落盘, 返回新状态
  void setFav(int allIndex, bool on);
  int  count();                 // 已收藏数量
  void clear();                 // 清空 (恢复出厂设置时调用)
}
