/* 霍尔开关组件 (U9 HAL251SO): 仅用于调试显示磁铁状态 */
#pragma once
#include <Arduino.h>

namespace Hall {
  void begin();
  int  level();        // 原始电平: LOW=0 / HIGH=1
  bool detected();     // true = 检出游丝磁场 (推挽输出拉低)
}