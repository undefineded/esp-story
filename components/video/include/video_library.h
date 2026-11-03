#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace Video {

struct Entry {
    std::string path;   // logical path under /video
    std::string name;   // file name with extension
    std::string title;  // display name without extension
};

// Lists *.avi files in /video (non-recursive, sorted by name). The sibling
// .mp3 is resolved by the player through withoutExtension(title).
std::vector<Entry> scanLibrary(const std::string& root, std::size_t limit = 64);

// "/video/小星星.mp3" for "/video/小星星.avi" (may not exist).
std::string audioPathFor(const std::string& video_path);

}
