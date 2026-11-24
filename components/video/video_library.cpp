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
    const char* suffix = name.c_str() + name.size() - 4;
    const char tail[4] = {'.', 'a', 'v', 'i'};
    for (int i = 0; i < 4; ++i) {
        if (std::tolower(static_cast<unsigned char>(suffix[i])) != tail[i]) return false;
    }
    return true;
}

std::string withoutExtension(const std::string& name) {
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

constexpr unsigned kMaxDepth = 3;

bool isDirectory(const struct dirent* item, const std::string& full) {
    if (item->d_type == DT_DIR) return true;
    if (item->d_type == DT_REG) return false;
    struct stat status {};
    return stat(full.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

bool isRegular(const struct dirent* item, const std::string& full) {
    if (item->d_type == DT_REG) return true;
    if (item->d_type == DT_DIR) return false;
    struct stat status {};
    return stat(full.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

// Relative paths of every file in the .fill mirror, gathered with readdir only.
// f_stat on FATFS walks the directory from its first entry on every call, so
// probing the mirror per clip turns a 400-clip card into an O(n^2) scan; one
// sequential pass plus a binary search costs a single walk.
void collectMirror(const std::string& root, const std::string& relative, unsigned depth,
                   std::vector<std::string>& out) {
    const std::string path = relative.empty() ? kFillDirectory : kFillDirectory + "/" + relative;
    DIR* directory = opendir((root + path).c_str());
    if (!directory) return;
    while (const auto* item = readdir(directory)) {
        const std::string name = item->d_name;
        if (name == "." || name == "..") continue;
        const std::string child = relative.empty() ? name : relative + "/" + name;
        const std::string full = root + kFillDirectory + "/" + child;
        if (isDirectory(item, full)) {
            if (depth + 1 < kMaxDepth) collectMirror(root, child, depth + 1, out);
            continue;
        }
        out.push_back(child);
    }
    closedir(directory);
}

// Walks /video (skipping the .fill mirror) and appends one entry per AVI.
// FATFS reports the entry type in d_type, so the common case needs no stat().
void collect(const std::string& root, const std::string& relative, unsigned depth,
             std::vector<Entry>& entries, std::size_t limit,
             const std::vector<std::string>& mirrors, bool& ok) {
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
        if (isDirectory(item, root + "/video/" + child)) {
            if (depth + 1 < kMaxDepth) collect(root, child, depth + 1, entries, limit, mirrors, ok);
            continue;
        }
        if (!isAvi(name)) continue;
        if (!isRegular(item, root + "/video/" + child)) continue;
        // Pre-cropped "height fill" copies mirror the relative path under
        // /video/.fill, so equal names in different folders never collide.
        std::string fill;
        if (!mirrors.empty() && std::binary_search(mirrors.begin(), mirrors.end(), child))
            fill = kFillDirectory + "/" + child;
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
    entries.reserve(std::min<std::size_t>(limit, 256));
    std::vector<std::string> mirrors;
    struct stat status {};
    if (stat((root + kFillDirectory).c_str(), &status) == 0 && S_ISDIR(status.st_mode))
        collectMirror(root, "", 0, mirrors);
    std::sort(mirrors.begin(), mirrors.end());
    collect(root, "", 0, entries, limit, mirrors, ok);
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
