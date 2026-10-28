/* 上位机串口桥: USB CDC 上的换行分隔 JSON 协议
 *
 * 设备日志仍以 [TAG] 开头; 本模块只输出以 '{' 开头的 JSON 行, 上位机按行解析。
 * 请求: {"v":2,"id":1,"cmd":"nfc","act":"status"}
 * 响应: {"v":2,"id":1,"ok":true,...}
 * 事件: {"v":2,"evt":"nfc","type":"card",...}
 */
#pragma once
#include <stdint.h>

namespace HostLink {
  void begin();
  void loop();
  bool recentlyActive(uint32_t withinMs = 60000);  // 最近是否有上位机指令 (电源管理用：别在传文件/读卡时睡着)
}
