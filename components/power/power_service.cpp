#include "power_service.h"

#include <cstdio>

#include "config.h"
#include "display.h"
#include "esp_err.h"
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

// One-line diagnostics for the idle policy: the windows as configured, and the
// reason deep sleep did not happen once the window had elapsed. Both are silent
// unless something changes, so they cost one log line per boot / per idle cycle.
uint16_t s_logged_screen_min = 0xFFFF;
uint16_t s_logged_sleep_min = 0xFFFF;
bool s_blocked_logged = false;
// Set when the wake pins could not be armed: sleeping without a wake source
// would need a power cycle to recover, so this boot stays awake instead.
bool s_sleep_unavailable = false;

const int kWakePins[] = {ENC_A, ENC_B, ENC_SW};

// `swallow` belongs to a wake caused by the user: the press that turned the panel
// back on must not also act on the page. A panel that comes back because
// playback ended is not a key press and must not eat the next one.
void screenOn(bool swallow) {
    if (!s_screen_off) return;
    s_screen_off = false;
    Display::setBrightness(s_saved_brightness);
    if (swallow) Input::swallowFor(600);
    ESP_LOGI(kTag, "%s", swallow ? "screen on" : "screen on (playback ended)");
}

void screenOffNow(bool playing) {
    if (s_screen_off) return;
    s_screen_off = true;
    Display::setBrightness(0);
    ESP_LOGI(kTag, "screen off (%s)", playing ? "playing, audio continues" : "idle");
}

#if CONFIG_STORYBOX_POWER_SLEEP
// Returns false when the wake pins could not be armed. Entering deep sleep then
// would leave a board that only a power cycle can wake, so stay awake instead.
bool deepSleepNow() {
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
    const esp_err_t armed = esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (armed != ESP_OK) {
        ESP_LOGE(kTag, "wake pins cannot arm ext1 (%s); staying awake", esp_err_to_name(armed));
        Display::setBrightness(s_saved_brightness);
        return false;
    }
    ESP_LOGI(kTag, "deep sleep (no playback, idle window elapsed)");
    esp_deep_sleep_start();
    return false;  // esp_deep_sleep_start() does not return
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
    // The policy header is shared with the host tests, so the build capability
    // (deep sleep compiled in or not) is injected here rather than in Config.
    Config effective_config = config;
#if CONFIG_STORYBOX_POWER_SLEEP
    effective_config.deep_sleep_supported = true;
#else
    effective_config.deep_sleep_supported = false;
#endif
    const uint32_t activity = Input::lastActivityMs();
    if (activity != s_seen_input) {
        s_seen_input = activity;
        s_blocked_logged = false;
        if (s_screen_off) screenOn(true);
    }

    if (config.screen_off_min != s_logged_screen_min || config.sleep_min != s_logged_sleep_min) {
        s_logged_screen_min = config.screen_off_min;
        s_logged_sleep_min = config.sleep_min;
        ESP_LOGI(kTag, "idle policy: screen off %u min, deep sleep %u min (0 = off)",
                 static_cast<unsigned>(config.screen_off_min),
                 static_cast<unsigned>(config.sleep_min));
    }

    Inputs effective = inputs;
    effective.idle_ms = now_ms - activity;
    const auto decision = evaluate(effective, effective_config);

    // The window elapsed but the device stayed awake: say which input blocked it.
    // A build without deep sleep never sleeps by design, so it is not "blocked".
    const uint64_t sleep_window_ms = uint64_t{effective_config.sleep_min} * 60000u;
    if (effective_config.deep_sleep_supported && !decision.deep_sleep && sleep_window_ms &&
        effective.idle_ms >= sleep_window_ms && !s_blocked_logged) {
        s_blocked_logged = true;
        ESP_LOGW(kTag, "deep sleep blocked: audio=%d video=%d host=%d idle=%u ms",
                 static_cast<int>(effective.audio_playing),
                 static_cast<int>(effective.video_playing),
                 static_cast<int>(effective.host_active),
                 static_cast<unsigned>(effective.idle_ms));
    }

    // Only a build with deep sleep can report this decision; without it the
    // policy keeps using screen_off_min for idle blanking instead.
    if (decision.deep_sleep) {
#if CONFIG_STORYBOX_POWER_SLEEP
        if (!s_sleep_unavailable && !deepSleepNow()) s_sleep_unavailable = true;
#endif
    }
    // The panel blanks only while audio keeps playing. When playback ends on its
    // own (track finished) or a parent stops it over BLE, the policy stops asking
    // for a dark panel and the screen has to come back by itself - the user is
    // not going to press a key to find out why everything went black. A build
    // that enters deep sleep instead never reaches this branch.
    if (s_screen_off && !decision.screen_off && !decision.deep_sleep) screenOn(false);
    if (decision.screen_off) screenOffNow(inputs.audio_playing);
    return s_screen_off;
}

bool screenOff() { return s_screen_off; }

void wake(uint8_t brightness) {
    s_saved_brightness = brightness;
    screenOn(false);
}

}
