#include "usb_transport.h"
#include "native_protocol.h"
#include "file_reader.h"
#include "transfer_frame.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace Host {
namespace {
constexpr std::size_t JsonCapacity=4096;
struct Packet { std::size_t size; uint8_t* data; };
struct Command { std::size_t size,raw_size; uint8_t json[1024],raw[4096]; };
QueueHandle_t tx_queue=nullptr,command_queue=nullptr;
TaskHandle_t tx_task=nullptr,rx_task=nullptr,worker_task=nullptr;
std::atomic<Storage::FileReader*> file_reader{nullptr};
TransferFrame::Decoder decoder;
void enqueue(Packet packet) {
    if (xQueueSend(tx_queue,&packet,0)!=pdTRUE) std::free(packet.data);
}
int logOutput(const char* format,va_list args) {
    char buffer[256];
    const int size=std::vsnprintf(buffer,sizeof(buffer),format,args);
    if (size<=0) return size;
    Packet packet{static_cast<std::size_t>(std::min(size,255)),nullptr};
    packet.data=static_cast<uint8_t*>(std::malloc(packet.size));
    if (packet.data) { std::memcpy(packet.data,buffer,packet.size); enqueue(packet); }
    return size;
}
void transmit(void*) {
    Packet packet;
    for (;;) {
        if (xQueueReceive(tx_queue,&packet,portMAX_DELAY)!=pdTRUE) continue;
        std::size_t sent=0;
        const auto deadline=esp_timer_get_time()+250000;
        while (sent<packet.size && esp_timer_get_time()<deadline) {
            const int bytes=usb_serial_jtag_write_bytes(packet.data+sent,packet.size-sent,pdMS_TO_TICKS(20));
            if (bytes>0) sent+=bytes;
            else vTaskDelay(1);
        }
        // On host backpressure, discard this response tail. The framed decoder
        // resynchronizes; the client times out/retries. Audio never waits for USB.
        std::free(packet.data);
    }
}
void work(void*) {
    static Command command; // One worker; keep large buffers off its task stack.
    for (;;) {
        const bool received=xQueueReceive(command_queue,&command,pdMS_TO_TICKS(100))==pdTRUE;
        auto* files=file_reader.load(std::memory_order_acquire);
        const uint64_t now=esp_timer_get_time()/1000;
        if (files) files->expire(now);
        if (!received) continue;
        Packet response{0,static_cast<uint8_t*>(std::malloc(TransferFrame::Header+JsonCapacity+4096))};
        if (!response.data) continue;
        auto* raw=response.data+TransferFrame::Header+JsonCapacity;
        std::size_t raw_size=0;
        const auto size=respond(command.json,command.size,static_cast<uint32_t>(now),
            heap_caps_get_free_size(MALLOC_CAP_8BIT),
            reinterpret_cast<char*>(response.data+TransferFrame::Header),JsonCapacity,
            files,raw,4096,&raw_size,command.raw_size,now,command.raw);
        if (!size) { std::free(response.data); continue; }
        std::memmove(response.data+TransferFrame::Header+size,raw,raw_size);
        TransferFrame::header(response.data,size,raw_size);
        response.size=TransferFrame::Header+size+raw_size;
        enqueue(response);
    }
}
void receive(void*) {
    uint8_t input[256]; static Command command; // One RX owner.
    int64_t last_rx=0;
    for (;;) {
        const int count=usb_serial_jtag_read_bytes(input,sizeof(input),pdMS_TO_TICKS(20));
        const auto now=esp_timer_get_time();
        if (decoder.used && now-last_rx>2000000) decoder.reset();
        if (count<=0) continue;
        last_rx=now;
        for (int i=0;i<count;++i) {
            if (!decoder.feed(input[i])) continue;
            const auto json_size=TransferFrame::u16(decoder.bytes+4);
            command.size=json_size<=sizeof(command.json)?json_size:0;
            command.raw_size=TransferFrame::u16(decoder.bytes+6);
            std::memcpy(command.json,decoder.bytes+TransferFrame::Header,command.size);
            std::memcpy(command.raw,decoder.bytes+TransferFrame::Header+json_size,command.raw_size);
            decoder.reset();
            // A saturated worker queue drops the command without executing it.
            // Host timeout/retry is required; RX never waits for SD operations.
            xQueueSend(command_queue,&command,0);
        }
    }
}
}
esp_err_t startUsb() {
    if (tx_queue) return ESP_ERR_INVALID_STATE;
    usb_serial_jtag_driver_config_t config{};
    config.tx_buffer_size=2048; config.rx_buffer_size=4096;
    const auto result=usb_serial_jtag_driver_install(&config);
    if (result!=ESP_OK) return result;
    tx_queue=xQueueCreate(8,sizeof(Packet));
    if (!tx_queue) { usb_serial_jtag_driver_uninstall(); return ESP_ERR_NO_MEM; }
    command_queue=xQueueCreate(4,sizeof(Command));
    if (!command_queue) {
        vQueueDelete(tx_queue); tx_queue=nullptr;
        usb_serial_jtag_driver_uninstall(); return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(transmit,"usb-tx",4096,nullptr,2,&tx_task)!=pdPASS ||
        xTaskCreate(work,"usb-files",8192,nullptr,2,&worker_task)!=pdPASS ||
        xTaskCreate(receive,"usb-rx",6144,nullptr,2,&rx_task)!=pdPASS) {
        if (worker_task) vTaskDelete(worker_task);
        if (tx_task) vTaskDelete(tx_task);
        vQueueDelete(command_queue); command_queue=nullptr; worker_task=nullptr;
        vQueueDelete(tx_queue); tx_queue=nullptr; tx_task=nullptr;
        usb_serial_jtag_driver_uninstall(); return ESP_ERR_NO_MEM;
    }
    esp_log_set_vprintf(logOutput);
    return ESP_OK;
}
esp_err_t attachFiles(Storage::FileReader& files) {
    if (!tx_queue) return ESP_ERR_INVALID_STATE;
    Storage::FileReader* expected=nullptr;
    return file_reader.compare_exchange_strong(expected,&files,std::memory_order_release)
        ? ESP_OK : ESP_ERR_INVALID_STATE;
}
}
