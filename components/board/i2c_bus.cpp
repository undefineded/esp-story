#include "i2c_bus.h"
#include "board_profile.h"
namespace Board {
I2cBus::~I2cBus() { if (bus_) i2c_del_master_bus(bus_); }
esp_err_t I2cBus::begin() {
    if (bus_) return ESP_ERR_INVALID_STATE;
    i2c_master_bus_config_t config{};
    config.i2c_port = I2C_NUM_0;
    config.sda_io_num = static_cast<gpio_num_t>(i2c_sda);
    config.scl_io_num = static_cast<gpio_num_t>(i2c_scl);
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    return i2c_new_master_bus(&config, &bus_);
}
esp_err_t I2cBus::recover() {
    return bus_ ? i2c_master_bus_reset(bus_) : ESP_ERR_INVALID_STATE;
}
}
