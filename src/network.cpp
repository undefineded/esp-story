#include "network.h"
#include "config.h"
#include <WiFi.h>
#include <esp_mac.h>
#include <cstring>

namespace {
  bool     s_wifi = false;
  bool     s_ap   = false;
  bool     s_started = false;
  char     s_apSsid[24] = "StoryBox";
  bool     s_staWasConnected = false;
  uint32_t s_lastCheck = 0;

  void configure() {
    WiFi.persistent(false);
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(50);

    if (!s_wifi && !s_ap) {
      Serial.println("[NET] WiFi/AP off");
      return;
    }

    wifi_mode_t mode = (s_wifi && s_ap) ? WIFI_AP_STA : (s_ap ? WIFI_AP : WIFI_STA);
    WiFi.mode(mode);
    /* BLE 与 WiFi 共存时必须保留 modem sleep, 关闭会触发 ESP-IDF abort */
    WiFi.setSleep(true);

    if (s_ap) {
      bool ok = WiFi.softAP(s_apSsid, WIFI_AP_PASSWORD);
      Serial.printf("[NET] AP %s %s, IP %s\n",
                    s_apSsid, ok ? "on" : "FAILED",
                    WiFi.softAPIP().toString().c_str());
    }

    if (s_wifi) {
      if (strlen(WIFI_STA_SSID) > 0) {
        WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
        Serial.printf("[NET] WiFi STA connecting to %s\n", WIFI_STA_SSID);
      } else {
        Serial.println("[NET] WiFi STA on, but WIFI_STA_SSID is empty");
      }
    } else {
      s_staWasConnected = false;
    }
  }

  void makeApSsid() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_apSsid, sizeof(s_apSsid), "StoryBox-%02X%02X", mac[4], mac[5]);
  }
}

namespace Network {

void begin(bool wifiEnabled, bool apEnabled) {
  makeApSsid();
  s_wifi = wifiEnabled;
  s_ap = apEnabled;
  configure();
  s_started = true;
}

void apply(bool wifiEnabled, bool apEnabled) {
  wifiEnabled = wifiEnabled ? 1 : 0;
  apEnabled = apEnabled ? 1 : 0;
  if (s_started && s_wifi == wifiEnabled && s_ap == apEnabled) return;
  s_wifi = wifiEnabled;
  s_ap = apEnabled;
  configure();
  s_started = true;
}

void loop() {
  uint32_t now = millis();
  if (now - s_lastCheck < 2000) return;
  s_lastCheck = now;
  if (!s_wifi) return;

  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !s_staWasConnected) {
    Serial.printf("[NET] WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
  } else if (!connected && s_staWasConnected) {
    Serial.println("[NET] WiFi disconnected");
  }
  s_staWasConnected = connected;
}

bool wifiEnabled() { return s_wifi; }
bool apEnabled() { return s_ap; }
const char* apSsid() { return s_apSsid; }

} // namespace Network
