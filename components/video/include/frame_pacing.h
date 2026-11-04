#pragma once
#include <cstdint>

namespace Video {
// Count drawing/SPI time toward the frame interval. Always block briefly so
// an overloaded display cannot starve idle tasks; never spin to a deadline.
inline uint64_t frameWaitUs(int64_t started, int64_t now) {
    const int64_t remaining = 33334 - (now - started);
    return remaining > 1000 ? static_cast<uint64_t>(remaining) : 1000;
}
}
