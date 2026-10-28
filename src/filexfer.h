/* TF 卡文件服务: 给上位机文件管理器使用
 *
 * 只允许访问 SD_MMC 的绝对路径; 不支持 ..、相对路径和空路径。
 * 文件内容用 base64 分块传输, 每块 4KB; 传输状态由本模块保存。
 */
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace FileXfer {
  bool ready();
  void loop();
  bool handle(JsonDocument& req, JsonDocument& out, int32_t id);
  void abortAll();
}
