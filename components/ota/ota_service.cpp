#include "ota_service.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

namespace Ota {
namespace {

constexpr const char* kTag = "ota";

bool s_active = false;
std::size_t s_written = 0;
std::size_t s_expected = 0;
esp_ota_handle_t s_handle = 0;
const esp_partition_t* s_target = nullptr;

}  // namespace

esp_err_t begin(std::size_t expected_bytes) {
    if (s_active) return ESP_ERR_INVALID_STATE;
    const auto* target = esp_ota_get_next_update_partition(nullptr);
    if (!target) {
        ESP_LOGW(kTag, "no OTA slot in this partition layout");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (expected_bytes == 0 || expected_bytes > target->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    const auto result = esp_ota_begin(target, expected_bytes, &s_handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "ota begin: %s", esp_err_to_name(result));
        return result;
    }
    s_target = target;
    s_written = 0;
    s_expected = expected_bytes;
    s_active = true;
    ESP_LOGI(kTag, "update started on %s (%u bytes)", target->label,
             static_cast<unsigned>(expected_bytes));
    return ESP_OK;
}

esp_err_t write(const uint8_t* data, std::size_t size) {
    if (!s_active || !data || !size) return ESP_ERR_INVALID_STATE;
    if (s_written + size > s_expected) return ESP_ERR_INVALID_SIZE;
    const auto result = esp_ota_write(s_handle, data, size);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "ota write at %u: %s", static_cast<unsigned>(s_written), esp_err_to_name(result));
        return result;
    }
    s_written += size;
    return ESP_OK;
}

esp_err_t finish() {
    if (!s_active) return ESP_ERR_INVALID_STATE;
    esp_err_t result = esp_ota_end(s_handle);
    s_active = false;
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "ota end: %s", esp_err_to_name(result));
        return result;
    }
    result = esp_ota_set_boot_partition(s_target);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "set boot partition: %s", esp_err_to_name(result));
        return result;
    }
    ESP_LOGI(kTag, "update staged on %s; reboot to apply", s_target->label);
    return ESP_OK;
}

void abort() {
    if (!s_active) return;
    esp_ota_abort(s_handle);
    s_active = false;
    s_written = s_expected = 0;
    ESP_LOGW(kTag, "update aborted");
}

Status status() {
    Status value;
    value.supported = esp_ota_get_next_update_partition(nullptr) != nullptr;
    value.active = s_active;
    value.written = s_written;
    value.expected = s_expected;
    return value;
}

}
