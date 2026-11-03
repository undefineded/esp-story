#pragma once
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace Ota {

struct Status {
    bool supported = false;   // dual-slot layout with an update partition
    bool active = false;      // an update is in progress
    std::size_t written = 0;
    std::size_t expected = 0;
};

// Starts an update on the inactive slot. Fails when the layout is single-slot
// or another update is already running.
esp_err_t begin(std::size_t expected_bytes);
esp_err_t write(const uint8_t* data, std::size_t size);
// Ends the update and selects the new slot for the next boot. Does not reboot.
esp_err_t finish();
void abort();
Status status();

}
