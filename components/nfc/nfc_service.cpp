#include "nfc_service.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
namespace NativeNfc {
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
    for (;;) {
        bool polling;
        if (xQueueReceive(commands_,&polling,0)==pdTRUE) Nfc::setPolling(polling);
        Nfc::loop();
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
