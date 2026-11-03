#include "nfc_service.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
namespace NativeNfc {
namespace {
uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
// With no PN532 on the bus every Nfc::loop() probe is an I2C NACK burst (three
// error lines every 3.5 s) on the bus the FRAM shares. Keep it quiet and retry
// slowly instead, so a module fitted later is still noticed within this window.
constexpr uint32_t kAbsentRetryMs = 300000;
}
esp_err_t Service::begin(i2c_master_bus_handle_t bus) {
    if (commands_) return ESP_ERR_INVALID_STATE;
    commands_=xQueueCreate(4,sizeof(bool));
    if (!commands_) return ESP_ERR_NO_MEM;
    const auto result=Pn532::attachBus(bus);
    if (result!=ESP_OK) { vQueueDelete(commands_); commands_=nullptr; return result; }
    if (xTaskCreate([](void* self){static_cast<Service*>(self)->run();},"nfc",6144,this,2,nullptr)!=pdPASS) {
        Pn532::detachBus(); vQueueDelete(commands_); commands_=nullptr; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t Service::setPolling(bool enabled) {
    if (!commands_) return ESP_ERR_INVALID_STATE;
    return xQueueSend(commands_,&enabled,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}
Snapshot Service::snapshot() { std::lock_guard<std::mutex> lock(mutex_); return snapshot_; }
void Service::run() {
    Nfc::begin();
    bool absent = !Nfc::modulePresent();
    uint32_t retry_at = nowMs() + kAbsentRetryMs;
    for (;;) {
        bool polling;
        if (xQueueReceive(commands_,&polling,0)==pdTRUE) Nfc::setPolling(polling);
        const uint32_t now = nowMs();
        if (absent) {
            if (now >= retry_at) {
                retry_at = now + kAbsentRetryMs;
                Nfc::loop();
                absent = !Nfc::modulePresent();
            }
        } else {
            Nfc::loop();
            if (!Nfc::modulePresent()) {
                absent = true;
                retry_at = now + kAbsentRetryMs;
            }
        }
        Snapshot current;
        current.module=Nfc::modulePresent(); current.polling=Nfc::polling(); current.busy=Nfc::writeBusy();
        current.error=Pn532::lastError();
        std::snprintf(current.firmware,sizeof(current.firmware),"%s",Nfc::firmwareVersion());
        if (const auto* card=Nfc::card()) current.card=*card;
        { std::lock_guard<std::mutex> lock(mutex_); snapshot_=current; }
        // Diagnostic service exports a snapshot, not write-completion events.
        // USB write routing must add an acknowledged event channel before enable.
        Nfc::Event event;
        while (Nfc::takeEvent(event)) {}
        vTaskDelay(std::max(TickType_t{1},pdMS_TO_TICKS(20)));
    }
}
}
