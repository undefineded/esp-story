#include "sd_volume.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

namespace Storage {
esp_err_t SdVolume::mount() {
    if (card_) return ESP_ERR_INVALID_STATE;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_12;
    slot.cmd = GPIO_NUM_11;
    slot.d0 = GPIO_NUM_13;
    // GPIO10 is DAT3, not a mechanical card-detect switch.
    slot.cd = GPIO_NUM_NC;
    slot.wp = GPIO_NUM_NC;
    esp_vfs_fat_sdmmc_mount_config_t config{};
    config.format_if_mount_failed = false;
    config.max_files = 8;
    config.allocation_unit_size = 16 * 1024;
    sdmmc_card_t* card = nullptr;
    const auto result = esp_vfs_fat_sdmmc_mount(root, &host, &slot, &config, &card);
    if (result == ESP_OK) card_ = card;
    return result;
}
esp_err_t SdVolume::unmount() {
    if (!card_) return ESP_ERR_INVALID_STATE;
    const auto result = esp_vfs_fat_sdcard_unmount(root, card_);
    if (result == ESP_OK) card_ = nullptr;
    return result;
}
}
