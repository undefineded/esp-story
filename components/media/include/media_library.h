#pragma once
#include "storage_access.h"
#include "upload_transaction.h"
#include <memory>
#include <vector>

namespace Media {
struct Track { std::string path, name; uint32_t id; int category; };
struct Story { std::string directory, title, audio, timeline; };
struct Catalog {
    uint64_t generation = 0;
    uint64_t storage_revision = 0;
    std::vector<std::string> categories;
    std::vector<Track> tracks;
    std::vector<Story> stories;
};
enum class ScanResult { Ok, Busy, IoError, LimitExceeded };
class Library {
public:
    explicit Library(Storage::AccessGate& gate) : gate_(gate), catalog_(std::make_shared<Catalog>()) {}
    ScanResult scan(const std::string& root);
    bool loadCache(const std::string& root); // legacy v2 primary, then .bak
    Storage::UploadResult saveCache(const std::string& root);
    std::shared_ptr<const Catalog> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_); return catalog_;
    }
    static uint32_t nameId(const std::string& name);
    static bool nursery(const std::string& category);
private:
    Storage::AccessGate& gate_;
    mutable std::mutex mutex_;
    std::mutex scan_mutex_;
    std::shared_ptr<const Catalog> catalog_;
};
// Browsing does not modify the active playlist. Capture the catalog snapshot
// when selecting playback, so a later catalog publication cannot invalidate it.
class Playlist {
public:
    bool select(std::shared_ptr<const Catalog> catalog, int category,
                bool nursery_only = false, uint64_t allowed = ~uint64_t{0});
    const Track* current() const;
    bool move(int delta);
    bool seekId(uint32_t id);
    std::size_t size() const { return indices_.size(); }
    std::size_t position() const { return position_; }
private:
    std::shared_ptr<const Catalog> catalog_;
    std::vector<std::size_t> indices_;
    std::size_t position_ = 0;
};
}
