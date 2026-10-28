#include "ble.h"
#include "filexfer.h"
#include "config.h"
#include "favorites.h"
#include "parental.h"
#include "player.h"
#include "story.h"
#include "storage.h"
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string>
#include <cstring>

namespace {

const char* SERVICE_UUID = "6b7a0001-3c4d-4e5f-8a9b-0c1d2e3f4a5b";
const char* RX_UUID      = "6b7a0002-3c4d-4e5f-8a9b-0c1d2e3f4a5b";
const char* TX_UUID      = "6b7a0003-3c4d-4e5f-8a9b-0c1d2e3f4a5b";

struct Line {
  char text[520];
};

NimBLEServer*          s_server = nullptr;
NimBLECharacteristic*  s_tx     = nullptr;
QueueHandle_t          s_lineQ  = nullptr;
QueueHandle_t          s_cmdQ   = nullptr;

volatile bool s_connected    = false;
volatile bool s_authed       = false;
volatile bool s_needAdv      = false;
volatile bool s_helloPending = false;
volatile bool s_configDirty  = false;
volatile bool s_txLogged     = false;
bool          s_advEnabled   = true;
uint32_t      s_connectAt    = 0;
uint32_t      s_lastStatus   = 0;
char          s_name[24]     = "StoryBox";

std::string s_rx;
bool        s_overflow = false;

void startAdvertising() {
  if (!s_advEnabled || s_connected) return;
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->start();
}

void pushLine(const char* line) {
  if (!s_lineQ) return;
  Line item{};
  strncpy(item.text, line, sizeof(item.text) - 1);
  xQueueSend(s_lineQ, &item, 0);
}

void pushAction(Ble::Action action, int32_t value = 0) {
  if (!s_cmdQ) return;
  Ble::Command c{action, value};
  xQueueSend(s_cmdQ, &c, 0);
}

void sendDoc(JsonDocument& doc, bool quiet = false) {
  if (!s_tx) return;
  char buf[760];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  if (n == 0) return;
  /* 缓冲区不足时宁可丢包也不要发一条没有 '\n' 的半截 JSON, 否则小程序只能等超时 */
  if (n >= sizeof(buf) - 1) {
    Serial.println("[BLE] tx overflow");
    return;
  }
  buf[n++] = '\n';

  /* 一个 notify 包最多只能带 MTU-3 字节, 超长会被截断/丢弃, 小程序就永远等不到 '\n'
     而超时。这里按对端 MTU 切片发送; 小程序是按字节流拼接再找换行的, 分片不影响解析。 */
  uint16_t mtu = 23;
  if (s_server && s_server->getConnectedCount() > 0) {
    mtu = s_server->getPeerInfo(0).getMTU();
    if (mtu < 23) mtu = 23;
  }
  size_t chunk = (size_t)mtu - 3;
  size_t chunks = (n + chunk - 1) / chunk;

  if (!s_txLogged) {
    s_txLogged = true;
    Serial.printf("[BLE] tx first: %uB, mtu %u, %u chunk(s)\n",
                  (unsigned)n, (unsigned)mtu, (unsigned)chunks);
  } else if (!quiet) {
    Serial.printf("[BLE] tx %uB, mtu %u%s\n",
                  (unsigned)n, (unsigned)mtu, chunks > 1 ? " split" : "");
  }

  for (size_t off = 0; off < n; off += chunk) {
    size_t len = n - off;
    if (len > chunk) len = chunk;
    s_tx->setValue((uint8_t*)(buf + off), len);
    s_tx->notify();
    if (off + len < n) delay(6);      // 给控制器留出发上一个包的时间
  }
}

void sendError(int32_t id, const char* code) {
  JsonDocument doc;
  doc["v"] = 1;
  if (id) doc["id"] = id;
  doc["ok"] = false;
  doc["err"] = code;
  sendDoc(doc);
}

void sendOk(int32_t id) {
  JsonDocument doc;
  doc["v"] = 1;
  if (id) doc["id"] = id;
  doc["ok"] = true;
  sendDoc(doc);
}

const char* lockName(Parental::Lock l) {
  switch (l) {
    case Parental::Lock::DailyLimit: return "daily";
    case Parental::Lock::Bedtime:    return "bedtime";
    case Parental::Lock::Remote:     return "remote";
    default:                         return "";
  }
}

void buildState(JsonObject data) {
  PlayerStatus st = Player::status();
  Parental::Lock lk = Parental::lock();
  bool story = Story::active();

  data["play"]   = (st.playing && !st.paused);
  data["paused"] = st.paused;
  data["vol"]    = st.volume;
  data["volMin"] = Parental::volMin();
  data["volMax"] = Parental::volMax();
  data["mode"]   = story ? "story" : (st.hasTrack ? "music" : "idle");
  data["title"]  = story ? Story::currentTitle() : (st.hasTrack ? st.name : "");
  data["cur"]    = st.curSec;
  data["dur"]    = st.durSec;
  data["used"]   = Parental::usedTodaySec();
  data["limit"]  = (uint32_t)Parental::dailyLimitMin() * 60u;
  data["lock"]   = (uint8_t)lk;
  data["reason"] = lockName(lk);
  data["rem"]    = Parental::remainingSec();
  data["bedtime"] = Parental::bedtimeEnabled();
  data["remote"] = Parental::remoteLocked();
  data["childLock"] = Parental::childLock();
  data["bound"]  = Parental::bound();
  data["timeSynced"] = Parental::timeSynced();
  data["dayKey"] = Parental::dayKey();
}

void buildConfig(JsonObject data) {
  PlayerStatus st = Player::status();
  data["dailyLimitMin"] = Parental::dailyLimitMin();
  data["volMin"]        = Parental::volMin();
  data["volMax"]        = Parental::volMax();
  data["vol"]           = st.volume;
  data["bedtimeEnabled"] = Parental::bedtimeEnabled();
  data["bedStartMin"]   = Parental::bedtimeStartMin();
  data["bedEndMin"]     = Parental::bedtimeEndMin();
  data["allowlistEnabled"] = Parental::allowlistEnabled();
  data["remoteLocked"]  = Parental::remoteLocked();
  data["childLock"]     = Parental::childLock();
  data["bound"]         = Parental::bound();
  data["hasPin"]        = Parental::hasPin();
  data["used"]          = Parental::usedTodaySec();
  data["limit"]         = (uint32_t)Parental::dailyLimitMin() * 60u;
  data["lock"]          = (uint8_t)Parental::lock();
  data["reason"]        = lockName(Parental::lock());
}

void sendHello(int32_t id) {
  JsonDocument doc;
  doc["v"]    = 1;
  if (id) doc["id"] = id;
  doc["evt"]  = "hello";
  doc["fw"]   = "storybox-1.0";
  doc["name"] = s_name;
  doc["bound"] = Parental::bound();
  doc["hasPin"] = Parental::hasPin();
  doc["volMin"] = Parental::volMin();
  doc["volMax"] = Parental::volMax();
  if (!Parental::bound()) doc["code"] = Parental::bindCode();
  sendDoc(doc);
}

void sendStateEvent() {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["v"] = 1;
  root["evt"] = "state";
  buildState(root);
  sendDoc(doc, true);      // 每秒一条, 不刷日志
}

bool getPaging(JsonDocument& doc, int total, int& offset, int& limit) {
  offset = doc["offset"] | 0;
  limit  = doc["limit"]  | 4;
  if (offset < 0) offset = 0;
  if (offset > total) offset = total;
  if (limit < 1) limit = 1;
  if (limit > 4) limit = 4;
  return offset < total;
}

void handleGet(JsonDocument& doc, int32_t id) {
  const char* what = doc["what"] | "state";
  JsonDocument out;
  out["v"] = 1;
  if (id) out["id"] = id;
  out["ok"] = true;

  if (strcmp(what, "state") == 0) {
    JsonObject data = out["data"].to<JsonObject>();
    buildState(data);
    sendDoc(out);
    return;
  }
  if (strcmp(what, "config") == 0) {
    JsonObject data = out["data"].to<JsonObject>();
    buildConfig(data);
    sendDoc(out);
    return;
  }
  if (strcmp(what, "stories") == 0) {
    int total = Story::count();
    int offset = 0, limit = 6;
    getPaging(doc, total, offset, limit);
    JsonObject data = out["data"].to<JsonObject>();
    data["total"]  = total;
    data["offset"] = offset;
    JsonArray arr = data["items"].to<JsonArray>();
    for (int i = offset; i < total && i < offset + limit; i++) {
      JsonObject item = arr.add<JsonObject>();
      item["i"] = i;
      item["n"] = Story::title(i);
      item["a"] = Parental::storyAllowed(i);
    }
    sendDoc(out);
    return;
  }
  if (strcmp(what, "music") == 0) {
    int total = Storage::allTrackCount();
    int offset = 0, limit = 4;
    getPaging(doc, total, offset, limit);
    JsonObject data = out["data"].to<JsonObject>();
    data["total"]  = total;
    data["offset"] = offset;
    JsonArray arr = data["items"].to<JsonArray>();
    for (int i = offset; i < total && i < offset + limit; i++) {
      int ci = Storage::allTrackCategory(i);
      JsonObject item = arr.add<JsonObject>();
      item["i"] = i;
      item["n"] = Storage::allTrackName(i);
      item["c"] = Storage::catName(ci);
      item["ci"] = ci;
      item["a"] = Parental::categoryAllowed(ci);
      item["f"] = Favorites::isFav(i);
    }
    sendDoc(out);
    return;
  }
  if (strcmp(what, "cats") == 0) {
    int total = Storage::catCount() - 1;      // 不把"全部歌曲"作为可配置项
    if (total < 0) total = 0;
    int offset = 0, limit = 6;
    getPaging(doc, total, offset, limit);
    JsonObject data = out["data"].to<JsonObject>();
    data["total"]  = total;
    data["offset"] = offset;
    JsonArray arr = data["items"].to<JsonArray>();
    for (int k = offset; k < total && k < offset + limit; k++) {
      int actual = k + 1;
      JsonObject item = arr.add<JsonObject>();
      item["i"] = actual;
      item["n"] = Storage::catName(actual);
      item["c"] = Storage::catTrackCount(actual);
      item["a"] = Parental::categoryAllowed(actual);
    }
    sendDoc(out);
    return;
  }
  if (strcmp(what, "report") == 0) {
    JsonObject data = out["data"].to<JsonObject>();
    data["today"] = Parental::usedTodaySec();
    data["day"]   = Parental::dayKey();
    data["limit"] = (uint32_t)Parental::dailyLimitMin() * 60u;
    JsonArray arr = data["history"].to<JsonArray>();
    for (uint8_t i = 0; i < Parental::historyCount(); i++) {
      JsonObject item = arr.add<JsonObject>();
      item["day"]  = Parental::historyDay(i);
      item["used"] = Parental::historyUsed(i);
    }
    sendDoc(out);
    return;
  }
  sendError(id, "BAD_WHAT");
}

void handleSet(JsonDocument& doc, int32_t id) {
  bool changed = false;

  if (doc["dailyLimitMin"].is<int>()) {
    Parental::setDailyLimitMin((uint16_t)doc["dailyLimitMin"].as<int>());
    changed = true;
  }

  if (doc["volMin"].is<int>() || doc["volMax"].is<int>()) {
    int lo = doc["volMin"].is<int>() ? doc["volMin"].as<int>() : Parental::volMin();
    int hi = doc["volMax"].is<int>() ? doc["volMax"].as<int>() : Parental::volMax();
    Parental::setVolumeRange((uint8_t)lo, (uint8_t)hi);
    changed = true;
  }

  bool bed = doc["bedtimeEnabled"].is<bool>();
  bool bedTimes = doc["bedStartMin"].is<int>() || doc["bedEndMin"].is<int>();
  if (bed || bedTimes) {
    bool enabled = bed ? doc["bedtimeEnabled"].as<bool>() : Parental::bedtimeEnabled();
    int start = doc["bedStartMin"].is<int>() ? doc["bedStartMin"].as<int>() : Parental::bedtimeStartMin();
    int end   = doc["bedEndMin"].is<int>()   ? doc["bedEndMin"].as<int>()   : Parental::bedtimeEndMin();
    Parental::setBedtime(enabled, (uint16_t)start, (uint16_t)end);
    changed = true;
  }

  if (doc["allowlistEnabled"].is<bool>()) {
    Parental::setAllowlistEnabled(doc["allowlistEnabled"].as<bool>());
    changed = true;
  }

  JsonArray allowStories = doc["allowStories"].as<JsonArray>();
  if (!allowStories.isNull()) {
    uint64_t mask = 0;
    for (JsonVariant v : allowStories) {
      int idx = v.as<int>();
      if (idx >= 0 && idx < 64) mask |= (1ULL << idx);
    }
    Parental::setStoryMask(mask);
    changed = true;
  }

  JsonArray allowCats = doc["allowCats"].as<JsonArray>();
  if (!allowCats.isNull()) {
    uint64_t mask = 0;
    for (JsonVariant v : allowCats) {
      int idx = v.as<int>();
      if (idx > 0 && idx < 64) mask |= (1ULL << idx);
    }
    Parental::setCatMask(mask);
    changed = true;
  }

  if (doc["remoteLocked"].is<bool>()) {
    Parental::setRemoteLocked(doc["remoteLocked"].as<bool>());
    changed = true;
  }

  if (doc["childLock"].is<bool>()) {
    Parental::setChildLock(doc["childLock"].as<bool>());
    changed = true;
  }

  if (!changed) {
    sendError(id, "NO_FIELDS");
    return;
  }

  s_configDirty = true;
  JsonDocument out;
  out["v"] = 1;
  if (id) out["id"] = id;
  out["ok"] = true;
  JsonObject data = out["data"].to<JsonObject>();
  buildConfig(data);
  sendDoc(out);
}

void handleCtrl(JsonDocument& doc, int32_t id) {
  if (FileXfer::busy()) { sendError(id, "TRANSFER_BUSY"); return; }
  const char* action = doc["action"] | "";
  int value = doc["value"] | 0;

  if (strcmp(action, "volume") == 0) {
    pushAction(Ble::Action::Volume, Parental::clampVolume(value));
    sendOk(id);
    return;
  }

  /* 收藏/取消收藏: 故事机与小程序共用同一套全量曲目下标 */
  if (strcmp(action, "fav") == 0) {
    if (value < 0 || value >= Storage::allTrackCount()) { sendError(id, "RANGE"); return; }
    bool now = Favorites::toggle(value);
    JsonDocument out;
    out["v"] = 1;
    if (id) out["id"] = id;
    out["ok"] = true;
    out["f"]  = now;
    out["n"]  = Favorites::count();
    sendDoc(out);
    return;
  }

  if (strcmp(action, "story") == 0) {
    if (!Parental::storyAllowed(value)) { sendError(id, "NOT_ALLOWED"); return; }
    if (!Parental::canPlay()) { sendError(id, "LOCKED"); return; }
    pushAction(Ble::Action::PlayStory, value);
    sendOk(id);
    return;
  }

  if (strcmp(action, "music") == 0) {
    if (value < 0 || value >= Storage::allTrackCount()) { sendError(id, "RANGE"); return; }
    int ci = Storage::allTrackCategory(value);
    if (!Parental::categoryAllowed(ci)) { sendError(id, "NOT_ALLOWED"); return; }
    if (!Parental::canPlay()) { sendError(id, "LOCKED"); return; }
    pushAction(Ble::Action::PlayMusic, value);
    sendOk(id);
    return;
  }

  if (strcmp(action, "stop") == 0) {
    pushAction(Ble::Action::Stop);
    sendOk(id);
    return;
  }

  if (!Parental::canPlay() &&
      (strcmp(action, "play") == 0 || strcmp(action, "toggle") == 0 ||
       strcmp(action, "next") == 0 || strcmp(action, "prev") == 0)) {
    sendError(id, "LOCKED");
    return;
  }

  Ble::Action a = Ble::Action::None;
  if      (strcmp(action, "play") == 0)   a = Ble::Action::Play;
  else if (strcmp(action, "pause") == 0)  a = Ble::Action::Pause;
  else if (strcmp(action, "toggle") == 0) a = Ble::Action::Toggle;
  else if (strcmp(action, "next") == 0)   a = Ble::Action::Next;
  else if (strcmp(action, "prev") == 0)   a = Ble::Action::Prev;
  else { sendError(id, "BAD_ACTION"); return; }

  pushAction(a, value);
  sendOk(id);
}

void handlePin(JsonDocument& doc, int32_t id) {
  const char* action = doc["action"] | "";
  const char* pin = doc["pin"] | "";
  if (strcmp(action, "set") == 0) {
    if (!Parental::setPin(pin)) { sendError(id, "PIN_FORMAT"); return; }
    sendOk(id);
    return;
  }
  if (strcmp(action, "verify") == 0) {
    JsonDocument out;
    out["v"] = 1;
    if (id) out["id"] = id;
    out["ok"] = true;
    out["valid"] = Parental::verifyPin(pin);
    sendDoc(out);
    return;
  }
  sendError(id, "BAD_ACTION");
}

void processLine(const char* line) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) { sendError(0, "BAD_JSON"); return; }

  int32_t id = doc["id"] | 0;
  const char* cmd = doc["cmd"] | "";

  Serial.printf("[BLE] rx %s", cmd);
  if (strcmp(cmd, "get") == 0) Serial.printf(" what=%s", (const char*)(doc["what"] | "?"));
  if (strcmp(cmd, "ctrl") == 0) Serial.printf(" action=%s", (const char*)(doc["action"] | "?"));
  Serial.printf(" id=%d\n", (int)id);

  if (strcmp(cmd, "hello") == 0) {
    sendHello(id);
    return;
  }

  if (strcmp(cmd, "bind") == 0) {
    if (Parental::bound()) { sendError(id, "BOUND"); return; }
    const char* code = doc["code"] | "";
    char token[40] = {0};
    if (!Parental::bind(code, token, sizeof(token))) {
      sendError(id, "BIND");
      return;
    }
    s_authed = true;
    JsonDocument out;
    out["v"] = 1;
    if (id) out["id"] = id;
    out["ok"] = true;
    out["token"] = token;
    sendDoc(out);
    return;
  }

  if (strcmp(cmd, "auth") == 0) {
    const char* token = doc["token"] | "";
    bool ok = Parental::verifyToken(token);
    if (!ok && Parental::hasPin()) {
      const char* pin = doc["pin"] | "";
      ok = Parental::verifyPin(pin);
    }
    /* 已绑定但没设置家长密码: 没有可校验的秘密, 不能把换手机/清缓存的用户锁死。
       一旦设了密码, 就必须给对 token 或 pin。 */
    if (!ok && Parental::bound() && !Parental::hasPin()) ok = true;
    if (!ok) { sendError(id, "AUTH"); return; }
    s_authed = true;
    sendOk(id);
    return;
  }

  if (!s_authed) { sendError(id, "AUTH"); return; }

  if (strcmp(cmd, "sync") == 0) {
    uint32_t epoch = doc["epoch"] | 0;
    int tz = doc["tz"] | 480;
    Parental::syncTime(epoch, (int16_t)tz);
    sendOk(id);
    return;
  }

  if (strcmp(cmd, "get") == 0)     { handleGet(doc, id); return; }
  if (strcmp(cmd, "set") == 0)     { handleSet(doc, id); return; }
  if (strcmp(cmd, "ctrl") == 0)    { handleCtrl(doc, id); return; }
  if (strcmp(cmd, "pin") == 0)     { handlePin(doc, id); return; }

  if (strcmp(cmd, "reset") == 0) {
    Parental::factoryReset();
    s_authed = false;
    sendOk(id);
    return;
  }

  if (strcmp(cmd, "unbind") == 0) {
    Parental::unbind();
    s_authed = false;
    sendOk(id);
    return;
  }

  sendError(id, "BAD_CMD");
}

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server) override {
    (void)server;
    s_connected    = true;
    s_authed       = false;
    s_connectAt    = millis();
    s_helloPending = true;
    s_lastStatus   = millis();
    s_rx.clear();
    s_overflow = false;
    s_txLogged = false;
    NimBLEDevice::getAdvertising()->stop();
    Serial.println("[BLE] connected");
  }

  void onDisconnect(NimBLEServer* server) override {
    (void)server;
    s_connected    = false;
    s_authed       = false;
    s_helloPending = false;
    s_needAdv      = true;
    s_rx.clear();
    Serial.println("[BLE] disconnected");
  }
};

class RxCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* ch) override {
    NimBLEAttValue v = ch->getValue();
    const uint8_t* p = v.data();
    size_t n = v.length();
    for (size_t i = 0; i < n; i++) {
      char c = (char)p[i];
      if (c == '\n') {
        if (!s_overflow) {
          size_t len = s_rx.length();
          while (len > 0 && (s_rx[len - 1] == '\r' || s_rx[len - 1] == ' ')) len--;
          if (len > 0) {
            s_rx.resize(len);
            pushLine(s_rx.c_str());
          }
        }
        s_rx.clear();
        s_overflow = false;
      } else if (s_rx.length() < 480) {
        s_rx.push_back(c);
      } else {
        s_overflow = true;
      }
    }
  }
};

} // namespace

namespace Ble {

void begin(bool advertisingEnabled) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(s_name, sizeof(s_name), "StoryBox-%02X%02X", mac[4], mac[5]);
  s_advEnabled = advertisingEnabled;

  s_lineQ = xQueueCreate(4, sizeof(Line));
  s_cmdQ  = xQueueCreate(8, sizeof(Command));

  NimBLEDevice::init(s_name);
  NimBLEDevice::setMTU(512);

  s_server = NimBLEDevice::createServer();
  s_server->setCallbacks(new ServerCB());
  s_server->advertiseOnDisconnect(false);

