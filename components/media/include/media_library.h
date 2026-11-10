#pragma once
#include "storage_access.h"
#include "upload_transaction.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace Media {
enum class StoryKind { Audio, Illustrated };
// Catalog limits. Tracks are two std::string each, so 1000 tracks costs roughly
// 60-80 KiB of internal heap; the parental allowlist is a uint64 bitmask with
// one bit per category (bit 0 is "loose /music files"), hence 32 categories.
// Story is five std::string (directory/title/audio/timeline/category), i.e. 124 B
// on the 32-bit target: the 1024-entry vector is one >8 KiB malloc and therefore
// lands in PSRAM, while the individual path strings (SSO overflows at 15 chars)
// stay in internal heap. The real card already holds 656 stories, so 512 forced
// LimitExceeded and dropped the whole catalog.
inline constexpr std::size_t kMaxStories = 1024;
inline constexpr std::size_t kMaxTracks = 2000;
inline constexpr std::size_t kMaxCategories = 32;
struct Track { std::string path, name; uint32_t id; int category; };
struct Story {
    std::string directory, title, audio, timeline;
    StoryKind kind = StoryKind::Audio;
    std::string category;  // first directory under /story, used for grouping
};
// Path helpers shared by the scanner, the index cache and the UI.
std::string baseName(const std::string& path);
std::string withoutExtension(const std::string& name);
// Fills title/kind/category from directory, audio and timeline. Idempotent.
void normalizeStory(Story& story);
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
    int category() const { return category_; }
private:
    std::shared_ptr<const Catalog> catalog_;
    std::vector<std::size_t> indices_;
    std::size_t position_ = 0;
    int category_ = 0;
};
}


