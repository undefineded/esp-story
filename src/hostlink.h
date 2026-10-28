/* 上位机串口桥: USB CDC SBX4 二进制帧协议
 *
 * 16字节头部 + JSON元数据 + 原始文件数据；头部和帧体分别 CRC32。
 * 设备诊断日志仍可位于帧之间，上位机按 magic/长度识别数据帧。
 * 元数据请求/响应/事件使用 v:4，文件数据不编码为文本。
 */
#pragma once
#include <stdint.h>

namespace HostLink {
  void begin();
  void loop();
  bool recentlyActive(uint32_t withinMs = 60000);  // 最近是否有上位机指令 (电源管理用：别在传文件/读卡时睡着)
}
