#pragma once
#include "board_profile.h"
#include "driver/i2c_master.h"

namespace Board {
// Bus lifetime belongs to the caller; never deletes or resets a shared bus.
// Construct/use from the persistence owner task; serialize multi-part records there.
class FramDevice {
public:
    explicit FramDevice(const Profile& profile) : profile_(profile) {}
    ~FramDevice();
    FramDevice(const FramDevice&) = delete;
    FramDevice& operator=(const FramDevice&) = delete;
    esp_err_t begin(i2c_master_bus_handle_t bus, bool hardware_confirmed);
    esp_err_t read(std::size_t offset, uint8_t* data, std::size_t size);
    esp_err_t write(std::size_t offset, const uint8_t* data, std::size_t size);
private:
    void close();
    Profile profile_;
    i2c_master_dev_handle_t devices_[8]{};
};
}
