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
constexpr uint32_t kCommandTimeoutMs = 1000;
}
esp_err_t Service::begin(i2c_master_bus_handle_t bus, bool enabled) {
    if (commands_) return ESP_ERR_INVALID_STATE;
    commands_=xQueueCreate(4,sizeof(Command));
    if (!commands_) return ESP_ERR_NO_MEM;
    done_=xSemaphoreCreateBinary();
    if (!done_) { vQueueDelete(commands_); commands_=nullptr; return ESP_ERR_NO_MEM; }
    const auto result=Pn532::attachBus(bus);
    if (result!=ESP_OK) {
        vSemaphoreDelete(done_); done_=nullptr;
        vQueueDelete(commands_); commands_=nullptr;
        return result;
    }
    enabled_.store(enabled,std::memory_order_release);
    if (xTaskCreate([](void* self){static_cast<Service*>(self)->run();},"nfc",6144,this,2,&task_)!=pdPASS) {
        Pn532::detachBus(); vSemaphoreDelete(done_); done_=nullptr;
        vQueueDelete(commands_); commands_=nullptr; task_=nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool Service::setEnabled(bool enabled) {
    if (!commands_) return false;
    Command command;
    command.action=Action::Enable;
    command.on=enabled;
    // Enabling must be accepted while the service is disabled.
    return submit(command,true);
}

bool Service::submit(Command& command, bool allowDisabled) {
    if (!commands_ || !done_) return false;
    if (!allowDisabled && !enabled_.load(std::memory_order_acquire)) return false;
    // The driver state machine can only be entered from its own task.
    if (task_ && xTaskGetCurrentTaskHandle()==task_) return false;
    // Requests are serialised here so one binary semaphore is enough; the
    // PN532 protocol itself is serial anyway.
    std::lock_guard<std::mutex> lock(submit_mutex_);
    command.wait=true;
    if (xQueueSend(commands_,&command,pdMS_TO_TICKS(kCommandTimeoutMs))!=pdTRUE) return false;
    // Every Nfc operation is bounded by its I2C timeouts, so this wait is too.
    return xSemaphoreTake(done_,portMAX_DELAY)==pdTRUE;
}

esp_err_t Service::setPolling(bool enabled) {
    if (!commands_ || !enabled_.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    Command command;
    command.action=Action::Poll;
    command.on=enabled;
    return xQueueSend(commands_,&command,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;
}

bool Service::pollNow(bool enabled) {
    Command command;
    command.action=Action::Poll;
    command.on=enabled;
    return submit(command);
}

bool Service::refresh(const char** error) {
    const char* unused="";
    if (!error) error=&unused;
    *error="";
    Command command;
    command.action=Action::Refresh;
    command.error=error;
    if (!submit(command)) { *error="BUSY"; return false; }
    return (*error)[0]==0;
}

bool Service::readRaw(bool block, uint16_t start, uint16_t count,
                      const uint8_t* key, bool hasKey, Nfc::RawResult& out) {
    out=Nfc::RawResult{};
    Command command;
    command.action=Action::Read;
    command.block=block;
    command.start=start;
    command.count=count;
    command.key=key;
    command.hasKey=hasKey;
    command.raw=&out;
    if (!submit(command)) { out.error="BUSY"; return false; }
    return out.ok;
}

bool Service::startWriteNdef(const uint8_t* ndef, uint16_t len, bool verify,
                             const char** error) {
    const char* unused="";
    if (!error) error=&unused;
    *error="";
    Command command;
    command.action=Action::WriteNdef;
    command.data=ndef;
    command.len=len;
    command.verify=verify;
    command.error=error;
    if (!submit(command)) { *error="BUSY"; return false; }
    return (*error)[0]==0;
}

bool Service::startWriteRaw(bool block, uint16_t start, const uint8_t* data, uint16_t len,
                            const uint8_t* key, bool hasKey, const char** error) {
    const char* unused="";
    if (!error) error=&unused;
    *error="";
    Command command;
    command.action=Action::WriteRaw;
    command.block=block;
    command.start=start;
    command.data=data;
    command.len=len;
    command.key=key;
    command.hasKey=hasKey;
    command.error=error;
    if (!submit(command)) { *error="BUSY"; return false; }
    return (*error)[0]==0;
}

void Service::execute(Command& command) {
    switch (command.action) {
        case Action::Enable:
            enabled_.store(command.on,std::memory_order_release);
            if (!command.on) Nfc::setPolling(false);
            break;
        case Action::Poll:      Nfc::setPolling(command.on); break;
        case Action::Refresh:
            // A refresh while pages are being written cannot re-read the same
            // target without corrupting the job. The write already forces a
            // re-read when it finishes, so acknowledge instead of failing the
            // host request (the old firmware also answered ok here).
            if (!Nfc::writeBusy()) Nfc::refreshCard();
            break;
        case Action::Read:
            // A read would interleave with the in-flight page writes and fight
            // the same Pn532 target; report busy instead of corrupting both.
            if (command.raw) {
                if (Nfc::writeBusy()) command.raw->error="BUSY";
                else Nfc::readRaw(command.block,command.start,command.count,
                                  command.key,command.hasKey,*command.raw);
            }
            break;
        case Action::WriteNdef:
            Nfc::startWriteNdef(command.data,command.len,command.verify,command.error);
            break;
        case Action::WriteRaw:
            Nfc::startWriteRaw(command.block,command.start,command.data,command.len,
                               command.key,command.hasKey,command.error);
            break;
    }
}

Snapshot Service::snapshot() { std::lock_guard<std::mutex> lock(mutex_); return snapshot_; }

void Service::run() {
    bool initialized = false;
    bool absent = true;
    uint32_t retry_at = nowMs() + kAbsentRetryMs;

    auto publishSnapshot = [this](bool enabled) {
        Snapshot current;
        current.enabled = enabled;
        if (enabled) {
            current.module=Nfc::modulePresent();
            current.polling=Nfc::polling();
            current.busy=Nfc::writeBusy();
            current.error=Pn532::lastError();
            std::snprintf(current.firmware,sizeof(current.firmware),"%s",Nfc::firmwareVersion());
            if (const auto* card=Nfc::card()) current.card=*card;
        } else {
            current.module=false;
            current.polling=false;
            current.busy=false;
            current.error=Pn532::NoDevice;
            std::snprintf(current.firmware,sizeof(current.firmware),"--");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_=current;
    };

    if (enabled_.load()) {
        Nfc::begin();
        initialized = true;
        absent = !Nfc::modulePresent();
        retry_at = nowMs() + kAbsentRetryMs;
    }

    for (;;) {
        Command command;
        const bool received = xQueueReceive(commands_,&command,0)==pdTRUE;
        if (received) {
            if (command.action == Action::Enable) {
                enabled_.store(command.on,std::memory_order_release);
                if (!command.on) {
                    if (initialized) Nfc::setPolling(false);
                    initialized = false;
                    absent = true;
                    retry_at = nowMs() + kAbsentRetryMs;
                } else if (!initialized) {
                    Nfc::begin();
                    initialized = true;
                    absent = !Nfc::modulePresent();
                    retry_at = nowMs() + kAbsentRetryMs;
                }
            } else if (!enabled_.load()) {
                if (command.error) *command.error="NFC_DISABLED";
                if (command.raw) command.raw->error="NFC_DISABLED";
            } else {
                if (!initialized) {
                    Nfc::begin();
                    initialized = true;
                    absent = !Nfc::modulePresent();
                    retry_at = nowMs() + kAbsentRetryMs;
                }
                execute(command);
                // A refresh can bring a module that was absent back online.
                absent = !Nfc::modulePresent();
                // A host asking to poll is also a request to look for a module
                // that was missing at boot; without this the 5-minute absent
                // backoff would swallow the request and the desktop would keep
                // seeing present:false after a hot-plug.
                if (command.action == Action::Poll && command.on) retry_at = nowMs();
            }
        }

        if (!enabled_.load()) {
            // Master switch off: no Nfc::loop, no probing, no I2C traffic.
            publishSnapshot(false);
            if (received && command.wait) xSemaphoreGive(done_);
            vTaskDelay(std::max(TickType_t{1},pdMS_TO_TICKS(50)));
            continue;
        }

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
        publishSnapshot(true);
        // Write/card/module events are consumed by the application, which turns
        // them into USB "evt":"nfc" frames. Nothing is drained here: a second
        // consumer would steal events from the host.
        if (received && command.wait) xSemaphoreGive(done_);
        vTaskDelay(std::max(TickType_t{1},pdMS_TO_TICKS(20)));
    }
}
}
