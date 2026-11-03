/* 系统监视: 主循环占用率采样
 *
 * 固件主循环是 FreeRTOS 里的一个 busy loop, 没有现成的 idle 统计
 * (Arduino 预编译的 libfreertos 未开启 run-time stats)。这里用
 * "本帧实际执行时间 / (执行时间 + 主动让出的空闲时间)" 估算 CPU 占用:
 *   - 每帧结束主动 vTaskDelay(1), 让 idle / 音频任务拿到 CPU;
 *   - 文件传输期间不让出 (避免拖慢 SD 写入), 该窗口不计入采样;
 *   - 每秒汇总一次, 供上位机设备中心展示。
 */
#pragma once
#include <Arduino.h>

namespace SysMon {
  void    frameBegin();                    // 主循环每帧开始
  void    frameEnd(bool allowIdle);        // 每帧结束; allowIdle=false 时不采样也不延时
  uint8_t cpuLoad();                       // 最近 1 秒的主循环占用 (0..100)
}
