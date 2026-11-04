#include "audio_run_budget.h"
#include <cassert>
#include <iostream>

int main() {
    Audio::RunBudget budget(0);
    // Fast decode steps do not each incur a 10ms RTOS tick.
    for (int t = 100; t < 4000; t += 100) assert(!budget.shouldYield(true, t, 0));
    assert(budget.shouldYield(true, 4000, 0));
    budget.yielded(14000);
    assert(!budget.shouldYield(true, 15000, 0));
    // A blocking I2S write already gave the scheduler a chance to run idle/UI.
    assert(!budget.shouldYield(true, 40000, 20000));
    assert(!budget.shouldYield(true, 41000, 0));
    assert(budget.shouldYield(true, 44000, 0));
    budget.yielded(54000);
    // A long parser/decoder CPU step without an I2S write must not evade the budget.
    assert(budget.shouldYield(true, 74000, 0));
    assert(!budget.shouldYield(false, 1000000, 0));
    assert(!budget.shouldYield(true, 1001000, 0));
    std::cout << "PASS: audio CPU budget, blocking-I2S exemption, parser-only fairness and idle reset\n";
}
