#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include <utility>
#include "storage_access.h"

namespace Video {

struct Entry {
    std::string path;       // logical path under /video (letterboxed "fit" version)
    std::string fill_path;  // optional mirror under /video/.fill (full-screen crop)
    std::string name;       // file name with extension
    std::string title;      // display name without extension
    std::string group;      // first directory level under /video ("" for root files)
};

// Lists *.avi files under /video recursively (3 levels), sorted by title. A file
// with the same relative path inside /video/.fill is attached as fill_path
// instead of being listed separately, so the UI keeps one row per video.
// The cap is high enough for a full series library (four folders of ~50 clips
// each); the entry vector lives in PSRAM and costs about 200 bytes per file.
std::vector<Entry> scanLibrary(const std::string& root, std::size_t limit = 512);
bool scanLibraryChecked(const std::string& root, std::vector<Entry>& entries, std::size_t limit = 512);

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
