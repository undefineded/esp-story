#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include <utility>
#include "storage_access.h"

namespace Video {

struct Entry {
    std::string path;   // logical path under /video
    std::string name;   // file name with extension
    std::string title;  // display name without extension
};

// Lists *.avi files in /video (non-recursive, sorted by name).
std::vector<Entry> scanLibrary(const std::string& root, std::size_t limit = 64);
bool scanLibraryChecked(const std::string& root, std::vector<Entry>& entries, std::size_t limit = 64);

class LibraryCache {
public:
    bool refresh(Storage::AccessGate& gate, const std::string& root) {
        if (!gate.acquire()) return false;
        struct Lease { Storage::AccessGate& gate; ~Lease() { gate.release(); } } lease{gate};
        const auto revision = gate.revision();
        if (scanned_ && revision == revision_) return true;
        std::vector<Entry> next;
        if (!scanLibraryChecked(root, next)) return false;
        entries = std::move(next);
        revision_ = revision;
        scanned_ = true;
        return true;
    }
    std::vector<Entry> entries;
private:
    bool scanned_ = false;
    uint64_t revision_ = 0;
};

// "/video/小星星.mp3" for "/video/小星星.avi" (may not exist).
std::string audioPathFor(const std::string& video_path);

}
