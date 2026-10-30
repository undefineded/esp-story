/* 输入组件: 3 个按键 + EC11 旋转编码器 */
#pragma once
#include <stdint.h>

namespace Input {
  void   begin();

  int8_t okEvent();        // OK 键: 1=单击, 2=双击, 3=长按, 0=无
  int8_t nextEvent();      // NEXT 键: 1=短按, 2=长按, 0=无
  int8_t prevEvent();      // LAST 键: 1=短按, 2=长按, 0=无
  int8_t encEvent();       // 编码器 SET: 1=短按(释放), 2=长按(达到阈值), 0=无

  int    encoderSteps();   // 读取并清零旋转步数 (+顺时针 / -逆时针)

  /* 最近一次按键/旋钮动作的时刻 (ms), 供电源管理判断空闲;
     童锁期间同样记录, 便于熄屏时能被本地按键唤醒。 */
  uint32_t lastActivityMs();

  /* 唤醒用: 短暂丢弃所有输入, 避免"点亮屏幕"那一下顺带触发按键功能 */
  void   swallowFor(uint32_t ms);

  /* 童锁: 锁定期间仍持续读取并更新按键状态, 但所有事件返回 0;
     编码器步数直接丢弃, 避免解锁瞬间补发。 */
  void   setLocked(bool locked);
  bool   childUnlockHeld(); // 童锁下 OK + NEXT 同时按住 5 秒
}