  NimBLEService* service = s_server->createService(SERVICE_UUID);

  NimBLECharacteristic* rx = service->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCB());

  s_tx = service->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);

  service->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setName(s_name);
  adv->setScanResponse(true);
  adv->setMinInterval(160);
  adv->setMaxInterval(320);
  if (s_advEnabled) {
    adv->start();
    Serial.printf("[BLE] %s advertising\n", s_name);
  } else {
    Serial.println("[BLE] advertising disabled");
  }
}

void setAdvertisingEnabled(bool enabled) {
  s_advEnabled = enabled;
  if (!enabled) {
    s_needAdv = false;
    if (s_server) NimBLEDevice::getAdvertising()->stop();
    Serial.println("[BLE] advertising off");
  } else {
    if (!s_connected) s_needAdv = true;
    Serial.println("[BLE] advertising on");
  }
}

bool advertisingEnabled() { return s_advEnabled; }

void loop() {
  if (s_needAdv) {
    s_needAdv = false;
    if (s_advEnabled && !s_connected) startAdvertising();
  }

  if (s_helloPending && s_connected && millis() - s_connectAt >= 500) {
    s_helloPending = false;
    sendHello(0);
  }

  Line line;
  for (int i = 0; i < 4; i++) {
    if (!s_lineQ || xQueueReceive(s_lineQ, &line, 0) != pdTRUE) break;
    processLine(line.text);
  }

  if (s_connected && s_authed && millis() - s_lastStatus >= 1000) {
    s_lastStatus = millis();
    sendStateEvent();
  }
}

bool takeCommand(Command& out) {
  if (!s_cmdQ) return false;
  return xQueueReceive(s_cmdQ, &out, 0) == pdTRUE;
}

bool takeConfigChanged() {
  bool v = s_configDirty;
  s_configDirty = false;
  return v;
}

bool connected() { return s_connected; }

const char* deviceName() { return s_name; }

} // namespace Ble
