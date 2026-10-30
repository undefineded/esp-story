#pragma once
#include "storage_access.h"
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>
#include <array>
#include <cstring>

namespace Storage {
enum class UploadResult { Ok, Busy, Invalid, IoError, RecoveryRequired };
// Injectable syscall boundary for deterministic host fault tests. Production
// uses the ordinary VFS operations; no fault hooks or globals are enabled.
struct UploadIo {
    decltype(&std::fwrite) write=&std::fwrite;
    decltype(&std::fflush) flush=&std::fflush;
    decltype(&fsync) sync=&fsync;
    decltype(&std::fclose) close=&std::fclose;
    decltype(&std::rename) rename=&std::rename;
    int (*remove)(const char*)=&std::remove;
};
// Single file-service owner. Acquire after requesting playback/JPEG shutdown.
// Stale staging/backup files require recovery before accepting another upload.
class UploadTransaction {
public:
    explicit UploadTransaction(AccessGate& gate,UploadIo io={}) : gate_(gate),io_(io) {}
    ~UploadTransaction() { cancel(); }
    UploadTransaction(const UploadTransaction&) = delete;
    UploadTransaction& operator=(const UploadTransaction&) = delete;
    UploadResult begin(const std::string& root, const std::string& path,
                       std::size_t total, std::chrono::milliseconds timeout, bool caller_owns_maintenance=false) {
        if (active_) return UploadResult::Busy;
        std::string normalized;
        if (!normalizePath(path, normalized) || normalized == "/" || normalized.size() > 480)
            return UploadResult::Invalid;
        // Only a single service already holding this gate may borrow its lease.
        if (!caller_owns_maintenance && !gate_.beginMaintenance(timeout)) return UploadResult::Busy;
        owns_gate_=!caller_owns_maintenance;
        active_ = true;
        target_ = root + normalized;
        temporary_ = target_ + ".storybox-upload";
        backup_ = target_ + ".storybox-backup";
        struct stat status{};
        if (stat(temporary_.c_str(), &status) == 0 || stat(backup_.c_str(), &status) == 0) {
            release();
            return UploadResult::RecoveryRequired;
        }
        // Exclusive create: never truncate an existing staging file.
        file_ = std::fopen(temporary_.c_str(), "wbx");
        if (!file_) { release(); return UploadResult::IoError; }
        owns_temporary_ = true;
        total_ = total;
        offset_ = 0;
        failed_ = false;
        last_offset_ = 0;
        last_size_ = 0;
        return UploadResult::Ok;
    }
    UploadResult append(std::size_t offset, const uint8_t* data, std::size_t size) {
        if (!file_ || failed_ || (!data && size) || size > 4096 || !size)
            return UploadResult::Invalid;
        // An immediate retry is idempotent only when its payload is identical.
        if (last_size_ && offset == last_offset_ && size == last_size_)
            return std::memcmp(last_data_.data(),data,size)==0 ? UploadResult::Ok : UploadResult::Invalid;
        if (offset != offset_ || size > total_ - offset_) return UploadResult::Invalid;
        if (io_.write(data, 1, size, file_) != size) {
            failed_ = true;
            return UploadResult::IoError;
        }
        last_offset_ = offset;
        last_size_ = size;
        std::memcpy(last_data_.data(),data,size);
        offset_ += size;
        return UploadResult::Ok;
    }
    UploadResult commit() {
        if (!file_ || failed_ || offset_ != total_) return UploadResult::Invalid;
        struct stat staged{};
        const bool synced = io_.flush(file_) == 0 && io_.sync(fileno(file_)) == 0 &&
            fstat(fileno(file_),&staged)==0 && staged.st_size>=0 &&
            static_cast<uint64_t>(staged.st_size)==total_;
        const bool closed = io_.close(file_) == 0;
        file_ = nullptr;
        if (!synced || !closed) { cancel(); return UploadResult::IoError; }
        struct stat status{};
        const bool existed = stat(target_.c_str(), &status) == 0;
        if (existed && !S_ISREG(status.st_mode)) { cancel(); return UploadResult::Invalid; }
        if (existed && io_.rename(target_.c_str(), backup_.c_str()) != 0) {
            cancel(); return UploadResult::IoError;
        }
        if (io_.rename(temporary_.c_str(), target_.c_str()) != 0) {
            const bool restored = !existed || io_.rename(backup_.c_str(), target_.c_str()) == 0;
            cancel();
            return restored ? UploadResult::IoError : UploadResult::RecoveryRequired;
        }
        owns_temporary_ = false;
        const bool cleaned = !existed || io_.remove(backup_.c_str()) == 0;
        release();
        return cleaned ? UploadResult::Ok : UploadResult::RecoveryRequired;
    }
    UploadResult cancel() {
        bool ok = true;
        if (file_) { ok = io_.close(file_) == 0; file_ = nullptr; }
        if (owns_temporary_) {
            if (io_.remove(temporary_.c_str()) != 0 && errno != ENOENT) ok = false;
            owns_temporary_ = false;
        }
        release();
        return ok ? UploadResult::Ok : UploadResult::IoError;
    }
private:
    void release() { if (active_ && owns_gate_) gate_.endMaintenance(); active_ = false; owns_gate_=false; }
    AccessGate& gate_;
    UploadIo io_;
    std::FILE* file_ = nullptr;
    std::string target_, temporary_, backup_;
    std::size_t total_ = 0, offset_ = 0, last_offset_ = 0, last_size_ = 0;
    bool active_ = false, owns_temporary_ = false, failed_ = false;
    bool owns_gate_=false;
    std::array<uint8_t,4096> last_data_{};
};
}
