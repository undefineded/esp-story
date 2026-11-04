#include "video_library.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>

namespace Video {
namespace {

bool isAvi(const std::string& name) {
    if (name.size() < 4) return false;
    std::string suffix = name.substr(name.size() - 4);
    for (auto& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return suffix == ".avi";
}

std::string withoutExtension(const std::string& name) {
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

}  // namespace

std::vector<Entry> scanLibrary(const std::string& root, std::size_t limit) {
    std::vector<Entry> entries;
    scanLibraryChecked(root, entries, limit);
    return entries;
}

bool scanLibraryChecked(const std::string& root, std::vector<Entry>& output, std::size_t limit) {
    std::vector<Entry> entries;
    DIR* directory = opendir((root + "/video").c_str());
    if (!directory) return false;
    bool ok = true;
    while (entries.size() < limit) {
        errno = 0;
        const auto* item = readdir(directory);
        if (!item) { ok = errno == 0; break; }
        const std::string name = item->d_name;
        if (name == "." || name == ".." || !isAvi(name)) continue;
        struct stat status {};
        const std::string full = "/video/" + name;
        if (stat((root + full).c_str(), &status) != 0) { ok = false; break; }
        if (!S_ISREG(status.st_mode)) continue;
        entries.push_back({full, name, withoutExtension(name)});
        if (entries.size() >= limit) break;
    }
    if (closedir(directory) != 0) ok = false;
    if (!ok) return false;
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    output = std::move(entries);
    return true;
}

std::string audioPathFor(const std::string& video_path) {
    const auto dot = video_path.find_last_of('.');
    return (dot == std::string::npos ? video_path : video_path.substr(0, dot)) + ".mp3";
}

}
