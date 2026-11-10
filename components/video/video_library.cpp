#include "video_library.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <utility>

namespace Video {
namespace {

const std::string kFillDirectory = "/video/.fill";

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

constexpr unsigned kMaxDepth = 3;

// Walks /video (skipping the .fill mirror) and appends one entry per AVI.
void collect(const std::string& root, const std::string& relative, unsigned depth,
             std::vector<Entry>& entries, std::size_t limit, bool& ok) {
    if (entries.size() >= limit) return;
    const std::string directory_path =
        relative.empty() ? std::string("/video") : "/video/" + relative;
    DIR* directory = opendir((root + directory_path).c_str());
    if (!directory) {
        if (depth == 0) ok = false;  // unreadable subfolders are skipped, not fatal
        return;
    }
    while (entries.size() < limit) {
        errno = 0;
        const auto* item = readdir(directory);
        if (!item) { if (errno) ok = false; break; }
        const std::string name = item->d_name;
        if (name == "." || name == "..") continue;
        if (relative.empty() && name == ".fill") continue;
        const std::string child = relative.empty() ? name : relative + "/" + name;
        struct stat status {};
        if (stat((root + "/video/" + child).c_str(), &status) != 0) { ok = false; break; }
        if (S_ISDIR(status.st_mode)) {
            if (depth + 1 < kMaxDepth) collect(root, child, depth + 1, entries, limit, ok);
            continue;
        }
        if (!S_ISREG(status.st_mode) || !isAvi(name)) continue;
        // Pre-cropped "height fill" copies mirror the relative path under
        // /video/.fill, so equal names in different folders never collide.
        std::string fill;
        if (stat((root + kFillDirectory + "/" + child).c_str(), &status) == 0 &&
            S_ISREG(status.st_mode)) {
            fill = kFillDirectory + "/" + child;
        }
        const std::string group =
            relative.empty() ? std::string() : relative.substr(0, relative.find('/'));
        entries.push_back({"/video/" + child, fill, name, withoutExtension(name), group});
    }
    if (closedir(directory) != 0) ok = false;
}

}  // namespace

std::vector<Entry> scanLibrary(const std::string& root, std::size_t limit) {
    std::vector<Entry> entries;
    scanLibraryChecked(root, entries, limit);
    return entries;
}

bool scanLibraryChecked(const std::string& root, std::vector<Entry>& output, std::size_t limit) {
    bool ok = true;
    std::vector<Entry> entries;
    collect(root, "", 0, entries, limit, ok);
    if (!ok) return false;
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.group != b.group) return a.group < b.group;
        return a.title < b.title;
    });
    if (entries.size() > limit) entries.resize(limit);
    output = std::move(entries);
    return true;
}

std::string audioPathFor(const std::string& video_path) {
    const auto dot = video_path.find_last_of('.');
    return (dot == std::string::npos ? video_path : video_path.substr(0, dot)) + ".mp3";
}

}
