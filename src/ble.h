/* BLE 控制通道: GATT 服务 + JSON 协议 + 绑定/鉴权
 *
 * 小程序 -> 设备: 写入特征值, UTF-8 JSON, 以 '\n' 结尾
 * 设备 -> 小程序: notify 特征值, 同样 JSON 行
 *
 * BLE 回调只做收包和入队, 所有播放/配置动作在主循环执行。
 */
#pragma once
#include <Arduino.h>

namespace Ble {

enum class Action : uint8_t {
  None = 0,
  Play,
  Pause,
  Toggle,
  Next,
  Prev,
  Stop,
  Volume,
  PlayStory
};

struct Command {
  Action  action;
  int32_t value;
};

void begin(bool advertisingEnabled = true);
void loop();
void setAdvertisingEnabled(bool enabled);
bool advertisingEnabled();

bool takeCommand(Command& out);      // 主循环取走播放控制动作
bool takeConfigChanged();            // 家长配置变化, 需要重建内容过滤
bool connected();
const char* deviceName();

} // namespace Ble
