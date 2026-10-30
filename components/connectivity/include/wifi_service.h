#pragma once
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include <mutex>
#include <string>

namespace Connectivity {
enum class WifiState { Off, Idle, Connecting, Online, RetryWait, Failed };
struct WifiStatus {
    WifiState state=WifiState::Off;
    bool sta=false,ap=false;
    unsigned retries=0;
    esp_err_t error=ESP_OK;
};
// Application-lifetime owner, initialized before other network users. No secrets
// are logged; Wi-Fi uses RAM storage, separate from explicit settings persistence.
class WifiService {
public:
    WifiService()=default;
    WifiService(const WifiService&)=delete;
    WifiService& operator=(const WifiService&)=delete;
    esp_err_t begin();
    esp_err_t apply(bool sta,bool ap,const std::string& ssid,const std::string& password);
    WifiStatus status();
    const char* apSsid() const { return ap_ssid_; }
private:
    static void event(void* arg,esp_event_base_t base,int32_t id,void* data);
    void handle(esp_event_base_t base,int32_t id);
    void connect();
    void retry(esp_err_t error);
    std::mutex mutex_;
    WifiStatus status_;
    bool initialized_=false,started_=false,has_credentials_=false;
    char ap_ssid_[24]{};
    esp_timer_handle_t timer_=nullptr;
    esp_event_handler_instance_t wifi_handler_=nullptr,ip_handler_=nullptr;
    esp_netif_t* sta_netif_=nullptr;
    esp_netif_t* ap_netif_=nullptr;
};
}
