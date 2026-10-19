#pragma once
#include <stdint.h>

namespace RenderMath {
struct Span { int left, right; };

inline Span changedSpan(const uint16_t* pixels, const uint16_t* previous,
                        int width, int rows, bool invalid) {
  if (invalid) return {0, width - 1};
  Span span{width, -1};
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < width; ++x) {
      if (pixels[y * width + x] == previous[y * width + x]) continue;
      if (x < span.left) span.left = x;
      if (x > span.right) span.right = x;
    }
  }
  return span;
}

inline int scrollOffset(int width, int viewport, uint32_t elapsed) {
  if (width <= viewport) return 0;
  constexpr uint32_t speed = 28, hold = 2000;
  const int distance = width - viewport;
  const uint32_t travel = (distance * 1000u + speed - 1) / speed;
  const uint32_t phase = elapsed % (travel + 2 * hold);
  if (phase < hold) return 0;
  if (phase >= hold + travel) return distance;
  return (phase - hold) * speed / 1000;
}
}
