#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

namespace Video {
// One worker, latest-request-wins admission. Completion of a superseded open
// must never replace the state of a newer open/cancel/audio handoff.
template<class Command, class Snapshot> class VersionedControl {
public:
    void post(Command command, Snapshot initial = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        command.serial = serial_.load() + 1;
        command_ = command;
        snapshot_ = initial;
        serial_ = command.serial;
    }
    Command command() {
        std::lock_guard<std::mutex> lock(mutex_);
        return command_;
    }
    Snapshot snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }
    bool update(uint32_t serial, const Snapshot& snapshot) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (serial != serial_.load()) return false;
        snapshot_ = snapshot;
        return true;
    }
    void complete(uint32_t serial, const Snapshot& snapshot) {
        update(serial, snapshot);
        applied_ = serial; // Only the worker completes requests, in processing order.
    }
    uint32_t serial() const { return serial_; }
    bool pending() const { return serial_.load() != applied_.load(); }
private:
    std::mutex mutex_;
    Command command_{};
    Snapshot snapshot_{};
    std::atomic<uint32_t> serial_{0}, applied_{0};
};
}
