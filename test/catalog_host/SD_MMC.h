#pragma once
#include "FS.h"
#include <map>
#include <string>
#define FILE_READ "r"
#define FILE_WRITE "w"
struct TestSD {
  std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> files;
  bool failWrite = false, failRename = false;
  File open(const char* path, const char* mode) {
    bool write = mode[0] == 'w';
    if (write) files[path] = std::make_shared<std::vector<uint8_t>>();
    auto it = files.find(path);
    return it == files.end() ? File() : File(it->second, write, failWrite);
  }
  bool exists(const char* path) { return files.count(path); }
  bool remove(const char* path) { return files.erase(path); }
  bool rename(const char* from, const char* to) {
    if ((failRename && std::string(from).find(".tmp") != std::string::npos) ||
        !exists(from) || exists(to)) return false;
    files[to] = files[from]; files.erase(from); return true;
  }
};
extern TestSD SD_MMC;
