#pragma once
#include "driver/i2c_master.h"
namespace Board {
// All devices must be destroyed before their shared bus owner.
class I2cBus {
public:
    I2cBus() = default;
    I2cBus(const I2cBus&) = delete;
    I2cBus& operator=(const I2cBus&) = delete;
    ~I2cBus();
    esp_err_t begin();
    i2c_master_bus_handle_t handle() const { return bus_; }
    // Owner must first quiesce all device users; never called by PN532 alone.
    esp_err_t recover();
private:
    i2c_master_bus_handle_t bus_ = nullptr;
};
}
