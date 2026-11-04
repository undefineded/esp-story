#include "versioned_control.h"
#include <cassert>
#include <thread>
#include <iostream>

struct Command { uint32_t serial = 0; int target = 0; };
struct State { int value = 0; };
int main() {
    Video::VersionedControl<Command, State> control;
    assert(!control.pending());
    control.post({0, 1}, {10});
    const auto first = control.command();
    control.post({0, 2}, {20});
    control.complete(first.serial, {99});
    assert(control.pending() && control.snapshot().value == 20);
    assert(!control.update(first.serial, {999}));
    // Cancel a pending open, then select a third file before the worker returns.
    control.post({0, -1}, {-10});
    control.post({0, 3}, {30});
    auto latest = control.command();
    assert(latest.target == 3);
    control.complete(latest.serial, {300});
    assert(!control.pending() && control.snapshot().value == 300);
    std::atomic<bool> done{false};
    std::thread worker([&] {
        uint32_t processed = latest.serial;
        while (!done || control.pending()) {
            const auto command = control.command();
            if (command.serial != processed) {
                // The UI may submit more commands before this completion.
                std::this_thread::yield();
                control.complete(command.serial, {command.target});
                processed = command.serial;
            }
        }
    });
    for (int i = 1; i <= 10000; ++i) control.post({0, i}, {-i});
    done = true;
    worker.join();
    assert(!control.pending() && control.snapshot().value == 10000);
    std::cout << "PASS: open/cancel/switch coalescing, stale completion rejection and concurrent final selection\n";
}
