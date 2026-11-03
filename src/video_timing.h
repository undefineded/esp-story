/* 视频时间轴换算 (纯函数, 无硬件依赖, 可在主机端单测)
 *
 * 设备按 AVI 的固定帧间隔播放: 第 i 帧在 i * frameUs 时刻显示。
 * 解码/推屏耗时会让实际时刻略晚于计划, 播放器用 frameAt() 直接算出
 * "此刻应该显示哪一帧", 落后时自动丢帧, 不会越播越慢。
 */
#pragma once
#include <stdint.h>

namespace VideoTiming {

constexpr uint32_t kDefaultFrameUs = 40000;   // 25fps (AVI 头缺失时的兜底)

inline uint32_t frameUs(uint32_t usPerFrame) {
  return usPerFrame ? usPerFrame : kDefaultFrameUs;
}

/* 第 index 帧的计划显示时刻 (us) */
inline uint64_t frameStartUs(uint32_t index, uint32_t usPerFrame) {
  return (uint64_t)index * frameUs(usPerFrame);
}

/* elapsedMs 时刻应显示的帧号: 不晚于该时刻的最后一帧;
 * 超过末尾时返回 count-1, count<=0 返回 -1 */
inline int frameAt(uint32_t elapsedMs, uint32_t usPerFrame, int count) {
  if (count <= 0) return -1;
  const uint64_t us   = (uint64_t)elapsedMs * 1000ULL;
  const uint64_t step = frameUs(usPerFrame);
  int64_t idx = (int64_t)(us / step);
  if (idx >= count) idx = count - 1;
  if (idx < 0) idx = 0;
  return (int)idx;
}

/* 视频总时长 (ms): 帧数 * 帧间隔 */
inline uint32_t durationMs(int count, uint32_t usPerFrame) {
  if (count <= 0) return 0;
  return (uint32_t)(((uint64_t)count * frameUs(usPerFrame)) / 1000ULL);
}

/* 帧率 (四舍五入, 仅用于显示/日志) */
inline int fps(uint32_t usPerFrame) {
  const uint32_t us = frameUs(usPerFrame);
  return (int)((1000000ULL + us / 2) / us);
}

} // namespace VideoTiming
