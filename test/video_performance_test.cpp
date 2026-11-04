#include "video_library.h"
#include "frame_pacing.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    char temporary[] = "/tmp/storybox-video-review.XXXXXX";
    const auto* directory = mkdtemp(temporary);
    assert(directory);
    const std::filesystem::path root(directory);
    Storage::AccessGate gate;
    Video::LibraryCache cache;
    // Missing/inaccessible directory must not become a successful empty cache.
    assert(!cache.refresh(gate, root.string()));
    std::filesystem::create_directory(root / "video");
    std::ofstream(root / "video/old.avi").put('x');
    assert(cache.refresh(gate, root.string()) && cache.entries.size() == 1);

    // USB bumps revision BEFORE writing/renaming. A scan in that window must
    // not cache the old directory at the new revision, even on first entry.
    assert(gate.beginMaintenance(std::chrono::milliseconds(0)));
    gate.changed();
    Video::LibraryCache first_entry;
    assert(!cache.refresh(gate, root.string()));
    assert(!first_entry.refresh(gate, root.string()));
    assert(cache.entries.size() == 1);
    std::ofstream(root / "video/new.avi").put('x');
    gate.endMaintenance(); // no revision bump at commit
    assert(cache.refresh(gate, root.string()) && cache.entries.size() == 2);
    assert(first_entry.refresh(gate, root.string()) && first_entry.entries.size() == 2);
    assert(cache.entries[0].name == "new.avi");

    gate.changed();
    std::filesystem::rename(root / "video", root / "offline");
    assert(!cache.refresh(gate, root.string()));
    assert(cache.entries.size() == 2); // retain last good snapshot on failure
    std::filesystem::rename(root / "offline", root / "video");
    std::ofstream(root / "video/recovered.avi").put('x');
    assert(cache.refresh(gate, root.string()) && cache.entries.size() == 3);
    assert(gate.beginMaintenance(std::chrono::milliseconds(0))); // all leases released
    gate.endMaintenance();

    // 30.72ms full-screen SPI time is part of, not added to, the 33.334ms
    // interval. Slow frames get a bounded idle opportunity, not busy polling.
    assert(Video::frameWaitUs(1000000, 1030720) == 2614);
    assert(Video::frameWaitUs(1000000, 1000000) == 33334);
    assert(Video::frameWaitUs(1000000, 1033334) == 1000);
    assert(Video::frameWaitUs(1000000, 1100000) == 1000);
    std::filesystem::remove_all(root);
    std::cout << "PASS: video cache upload isolation, failed-scan retry and frame deadlines\n";
}
