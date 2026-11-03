#include "power_service.h"

#include <cstdio>

#include "config.h"
#include "display.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "sdkconfig.h"

#if CONFIG_STORYBOX_POWER_SLEEP
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#endif

namespace Power {
namespace {

constexpr const char* kTag = "power";

bool s_screen_off = false;
uint8_t s_saved_brightness = 100;
uint32_t s_seen_input = 0;

const int kWakePins[] = {ENC_A, ENC_B, ENC_SW};

void screenOn() {
    if (!s_screen_off) return;
    s_screen_off = false;
    Display::setBrightness(s_saved_brightness);
    Input::swallowFor(600);  // do not let the wake press trigger a page action
    ESP_LOGI(kTag, "screen on");
}

void screenOffNow() {
    if (s_screen_off) return;
    s_screen_off = true;
    Display::setBrightness(0);
    ESP_LOGI(kTag, "screen off (idle)");
}

#if CONFIG_STORYBOX_POWER_SLEEP
[[noreturn]] void deepSleepNow() {
    ESP_LOGI(kTag, "deep sleep (no playback, idle window elapsed)");
    Display::setBrightness(0);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Keep the RTC domain powered so the wake pins keep their pull-ups;
    // otherwise floating pins can wake the board immediately.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
    uint64_t mask = 0;
    for (const int pin : kWakePins) {
        const auto gpio = static_cast<gpio_num_t>(pin);
        rtc_gpio_init(gpio);
        rtc_gpio_set_direction(gpio, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(gpio);
        rtc_gpio_pulldown_dis(gpio);
        mask |= (uint64_t{1} << static_cast<uint32_t>(pin));
    }
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}
#endif

}  // namespace

void begin() {
    s_seen_input = Input::lastActivityMs();
    s_screen_off = false;
    const auto cause = esp_sleep_get_wakeup_cause();
    if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(kTag, "wake cause=%d", static_cast<int>(cause));
    }
#if !CONFIG_STORYBOX_POWER_SLEEP
    ESP_LOGW(kTag, "deep sleep disabled in this build (CONFIG_STORYBOX_POWER_SLEEP=n)");
#endif
}

bool loop(uint32_t now_ms, const Inputs& inputs, const Config& config) {
    (void)now_ms;
    s_saved_brightness = config.brightness;
    const uint32_t activity = Input::lastActivityMs();
    if (activity != s_seen_input) {
        s_seen_input = activity;
        if (s_screen_off) screenOn();
    }

    Inputs effective = inputs;
    effective.idle_ms = now_ms - activity;
    const auto decision = evaluate(effective, config);

    if (decision.deep_sleep) {
#if CONFIG_STORYBOX_POWER_SLEEP
        deepSleepNow();
#else
        if (!s_screen_off) screenOffNow();
#endif
    } else if (decision.screen_off) {
        screenOffNow();
    }
    return s_screen_off;
}

bool screenOff() { return s_screen_off; }

void wake(uint8_t brightness) {
    s_saved_brightness = brightness;
    screenOn();
}

}
