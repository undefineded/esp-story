#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace Video {
// Bounded mailbox with three buffers. The producer copies into whichever buffer the
// reader is not using (two are needed for the reader plus one for the writer) and
// only takes the lock to swap the readable pointer, so a
// 150 KiB frame copy never blocks on the consumer (and vice versa). Readers never
// observe a partially copied frame; old frames are replaced.
class LatestFrame {
public:
    void attach(void* first, void* second, void* third, std::size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffers_[0] = static_cast<uint8_t*>(first);
        buffers_[1] = static_cast<uint8_t*>(second);
        buffers_[2] = static_cast<uint8_t*>(third);
        size_ = size;
        readable_ = nullptr;
        reading_ = nullptr;
        available_ = false;
        pending_ = false;
        frames_ = 0;
        replaced_ = 0;
    }
    // Stop producers first. Retain session counters until the next attach.
    void detach() {
        std::lock_guard<std::mutex> lock(mutex_);
        buffers_[0] = buffers_[1] = buffers_[2] = nullptr;
        size_ = 0;
        readable_ = nullptr;
        reading_ = nullptr;
        available_ = false;
        pending_ = false;
    }
    bool publish(const void* data, std::size_t size) {
        if (!data || !size || size != size_) return false;
        uint8_t* target = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto* candidate : buffers_)
                if (candidate && candidate != readable_ && candidate != reading_) { target = candidate; break; }
            if (!target) { ++replaced_; return false; }
            if (pending_) ++replaced_;
        }
        std::memcpy(target, data, size_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            readable_ = target;
            available_ = true;
            pending_ = true;
            ++frames_;
        }
        return true;
    }
    bool consume(void* data, std::size_t capacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!readable_ || !data || !pending_ || capacity < size_) return false;
        std::memcpy(data, readable_, size_);
        pending_ = false;
        return true;
    }
    bool pending() const { return pending_; }
    template<class Visitor> bool visit(Visitor visitor) {
        uint8_t* snapshot = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!readable_ || !available_) return false;
            snapshot = readable_;
            reading_ = snapshot;
        }
        visitor(snapshot);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (reading_ == snapshot) reading_ = nullptr;
            pending_ = false;
        }
        return true;
    }
    uint32_t frames() const { return frames_; }
    uint32_t replaced() const { return replaced_; }
private:
    std::mutex mutex_;
    uint8_t* buffers_[3] = {nullptr, nullptr, nullptr};
    uint8_t* readable_ = nullptr;
    uint8_t* reading_ = nullptr;
    std::size_t size_ = 0;
    bool available_ = false;
    std::atomic<bool> pending_{false};
    std::atomic<uint32_t> frames_{0}, replaced_{0};
};
}
