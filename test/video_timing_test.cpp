// Host test: g++ -std=c++11 -Wall -Wextra -pedantic test/video_timing_test.cpp -o /tmp/esp-story-video-test
// 验证 MJPEG 播放的时间轴换算: 帧号取整、末尾钳位、总时长和帧率。
#include "../src/video_timing.h"
#include <cassert>
#include <cstdio>

int main() {
  using namespace VideoTiming;

  // 12fps: 一帧 83333us
  const uint32_t fps12 = 83333;
  assert(fps(fps12) == 12);
  assert(fps(40000) == 25);
  assert(fps(0) == 25);                       // 缺省值兜底

  // 帧 0 覆盖 [0, 83) ms, 帧 1 从 84ms 开始
  assert(frameAt(0, fps12, 72) == 0);
  assert(frameAt(83, fps12, 72) == 0);
  assert(frameAt(84, fps12, 72) == 1);
  assert(frameAt(1000, fps12, 72) == 12);     // 1s -> 第 12 帧
  assert(frameAt(5999, fps12, 72) == 71);     // 末尾钳位, 不越界
  assert(frameAt(99999, fps12, 72) == 71);
  assert(frameAt(0, fps12, 0) == -1);         // 空视频

  // 总时长 = 帧数 * 帧间隔 (6s @12fps -> 5999ms)
  assert(durationMs(72, fps12) == 5999);
  assert(durationMs(0, fps12) == 0);
  assert(frameStartUs(3, fps12) == 249999ULL);

  // 25fps 边界
  assert(frameAt(39, 40000, 10) == 0);
  assert(frameAt(40, 40000, 10) == 1);
  assert(durationMs(10, 40000) == 400);

  // 单调递增: 时间往前走, 帧号不能倒退
  int last = -1;
  for (uint32_t ms = 0; ms <= 6000; ms += 7) {
    int idx = frameAt(ms, fps12, 72);
    assert(idx >= last);
    assert(idx >= 0 && idx < 72);
    last = idx;
  }

  puts("PASS: video frame timing, clamping, duration and fps");
}
