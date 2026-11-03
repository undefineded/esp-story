#include "app.h"
#include "diagnostics.h"
#include "usb_transport.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Product entry point. Peripheral diagnostics take precedence when enabled:
// those sdkconfig fragments are hardware acceptance builds, not products.
extern "C" void app_main()
{
    constexpr const char *tag = "storybox";
#if CONFIG_STORYBOX_USB_PROTOCOL
    ESP_ERROR_CHECK(Host::startUsb());
#endif
#if CONFIG_STORYBOX_BOARD_REV_20261028
    constexpr const char *board = "rev_20261028 (unverified)";
#else
    constexpr const char *board = "legacy (unverified)";
#endif
    ESP_LOGI(tag, "board=%s; IDF=%s; reset=%d",
             board, esp_get_idf_version(), static_cast<int>(esp_reset_reason()));
    uint32_t flash_bytes = 0;
    const esp_err_t result = esp_flash_get_size(nullptr, &flash_bytes);
    if (result == ESP_OK) {
        ESP_LOGI(tag, "Flash=%lu bytes", static_cast<unsigned long>(flash_bytes));
    } else {
        ESP_LOGE(tag, "Flash query failed: %s", esp_err_to_name(result));
    }
    ESP_LOGI(tag, "PSRAM=%u bytes; internal free=%u; largest=%u",
             static_cast<unsigned>(esp_psram_get_size()),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#if CONFIG_STORYBOX_PERIPHERAL_DIAGNOSTICS
    ESP_LOGW(tag, "Peripheral diagnostics build; product UI is not started");
    if (xTaskCreate([](void*) { runDiagnostics(); vTaskDelete(nullptr); },
                    "diagnostics", 12288, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGE(tag, "Cannot start diagnostics: insufficient memory");
    }
#elif CONFIG_STORYBOX_APP_UI
    if (!App::start()) {
        ESP_LOGE(tag, "Cannot start the product UI");
    }
#else
    ESP_LOGW(tag, "No UI selected (CONFIG_STORYBOX_APP_UI is off)");
#endif
}
