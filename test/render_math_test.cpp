// Host test: g++ -std=c++11 -Wall -Wextra -pedantic test/render_math_test.cpp -o /tmp/esp-story-render-test
#include "../src/render_math.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <algorithm>

int main() {
  constexpr int width = 320, rows = 8;
  std::vector<uint16_t> frame(width * rows), previous(width * rows);
  auto span = RenderMath::changedSpan(frame.data(), previous.data(), width, rows, false);
  assert(span.right < span.left); // unchanged screen sends no pixels
  span = RenderMath::changedSpan(frame.data(), nullptr, width, rows, true);
  assert(span.left == 0 && span.right == width - 1); // first frame / rotation
  frame[7 * width + 319] = 0xffff;
  span = RenderMath::changedSpan(frame.data(), previous.data(), width, rows, false);
  assert(span.left == 319 && span.right == 319);
  frame[0] = 1;
  span = RenderMath::changedSpan(frame.data(), previous.data(), width, rows, false);
  assert(span.left == 0 && span.right == 319);

  // Reconstruct a simulated display using only emitted rectangles, including
  // changes back to black and updates scattered across multiple rows.
  uint32_t random = 42;
  for (int pass = 0; pass < 1000; ++pass) {
    random = random * 1664525u + 1013904223u;
    frame[random % frame.size()] = static_cast<uint16_t>(random >> 16);
    span = RenderMath::changedSpan(frame.data(), previous.data(), width, rows, false);
    if (span.right >= span.left) {
      for (int y = 0; y < rows; ++y)
        std::copy(frame.begin() + y * width + span.left,
                  frame.begin() + y * width + span.right + 1,
                  previous.begin() + y * width + span.left);
    }
    assert(frame == previous);
  }
  std::fill(frame.begin(), frame.end(), 0);
  span = RenderMath::changedSpan(frame.data(), previous.data(), width, rows, false);
  assert(span.left == 0 && span.right == 319);

  assert(RenderMath::scrollOffset(100, 200, 4000) == 0);
  assert(RenderMath::scrollOffset(256, 200, 1999) == 0);
  assert(RenderMath::scrollOffset(256, 200, 3000) == 28);
  assert(RenderMath::scrollOffset(256, 200, 4000) == 56);
  assert(RenderMath::scrollOffset(256, 200, 5999) == 56);
  assert(RenderMath::scrollOffset(256, 200, 6000) == 0);
  assert(RenderMath::scrollOffset(201, 200, 2036) == 1);
  uint32_t epoch = 0xfffffff0u, now = epoch + 3000u;
  assert(RenderMath::scrollOffset(256, 200, now - epoch) == 28);
  puts("PASS: dirty rectangles, 1000 frame reconstructions, scroll timing and clock rollover");
}
