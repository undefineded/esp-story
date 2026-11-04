#pragma once
#include <cstdint>

namespace Audio {
// I2S backpressure yields naturally; cached decode/parser work must still let
// lower-priority tasks run. Long decoder CPU work is not counted as I/O wait.
class RunBudget {
public:
    explicit RunBudget(int64_t now) : since_(now) {}
    bool shouldYield(bool playing, int64_t now, uint32_t write_us) {
        if (!playing || write_us >= 2000) { since_ = now; return false; }
        return now - since_ >= 4000;
    }
    void yielded(int64_t now) { since_ = now; }
private:
    int64_t since_;
};
}
