#include "wifi_service.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_wifi_default.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Connectivity {
esp_err_t WifiService::begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return ESP_ERR_INVALID_STATE;
    auto result=nvs_flash_init();
    // Never erase existing NVS automatically when initialization fails.
    if (result!=ESP_OK) return result;
    result=esp_netif_init();
    if (result!=ESP_OK) return result;
    result=esp_event_loop_create_default();
    if (result!=ESP_OK && result!=ESP_ERR_INVALID_STATE) return result;
    sta_netif_=esp_netif_create_default_wifi_sta();
    ap_netif_=esp_netif_create_default_wifi_ap();
    bool wifi_initialized=false;
    auto fail=[&](esp_err_t error) {
        if (ip_handler_) esp_event_handler_instance_unregister(IP_EVENT,ESP_EVENT_ANY_ID,ip_handler_);
        if (wifi_handler_) esp_event_handler_instance_unregister(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_handler_);
        if (timer_) esp_timer_delete(timer_);
        if (wifi_initialized) esp_wifi_deinit();
        if (sta_netif_) esp_netif_destroy_default_wifi(sta_netif_);
        if (ap_netif_) esp_netif_destroy_default_wifi(ap_netif_);
        ip_handler_=wifi_handler_=nullptr; timer_=nullptr;
        sta_netif_=ap_netif_=nullptr;
        return error;
    };
    if (!sta_netif_ || !ap_netif_) return fail(ESP_ERR_NO_MEM);
    wifi_init_config_t config=WIFI_INIT_CONFIG_DEFAULT();
    result=esp_wifi_init(&config);
    if (result!=ESP_OK) return fail(result);
    wifi_initialized=true;
    result=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result!=ESP_OK) return fail(result);
    esp_timer_create_args_t timer{};
    timer.callback=[](void* arg) {
        auto* self=static_cast<WifiService*>(arg);
        std::lock_guard<std::mutex> guard(self->mutex_);
        if (self->started_ && self->status_.sta && self->has_credentials_ &&
            self->status_.state==WifiState::RetryWait) self->connect();
    };
    timer.arg=this; timer.name="wifi-retry";
    result=esp_timer_create(&timer,&timer_);
    if (result!=ESP_OK) return fail(result);
    result=esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,event,this,&wifi_handler_);
    if (result!=ESP_OK) return fail(result);
    result=esp_event_handler_instance_register(IP_EVENT,ESP_EVENT_ANY_ID,event,this,&ip_handler_);
    if (result!=ESP_OK) return fail(result);
    uint8_t mac[6]{};
    result=esp_read_mac(mac,ESP_MAC_BT);
    if (result!=ESP_OK) return fail(result);
    std::snprintf(ap_ssid_,sizeof(ap_ssid_),"StoryBox-%02X%02X",mac[4],mac[5]);
    initialized_=true;
    return ESP_OK;
}
esp_err_t WifiService::apply(bool sta,bool ap,const std::string& ssid,const std::string& password) {
    if (ssid.size()>32 || password.size()>63 || ssid.find('\0')!=std::string::npos ||
        password.find('\0')!=std::string::npos) return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return ESP_ERR_INVALID_STATE;
    esp_timer_stop(timer_);
    if (started_) {
        const auto stopped=esp_wifi_stop();
        if (stopped!=ESP_OK) return stopped;
        started_=false;
    }
    status_={}; status_.sta=sta; status_.ap=ap;
    has_credentials_=!ssid.empty();
    if (!sta && !ap) return ESP_OK;
    auto fail=[&](esp_err_t result) { status_.state=WifiState::Failed; status_.error=result; return result; };
    auto result=esp_wifi_set_mode(sta?(ap?WIFI_MODE_APSTA:WIFI_MODE_STA):WIFI_MODE_AP);
    if (result!=ESP_OK) return fail(result);
    if (sta && has_credentials_) {
        wifi_config_t config{};
        std::memcpy(config.sta.ssid,ssid.data(),ssid.size());
        std::memcpy(config.sta.password,password.data(),password.size());
        result=esp_wifi_set_config(WIFI_IF_STA,&config);
        if (result!=ESP_OK) return fail(result);
    }
    if (ap) {
        wifi_config_t config{};
        std::strcpy(reinterpret_cast<char*>(config.ap.ssid),ap_ssid_);
        config.ap.ssid_len=std::strlen(ap_ssid_);
        // Preserve the legacy local AP credential; cloud authentication is separate.
        std::strcpy(reinterpret_cast<char*>(config.ap.password),"storybox123");
        config.ap.authmode=WIFI_AUTH_WPA2_PSK;
        config.ap.max_connection=4;
        result=esp_wifi_set_config(WIFI_IF_AP,&config);
        if (result!=ESP_OK) return fail(result);
    }
    result=esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (result!=ESP_OK) return fail(result);
    result=esp_wifi_start();
    if (result!=ESP_OK) return fail(result);
    started_=true; status_.state=WifiState::Idle;
    return ESP_OK;
}
void WifiService::connect() {
    status_.state=WifiState::Connecting;
    const auto result=esp_wifi_connect();
    if (result!=ESP_OK) retry(result);
}
void WifiService::retry(esp_err_t error) {
    status_.error=error;
    if (++status_.retries>6) { status_.state=WifiState::Failed; return; }
    status_.state=WifiState::RetryWait;
    esp_timer_stop(timer_);
    const uint64_t delay=std::min(30u,1u<<(status_.retries-1));
    const auto result=esp_timer_start_once(timer_,delay*1000000);
    if (result!=ESP_OK) { status_.state=WifiState::Failed; status_.error=result; }
}
void WifiService::event(void* arg,esp_event_base_t base,int32_t id,void*) {
    static_cast<WifiService*>(arg)->handle(base,id);
}
void WifiService::handle(esp_event_base_t base,int32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || !status_.sta || !has_credentials_) return;
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_START) connect();
    else if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) retry(ESP_ERR_WIFI_NOT_CONNECT);
    else if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        esp_timer_stop(timer_);
        status_.state=WifiState::Online; status_.retries=0; status_.error=ESP_OK;
    }
    else if (base==IP_EVENT && id==IP_EVENT_STA_LOST_IP) status_.state=WifiState::Connecting;
}
WifiStatus WifiService::status() { std::lock_guard<std::mutex> lock(mutex_); return status_; }
}
