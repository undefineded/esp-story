#include "fram_device.h"
#include <algorithm>
#include <cstring>

#include "esp_log.h"

namespace Board {
namespace {
constexpr const char* kTag = "fram";
// Last byte of each MB85RC16 bank. None of the persisted records reach here
// (settings 0x000, volume 0x010, position 0x020, PCTL 0x300, FAV 0x400), so a
// write/read pattern is a safe liveness and model check.
constexpr std::size_t kProbeOffsetInBank = 0xF8;
}

FramDevice::~FramDevice() { close(); }
void FramDevice::close() {
    for (auto& device : devices_) {
        if (device) i2c_master_bus_rm_device(device);
        device = nullptr;
    }
}
esp_err_t FramDevice::begin(i2c_master_bus_handle_t bus, bool confirmed) {
    if (!confirmed || !bus || devices_[0]) return ESP_ERR_INVALID_STATE;
    const unsigned count = profile_.fram == FramModel::Mb85rc16 ? 8 : 1;
    for (unsigned i = 0; i < count; ++i) {
        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = profile_.fram_address + i;
        config.scl_speed_hz = 100000;
        esp_err_t result = i2c_master_bus_add_device(bus, &config, &devices_[i]);
        if (result != ESP_OK) { close(); return result; }
    }
    // A probe that only ACKs is not proof: the legacy AT24C512 also answers on
    // 0x50. Each MB85RC16 bank must accept and return its own pattern; a wrong
    // model (FM24CL64B at 0x51, EEPROM at 0x50) fails here and the app keeps
    // settings in RAM instead of writing records to the wrong device.
    uint8_t pattern = 0;
    uint8_t readback = 0;
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t offset = profile_.fram == FramModel::Mb85rc16
                                       ? (static_cast<std::size_t>(i) << 8) + kProbeOffsetInBank
                                       : profile_.fram_capacity - 4;
        pattern = static_cast<uint8_t>(0xA5u + i * 0x11u);
        esp_err_t result = write(offset, &pattern, 1);
        if (result != ESP_OK) {
            ESP_LOGW(kTag, "bank %u write probe failed: %s", i, esp_err_to_name(result));
            close();
            return result;
        }
        result = read(offset, &readback, 1);
        if (result != ESP_OK || readback != pattern) {
            ESP_LOGW(kTag, "bank %u readback 0x%02X != 0x%02X (%s); FRAM disabled",
                     i, readback, pattern, esp_err_to_name(result));
            close();
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    ESP_LOGI(kTag, "%s FRAM online (%u bank probe%s passed)", profile_.name, count,
             count == 1 ? "" : "s");
    return ESP_OK;
}
esp_err_t FramDevice::read(std::size_t offset, uint8_t* data, std::size_t size) {
    if (!devices_[0]) return ESP_ERR_INVALID_STATE;
    if ((!data && size) || offset > profile_.fram_capacity || size > profile_.fram_capacity - offset)
        return ESP_ERR_INVALID_ARG;
    while (size) {
        Address address{};
        if (!encode(profile_, offset, size, address)) return ESP_ERR_INVALID_ARG;
        const auto chunk = std::min(size, std::min(address.contiguous, std::size_t{64}));
        auto device = devices_[profile_.fram == FramModel::Mb85rc16 ? offset >> 8 : 0];
        esp_err_t result = i2c_master_transmit_receive(device, address.bytes, address.length, data, chunk, 100);
        if (result != ESP_OK) return result;
        offset += chunk; data += chunk; size -= chunk;
    }
    return ESP_OK;
}
esp_err_t FramDevice::write(std::size_t offset, const uint8_t* data, std::size_t size) {
    if (!devices_[0]) return ESP_ERR_INVALID_STATE;
    if ((!data && size) || offset > profile_.fram_capacity || size > profile_.fram_capacity - offset)
        return ESP_ERR_INVALID_ARG;
    while (size) {
        Address address{};
        if (!encode(profile_, offset, size, address)) return ESP_ERR_INVALID_ARG;
        const auto chunk = std::min(size, std::min(address.contiguous, std::size_t{64}));
        uint8_t frame[66];
        std::memcpy(frame, address.bytes, address.length);
        std::memcpy(frame + address.length, data, chunk);
        auto device = devices_[profile_.fram == FramModel::Mb85rc16 ? offset >> 8 : 0];
        esp_err_t result = i2c_master_transmit(device, frame, address.length + chunk, 100);
        if (result != ESP_OK) return result;
        offset += chunk; data += chunk; size -= chunk;
    }
    return ESP_OK;
}
}
