#pragma once
#include <cstddef>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

namespace Media {

// Bulk data belongs in PSRAM, but the catalog is a pile of small std::string
// allocations (a full card is ~656 stories / 285 tracks, measured at 39 KiB of
// internal DRAM in 40-60 byte path strings). With
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192 every one of those lands in internal
// RAM, which is the same pool the video pipeline needs for its 23 KiB I2S DMA
// ring - that is how loading the index ended up breaking video playback.
//
// Lowering the malloc-to-PSRAM threshold for the duration of a catalog build
// moves the strings next to the canvas without changing any type. The window is
// short (a boot-time index read, or one scan behind the scan screen) and only
// requires that nothing allocates a DMA buffer inside it; the catalog path is
// pure file/string work, and every DMA buffer here uses explicit heap_caps caps
// which this threshold does not affect.
class ExtmemWindow {
public:
    // 16 B is the std::string SSO boundary: shorter text never reaches the heap,
    // so this moves every string that allocates at all, and nothing that fits
    // inside the (already PSRAM-resident) vector.
    explicit ExtmemWindow(std::size_t limit = 16) { heap_caps_malloc_extmem_enable(limit); }
    ~ExtmemWindow() { heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL); }
    ExtmemWindow(const ExtmemWindow&) = delete;
    ExtmemWindow& operator=(const ExtmemWindow&) = delete;
};

}
