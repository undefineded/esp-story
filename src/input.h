/* 输入组件: 3 个按键 + EC11 旋转编码器 */
#pragma once
#include <Arduino.h>

namespace Input {
  void   begin();

  int8_t okEvent();        // OK 键: 1=单击, 2=双击, 3=长按, 0=无
  int8_t nextEvent();      // NEXT 键: 1=短按, 2=长按, 0=无
  int8_t prevEvent();      // LAST 键: 1=短按, 2=长按, 0=无
  int8_t encEvent();       // 编码器 SET: 1=短按(释放), 2=长按(达到阈值), 0=无

  int    encoderSteps();   // 读取并清零旋转步数 (+顺时针 / -逆时针)
}