#include "hall.h"
#include "config.h"
#ifndef ARDUINO
#include "driver/gpio.h"
#include "esp_err.h"
#endif

namespace Hall {

void begin() {
#ifdef ARDUINO
  pinMode(HALL_PIN, INPUT_PULLUP);   // 推挽输出会覆盖内部上拉
#else
  gpio_config_t config{};
  config.pin_bit_mask = 1ULL << HALL_PIN;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&config));
#endif
}

#ifdef ARDUINO
int level() { return digitalRead(HALL_PIN); }
#else
int level() { return gpio_get_level(static_cast<gpio_num_t>(HALL_PIN)); }
#endif

bool detected() { return level() == 0; }

}
