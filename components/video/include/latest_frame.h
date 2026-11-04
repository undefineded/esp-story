#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace Video {
// Bounded mailbox. Storage is supplied by the owner and detached after producers
// stop. Readers never observe a partially copied frame; old frames are replaced.
class LatestFrame {
public:
    void attach(void* data, std::size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_ = data; size_ = size;
        pending_ = false; available_ = false; frames_ = 0; replaced_ = 0;
    }
    // Stop producers first. Retain session counters until the next attach.
    void detach() {
        std::lock_guard<std::mutex> lock(mutex_);
        data_ = nullptr; size_ = 0;
        pending_ = false; available_ = false;
    }
    bool publish(const void* data, std::size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!data_ || !data || !size_ || size != size_) return false;
        if (pending_) ++replaced_;
        std::memcpy(data_, data, size_);
        ++frames_;
        pending_ = true;
        available_ = true;
        return true;
    }
    bool consume(void* data, std::size_t capacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!data_ || !data || !pending_ || capacity < size_) return false;
        std::memcpy(data, data_, size_);
        pending_ = false;
        return true;
    }
    bool pending() const { return pending_; }
    template<class Visitor> bool visit(Visitor visitor) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!data_ || !available_) return false;
        visitor(data_);
        pending_ = false;
        return true;
    }
    uint32_t frames() const { return frames_; }
    uint32_t replaced() const { return replaced_; }
private:
    std::mutex mutex_;
    void* data_ = nullptr;
    std::size_t size_ = 0;
    bool available_ = false;
    std::atomic<bool> pending_{false};
    std::atomic<uint32_t> frames_{0}, replaced_{0};
};
}
