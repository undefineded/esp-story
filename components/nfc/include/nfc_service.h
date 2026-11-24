#pragma once
#include "nfc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <atomic>
#include <mutex>
namespace NativeNfc {
struct Snapshot {
    bool enabled=true,module=false,polling=false,busy=false;
    Nfc::Card card;
    char firmware[12]="--";
    Pn532::Error error=Pn532::NoDevice;
};
// Application-lifetime sole owner for the legacy NFC state machine.
//
// Pn532 and Nfc keep all of their state in file-static globals, so every call
// has to run on one task. The USB route therefore submits requests here and
// waits for this task instead of touching the driver from the protocol worker,
// which would race the polling loop and the I2C transactions it owns.
class Service {
public:
    Service()=default;
    Service(const Service&)=delete;
    Service& operator=(const Service&)=delete;
    esp_err_t begin(i2c_master_bus_handle_t bus, bool enabled = true);
    // Master switch. Disabling stops polling and suspends every PN532/I2C access
    // until enabled again; the NFC task stays alive but idle.
    bool setEnabled(bool enabled);
    bool enabled() const { return enabled_.load(std::memory_order_acquire); }
    // Boot-time fire-and-forget so the UI never waits for a missing module.
    esp_err_t setPolling(bool enabled);
    // Synchronous requests, executed on the NFC owner task. The caller blocks
    // while the PN532 transaction runs; never call these from the NFC task.
    bool pollNow(bool enabled);
    bool refresh(const char** error = nullptr);
    bool readRaw(bool block, uint16_t start, uint16_t count,
                 const uint8_t* key, bool hasKey, Nfc::RawResult& out);
    bool startWriteNdef(const uint8_t* ndef, uint16_t len, bool verify,
                        const char** error);
    bool startWriteRaw(bool block, uint16_t start, const uint8_t* data, uint16_t len,
                       const uint8_t* key, bool hasKey, const char** error);
    Snapshot snapshot();
private:
    enum class Action : uint8_t { Poll, Refresh, Read, WriteNdef, WriteRaw, Enable };
    struct Command {
        Action action = Action::Poll;
        bool on = false;
        bool block = false;
        bool hasKey = false;
        bool verify = false;
        uint16_t start = 0;
        uint16_t count = 0;
        const uint8_t* data = nullptr;
        uint16_t len = 0;
        const uint8_t* key = nullptr;
        Nfc::RawResult* raw = nullptr;
        const char** error = nullptr;
        // True for synchronous calls: the owner task signals done_ when the
        // request has been executed and the snapshot is up to date.
        bool wait = false;
    };
    bool submit(Command& command, bool allowDisabled = false);
    void execute(Command& command);
    void run();
    QueueHandle_t commands_=nullptr;
    SemaphoreHandle_t done_=nullptr;
    TaskHandle_t task_=nullptr;
    std::atomic<bool> enabled_{true};
    std::mutex submit_mutex_;
    std::mutex mutex_;
    Snapshot snapshot_;
};
}
