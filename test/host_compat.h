#pragma once
// POSIX helpers for the host tests so they also build with MinGW on Windows.
// On POSIX this header is a no-op and the platform headers are used directly.
#if defined(_WIN32)
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <cstring>
#include <windows.h>

#define lstat stat

static inline int fsync(int fd) { return _commit(fd); }

static inline int symlink(const char* target, const char* linkpath) {
    char absolute[4096];
    if (!_fullpath(absolute, target, sizeof(absolute))) return -1;
    return CreateSymbolicLinkA(linkpath, absolute, 0) ? 0 : -1;
}
#endif
