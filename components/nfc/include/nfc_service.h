#pragma once
#include "nfc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <mutex>
namespace NativeNfc {
struct Snapshot {
    bool module=false,polling=false,busy=false;
    Nfc::Card card;
    char firmware[12]="--";
    Pn532::Error error=Pn532::NoDevice;
};
// Application-lifetime sole owner for the legacy NFC state machine.
class Service {
public:
    Service()=default;
    Service(const Service&)=delete;
    Service& operator=(const Service&)=delete;
    esp_err_t begin(i2c_master_bus_handle_t bus);
    esp_err_t setPolling(bool enabled);
    Snapshot snapshot();
private:
    void run();
    QueueHandle_t commands_=nullptr;
    std::mutex mutex_;
    Snapshot snapshot_;
};
}
