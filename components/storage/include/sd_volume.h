#pragma once
#include "esp_err.h"
#include "sdmmc_cmd.h"

namespace Storage {
// Owner-task API. Mount/unmount only after all users have released their files.
class SdVolume {
public:
    SdVolume() = default;
    SdVolume(const SdVolume&) = delete;
    SdVolume& operator=(const SdVolume&) = delete;
    esp_err_t mount();
    esp_err_t unmount();
    bool mounted() const { return card_ != nullptr; }
    static constexpr const char* root = "/sdcard";
private:
    sdmmc_card_t* card_ = nullptr;
};
}
