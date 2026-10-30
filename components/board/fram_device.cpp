#include "fram_device.h"
#include <algorithm>
#include <cstring>

namespace Board {
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
