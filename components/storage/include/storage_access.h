#pragma once
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <string>
#include <cstdio>
#include <cstdint>

namespace Storage {
// Owned by StorageService; shared by audio, JPEG, catalog and file transport.
// Maintenance closes admission before waiting, without holding users' I/O locks.
class AccessGate {
public:
    uint64_t revision() { std::lock_guard<std::mutex> lock(mutex_); return revision_; }
    void changed() { std::lock_guard<std::mutex> lock(mutex_); ++revision_; }
    bool acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (maintenance_) return false;
        ++users_;
        return true;
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (users_) --users_;
        changed_.notify_all();
    }
    bool beginMaintenance(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (maintenance_) return false;
        maintenance_ = true;
        if (!changed_.wait_for(lock, timeout, [this] { return users_ == 0; })) {
            maintenance_ = false;
            return false;
        }
        return true;
    }
    void endMaintenance() {
        std::lock_guard<std::mutex> lock(mutex_);
        maintenance_ = false;
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    unsigned users_ = 0;
    uint64_t revision_=0;
    bool maintenance_ = false;
};

// Keep legacy relative-path acceptance, but reject traversal and ambiguous paths.
inline bool normalizePath(const std::string& input, std::string& output) {
    if (input.size() > 511 || input.find('\0') != std::string::npos ||
        input.find('\\') != std::string::npos) return false;
    std::string result;
    std::size_t at = 0;
    while (at < input.size()) {
        auto end = input.find('/', at);
        if (end == std::string::npos) end = input.size();
        auto part = input.substr(at, end - at);
        if (part == "..") return false;
        if (!part.empty() && part != ".") result += "/" + part;
        at = end + 1;
    }
    if (result.empty()) result = "/";
    if (result.size() > 511) return false;
    output = result;
    return true;
}

// IndexIO-compatible binary stream. Its lease lasts until fclose, not just read().
class FileStream {
public:
    explicit FileStream(AccessGate& gate) : gate_(gate) {}
    ~FileStream() { close(); }
    FileStream(const FileStream&) = delete;
    FileStream& operator=(const FileStream&) = delete;
    bool open(const std::string& root, const std::string& path, const char* mode) {
        if (file_) return false;
        std::string normalized;
        if (!normalizePath(path, normalized) || !gate_.acquire()) return false;
        file_ = std::fopen((root + normalized).c_str(), mode);
        if (!file_) gate_.release();
        return file_ != nullptr;
    }
    bool close() {
        if (!file_) return true;
        const bool ok = std::fclose(file_) == 0;
        file_ = nullptr;
        gate_.release();
        return ok;
    }
    std::size_t read(uint8_t* data, std::size_t size) {
        return file_ ? std::fread(data, 1, size, file_) : 0;
    }
    std::size_t write(const uint8_t* data, std::size_t size) {
        return file_ ? std::fwrite(data, 1, size, file_) : 0;
    }
    bool failed() const { return file_ && std::ferror(file_); }
    bool eof() const { return file_ && std::feof(file_); }
    bool available() {
        if (!file_) return false;
        const int next = std::fgetc(file_);
        if (next == EOF) return std::ferror(file_) != 0;
        return std::ungetc(next, file_) != EOF;
    }
private:
    AccessGate& gate_;
    std::FILE* file_ = nullptr;
};
}
