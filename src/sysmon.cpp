#include "sysmon.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

uint32_t s_frameStart = 0;
uint32_t s_busyUs     = 0;
uint32_t s_idleUs     = 0;
uint8_t  s_load       = 0;

const uint32_t kWindowUs = 1000000;   // 1 秒采样窗口
const TickType_t kIdleTick = 1;       // 1ms, 让 idle / 音频任务运行

} // namespace

namespace SysMon {

void frameBegin() {
  s_frameStart = micros();
}

void frameEnd(bool allowIdle) {
  if (!allowIdle) return;             // 传输期间: 不延时, 也不把这段繁忙时间算进占用率

  const uint32_t now = micros();
  s_busyUs += (uint32_t)(now - s_frameStart);

  /* 主动让出 1ms, 这段就是本帧的空闲时间 */
  vTaskDelay(kIdleTick);
  const uint32_t after = micros();
  s_idleUs += (uint32_t)(after - now);

  const uint32_t total = s_busyUs + s_idleUs;
  if (total >= kWindowUs) {
    uint32_t pct = (uint64_t)s_busyUs * 100u / total;
    if (pct > 100) pct = 100;
    s_load   = (uint8_t)pct;
    s_busyUs = 0;
    s_idleUs = 0;
  }
}

uint8_t cpuLoad() {
  return s_load;
}

} // namespace SysMon
