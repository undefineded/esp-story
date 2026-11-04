#include "latest_frame.h"
#include <array>
#include <cassert>
#include <thread>
#include <iostream>

int main() {
    Video::LatestFrame frame;
    std::array<uint32_t, 320 * 240 / 2> storage{}, input{}, result{};
    assert(!frame.publish(input.data(), sizeof(input)));
    frame.attach(storage.data(), sizeof(storage));
    assert(!frame.consume(result.data(), sizeof(result)));
    input.fill(1);
    assert(frame.publish(input.data(), sizeof(input)));
    assert(!frame.consume(result.data(), sizeof(result) - 1));
    input.fill(2);
    assert(frame.publish(input.data(), sizeof(input)));
    assert(frame.frames() == 2 && frame.replaced() == 1);
    assert(frame.consume(result.data(), sizeof(result)));
    for (auto pixel : result) assert(pixel == 2);
    assert(!frame.pending());
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (unsigned n = 3; n < 1003; ++n) {
            input.fill(n);
            assert(frame.publish(input.data(), sizeof(input)));
        }
        done = true;
    });
    unsigned consumed = 1;
    while (!done || frame.pending()) {
        if (frame.consume(result.data(), sizeof(result))) {
            ++consumed;
            for (auto pixel : result) assert(pixel == result[0]);
        } else std::this_thread::yield();
    }
    producer.join();
    assert(frame.frames() == consumed + frame.replaced());
    // The UI now borrows the mailbox under its lock instead of copying to a
    // separate page buffer. Cached visits support pause/overlay redraws.
    input.fill(42);
    assert(frame.publish(input.data(), sizeof(input)));
    for (int n = 0; n < 2; ++n) {
        assert(frame.visit([&](const void* data) {
            const auto* pixels = static_cast<const uint32_t*>(data);
            for (std::size_t i = 0; i < input.size(); ++i) assert(pixels[i] == 42);
        }));
        assert(!frame.pending());
    }
    const auto frames = frame.frames(), replaced = frame.replaced();
    frame.detach();
    assert(!frame.visit([](const void*) { assert(false); }));
    assert(!frame.pending() && frame.frames() == frames && frame.replaced() == replaced);
    assert(!frame.publish(input.data(), sizeof(input)));
    frame.detach(); // repeated Stop must preserve the same completed session
    assert(frame.frames() == frames && frame.replaced() == replaced);
    frame.attach(storage.data(), sizeof(storage));
    assert(!frame.pending() && frame.frames() == 0);
    frame.detach();
    assert(!frame.publish(input.data(), sizeof(input)));
    std::cout << "PASS: latest-frame replacement, bounds, detach and concurrent no-tearing\n";
}
