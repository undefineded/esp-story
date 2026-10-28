#include "hostlink.h"
#include "config.h"
#include "nfc.h"
#include "ble.h"
#include "filexfer.h"
#include "transfer_frame.h"
#include <ArduinoJson.h>
#include <cstring>

namespace {

constexpr uint16_t kNdefMax   = 1024;
constexpr uint16_t kRawMax    = 512;
constexpr uint32_t kHostIdleMs = 120000;

TransferFrame::Decoder s_decoder;
uint32_t s_rxAt = 0;
uint32_t s_lastHostMs = 0;
bool     s_booted = false;
uint8_t  s_ndefBuf[kNdefMax];
uint8_t  s_rawBuf[kRawMax];
uint8_t s_docBuf[TransferFrame::Capacity];

void hexEncode(const uint8_t* data, uint16_t len, char* out, size_t outSize) {
  static const char* digits = "0123456789ABCDEF";
  if (!out || outSize == 0) return;
  uint16_t maxChars = (uint16_t)(outSize - 1);
  uint16_t chars = (uint16_t)(len * 2);
  if (chars > maxChars) chars = (uint16_t)(maxChars & 0xFFFE);
  uint16_t o = 0;
  for (uint16_t i = 0; i < len && o + 1 < chars; i++) {
    out[o++] = digits[data[i] >> 4];
    out[o++] = digits[data[i] & 0x0F];
  }
  out[o] = 0;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* 瑙ｆ瀽鍗佸叚杩涘埗; 鍏佽绌烘牸鍜屽啋鍙峰垎闅? 杩斿洖瀛楄妭鏁? 澶辫触杩斿洖 -1 */
int hexDecode(const char* text, uint8_t* out, size_t outSize) {
  if (!text) return -1;
  size_t n = 0;
  int hi = -1;
  for (const char* p = text; *p; p++) {
    char c = *p;
    if (c == ' ' || c == ':' || c == '-') continue;
    int v = hexNibble(c);
    if (v < 0) return -1;
    if (hi < 0) {
      hi = v;
    } else {
      if (n >= outSize) return -1;
      out[n++] = (uint8_t)((hi << 4) | v);
      hi = -1;
    }
  }
  if (hi >= 0) return -1;
  return (int)n;
}

void sendDoc(JsonDocument& doc, const uint8_t* raw = nullptr, size_t rawLen = 0) {
  const size_t jsonLen = measureJson(doc);
  if (!jsonLen || jsonLen > TransferFrame::MaxJson || rawLen > TransferFrame::MaxData) return;
  serializeJson(doc, s_docBuf + TransferFrame::Header, jsonLen + 1);
  if (rawLen) memcpy(s_docBuf + TransferFrame::Header + jsonLen, raw, rawLen);
  TransferFrame::header(s_docBuf, jsonLen, rawLen);
  const size_t length = TransferFrame::Header + jsonLen + rawLen;
  size_t sent = 0;
  while (sent < length) {
    size_t n = Serial.write(s_docBuf + sent, length - sent);
    if (!n) break;
    sent += n;
  }
}

void sendError(int32_t id, const char* code, const char* msg = nullptr) {
  JsonDocument doc;
  doc["v"] = 4;
  if (id) doc["id"] = id;
  doc["ok"] = false;
  doc["err"] = code;
  if (msg) doc["msg"] = msg;
  sendDoc(doc);
}

void sendOk(int32_t id, const char* cmd = nullptr, const char* act = nullptr) {
  JsonDocument doc;
  doc["v"] = 4;
  if (id) doc["id"] = id;
  doc["ok"] = true;
  if (cmd) doc["cmd"] = cmd;
  if (act) doc["act"] = act;
  sendDoc(doc);
}

const char* phaseName(Nfc::WritePhase p) {
  switch (p) {
    case Nfc::WritePhase::Start:     return "start";
    case Nfc::WritePhase::Writing:   return "writing";
    case Nfc::WritePhase::Verifying: return "verifying";
    case Nfc::WritePhase::Done:      return "done";
    case Nfc::WritePhase::Error:     return "error";
    default:                         return "unknown";
  }
}

void buildCard(JsonObject obj, const Nfc::Card& c) {
  char uid[24];
  char atqa[8];
  char sak[8];
  char version[24];
  char cc[16];
  char lock[8];
  char user[2 * sizeof(c.user) + 1];
  char block0[40];

  hexEncode(c.uid, c.uidLen, uid, sizeof(uid));
  snprintf(atqa, sizeof(atqa), "%04X", (unsigned)c.atqa);
  snprintf(sak, sizeof(sak), "%02X", (unsigned)c.sak);
  hexEncode(c.version, c.versionOk ? c.versionLen : 0, version, sizeof(version));
  hexEncode(c.cc, c.ccValid ? 4 : 0, cc, sizeof(cc));
  hexEncode(c.lock, sizeof(c.lock), lock, sizeof(lock));
  hexEncode(c.user, c.userBytes, user, sizeof(user));
  hexEncode(c.block0, c.hasBlock0 ? 16 : 0, block0, sizeof(block0));

  obj["uid"]       = uid;
  obj["uidLen"]    = c.uidLen;
  obj["atqa"]      = atqa;
  obj["sak"]       = sak;
  obj["kind"]      = Nfc::kindId(c.kind);
  obj["type"]      = Nfc::kindName(c.kind);
  obj["std"]       = Nfc::standardName(c.kind);
  obj["type2"]     = c.type2;
  obj["writable"]  = c.writable;
  obj["formatted"] = c.formatted;
  obj["cap"]       = c.capacity;
  obj["userStart"] = c.userStart;
  obj["userPages"] = c.userPages;
  obj["userTotal"] = c.userTotal;
  obj["userBytes"] = c.userBytes;
  obj["version"]   = version;
  obj["cc"]        = cc;
  obj["lock"]      = lock;
  obj["data"]      = user;
  if (c.hasBlock0) obj["block0"] = block0;
}

void buildNfcStatus(JsonObject obj) {
  obj["present"] = Nfc::modulePresent();
  obj["chip"]    = Nfc::moduleName();
  obj["fw"]      = Nfc::firmwareVersion();
  obj["i2c"]     = "0x24";
  obj["polling"] = Nfc::polling();
  obj["busy"]    = Nfc::writeBusy();
  obj["uptime"]  = millis();
  obj["heap"]    = ESP.getFreeHeap();
  const Nfc::Card* c = Nfc::card();
  if (c && c->valid) {
    JsonObject card = obj["card"].to<JsonObject>();
    buildCard(card, *c);
  }
}

void sendHello(int32_t id) {
  JsonDocument doc;
  doc["v"]       = 4;
  if (id) doc["id"] = id;
  doc["ok"]      = true;
  doc["evt"]     = "hello";
  doc["fw"]      = "storybox-nfc-1.0";
  doc["proto"]   = 4;
  doc["board"]   = "ESP32-S3";
  doc["name"]    = Ble::deviceName();
  doc["role"]    = "storybox";
  JsonArray features = doc["features"].to<JsonArray>();
  features.add("nfc");
  features.add("nfc.ndef");
  features.add("nfc.raw");
  features.add("nfc.write-verify");
  features.add("fs");
  features.add("fs.read");
  features.add("fs.write");
  features.add("fs.sd");
  JsonObject fs = doc["fs"].to<JsonObject>();
  fs["ready"] = FileXfer::ready();
  JsonObject nfc = doc["nfc"].to<JsonObject>();
  buildNfcStatus(nfc);
  sendDoc(doc);
}

void sendStatus(int32_t id) {
  JsonDocument doc;
  doc["v"] = 4;
  if (id) doc["id"] = id;
  doc["ok"] = true;
  doc["cmd"] = "nfc";
  doc["act"] = "status";
  JsonObject data = doc["data"].to<JsonObject>();
  buildNfcStatus(data);
  sendDoc(doc);
}

void flushEvents() {
  Nfc::Event ev;
  int budget = 6;
  while (budget-- > 0 && Nfc::takeEvent(ev)) {
    JsonDocument doc;
    doc["v"] = 4;
    doc["evt"] = "nfc";
    switch (ev.type) {
      case Nfc::EventType::Module:
        doc["type"] = "module";
        doc["present"] = ev.modulePresent;
        doc["chip"] = Nfc::moduleName();
        doc["fw"] = Nfc::firmwareVersion();
        doc["i2c"] = "0x24";
        doc["polling"] = Nfc::polling();
        break;
      case Nfc::EventType::Card: {
        doc["type"] = "card";
        JsonObject card = doc["card"].to<JsonObject>();
        buildCard(card, ev.card);
        break;
      }
      case Nfc::EventType::Removed:
        doc["type"] = "removed";
        break;
      case Nfc::EventType::Write:
        doc["type"] = "write";
        doc["phase"] = phaseName(ev.phase);
        doc["pct"] = ev.percent;
        doc["written"] = ev.written;
        doc["ok"] = ev.ok;
        if (ev.code && ev.code[0]) doc["code"] = ev.code;
        if (ev.message[0]) doc["msg"] = ev.message;
        break;
      default:
        continue;
    }
    sendDoc(doc);
  }
}

void handleRead(JsonDocument& doc, int32_t id) {
  const char* kind = doc["kind"] | "page";
  int start = doc["start"] | 0;
  int count = doc["count"] | 8;
  const char* keyText = doc["key"] | "";
  if (start < 0 || start > 255) { sendError(id, "RANGE"); return; }
  if (count < 1) count = 1;
  if (count > 64) count = 64;

  uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  bool hasKey = false;
  if (keyText[0]) {
    int n = hexDecode(keyText, key, sizeof(key));
    if (n != 6) { sendError(id, "BAD_KEY"); return; }
    hasKey = true;
  }

  bool block = (strcmp(kind, "block") == 0);
  Nfc::RawResult res;
  bool ok = Nfc::readRaw(block, (uint16_t)start, (uint16_t)count, key, hasKey, res);
  if (!ok) { sendError(id, res.error ? res.error : "READ"); return; }

  char hex[2 * sizeof(res.data) + 1];
  hexEncode(res.data, res.len, hex, sizeof(hex));

  JsonDocument out;
  out["v"] = 4;
  if (id) out["id"] = id;
  out["ok"] = true;
  out["cmd"] = "nfc";
  out["act"] = "read";
  JsonObject data = out["data"].to<JsonObject>();
  data["kind"] = block ? "block" : "page";
  data["start"] = start;
  data["len"] = res.len;
  data["hex"] = hex;
  sendDoc(out);
}

void handleWriteNdef(JsonDocument& doc, int32_t id) {
  const char* ndefText = doc["ndef"] | "";
  int n = hexDecode(ndefText, s_ndefBuf, sizeof(s_ndefBuf));
  if (n <= 0) { sendError(id, "BAD_HEX"); return; }
  bool verify = doc["verify"] | true;
  const char* err = "";
  if (!Nfc::startWriteNdef(s_ndefBuf, (uint16_t)n, verify, &err)) {
    sendError(id, err && err[0] ? err : "WRITE");
    return;
  }
  JsonDocument out;
  out["v"] = 4;
  if (id) out["id"] = id;
  out["ok"] = true;
  out["cmd"] = "nfc";
  out["act"] = "write_ndef";
  out["accepted"] = true;
  out["len"] = n;
  sendDoc(out);
}

void handleWriteRaw(JsonDocument& doc, int32_t id) {
  const char* kind = doc["kind"] | "page";
  int start = doc["start"] | 0;
  const char* dataText = doc["data"] | "";
  const char* keyText = doc["key"] | "";
  if (start < 0 || start > 255) { sendError(id, "RANGE"); return; }
  int n = hexDecode(dataText, s_rawBuf, sizeof(s_rawBuf));
  if (n <= 0) { sendError(id, "BAD_HEX"); return; }

  uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  bool hasKey = false;
  if (keyText[0]) {
    int kn = hexDecode(keyText, key, sizeof(key));
    if (kn != 6) { sendError(id, "BAD_KEY"); return; }
    hasKey = true;
  }

  bool block = (strcmp(kind, "block") == 0);
  const char* err = "";
  if (!Nfc::startWriteRaw(block, (uint16_t)start, s_rawBuf, (uint16_t)n,
                          key, hasKey, &err)) {
    sendError(id, err && err[0] ? err : "WRITE");
    return;
  }
  JsonDocument out;
  out["v"] = 4;
  if (id) out["id"] = id;
  out["ok"] = true;
  out["cmd"] = "nfc";
  out["act"] = "write_raw";
  out["accepted"] = true;
  out["len"] = n;
  sendDoc(out);
}

void handleNfc(JsonDocument& doc, int32_t id) {
  const char* act = doc["act"] | "";
  if (strcmp(act, "status") == 0) { sendStatus(id); return; }

  if (strcmp(act, "poll") == 0 || strcmp(act, "start") == 0 ||
      strcmp(act, "stop") == 0) {
    bool on = (strcmp(act, "stop") == 0) ? false : (doc["on"] | true);
    Nfc::setPolling(on);
    sendStatus(id);
    return;
  }

  if (strcmp(act, "refresh") == 0) {
    Nfc::refreshCard();
    sendOk(id, "nfc", "refresh");
    return;
  }

  if (strcmp(act, "read") == 0) { handleRead(doc, id); return; }
  if (strcmp(act, "write_ndef") == 0) { handleWriteNdef(doc, id); return; }
  if (strcmp(act, "write_raw") == 0) { handleWriteRaw(doc, id); return; }

  sendError(id, "BAD_ACT");
}

void processFrame(const uint8_t* json, size_t jsonLen, const uint8_t* raw, size_t rawLen) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json, jsonLen);
  if (err) { sendError(0, "BAD_JSON"); return; }

  int32_t id = doc["id"] | 0;
  if (doc["v"].as<int>() != 4) { sendError(id, "PROTOCOL_V4_REQUIRED"); return; }
  const char* cmd = doc["cmd"] | "";

  s_lastHostMs = millis();

  if (strcmp(cmd, "sys") == 0 || strcmp(cmd, "hello") == 0) {
    const char* act = doc["act"] | (strcmp(cmd, "hello") == 0 ? "hello" : "");
    if (strcmp(act, "hello") == 0) { sendHello(id); return; }
    if (strcmp(act, "ping") == 0) {
      JsonDocument out;
      out["v"] = 4;
      if (id) out["id"] = id;
      out["ok"] = true;
      out["evt"] = "pong";
      out["uptime"] = millis();
      out["heap"] = ESP.getFreeHeap();
      sendDoc(out);
      return;
    }
    sendError(id, "BAD_ACT");
    return;
  }

  if (strcmp(cmd, "nfc") == 0) {
    if (FileXfer::busy()) { sendError(id, "TRANSFER_BUSY", "File transfer in progress"); return; }
    handleNfc(doc, id); return;
  }

  if (strcmp(cmd, "fs") == 0) {
    JsonDocument out;
    FileXfer::handle(doc, out, id, raw, rawLen);
    size_t length = 0;
    const uint8_t* data = FileXfer::outputData(length);
    sendDoc(out, data, length);
    return;
  }

  sendError(id, "BAD_CMD");
}

void readSerial() {
  if (s_decoder.used && (uint32_t)(millis() - s_rxAt) > 2000) s_decoder.reset();
  int budget = 24576;
  const uint32_t started = micros();
  while (budget-- > 0 && Serial.available() > 0) {
    if ((budget & 255) == 0 && (uint32_t)(micros() - started) >= 6000) break;
    const int value = Serial.read();
    if (value < 0) break;
    s_rxAt = millis();
    if (s_decoder.feed((uint8_t)value)) {
      const size_t json = TransferFrame::u16(s_decoder.bytes + 4);
      const size_t raw = TransferFrame::u16(s_decoder.bytes + 6);
      processFrame(s_decoder.bytes + TransferFrame::Header, json,
                   s_decoder.bytes + TransferFrame::Header + json, raw);
      s_decoder.reset();
    }
  }
}

} // namespace

namespace HostLink {

void begin() {
  s_decoder.reset();
  s_lastHostMs = millis();

  /* 鍚姩鏃朵富鍔ㄥ箍鎾竴娆?hello; 涓婁綅鏈哄鏋滃凡缁忔墦寮€涓插彛鍙洿鎺ヨ瘑鍒? 鍚﹀垯涓诲姩璇㈤棶 */
  JsonDocument doc;
  doc["v"] = 4;
  doc["evt"] = "boot";
  doc["fw"] = "storybox-nfc-1.0";
  doc["board"] = "ESP32-S3";
  doc["proto"] = 4;
  JsonArray features = doc["features"].to<JsonArray>();
  features.add("nfc");
  features.add("nfc.ndef");
  features.add("nfc.raw");
  features.add("fs");
  features.add("fs.read");
  features.add("fs.write");
  features.add("fs.sd");
  JsonObject fs = doc["fs"].to<JsonObject>();
  fs["ready"] = FileXfer::ready();
  JsonObject nfc = doc["nfc"].to<JsonObject>();
  buildNfcStatus(nfc);
  sendDoc(doc);
  s_booted = true;
}

void loop() {
  if (FileXfer::busy() && !Serial) FileXfer::abortAll();
  FileXfer::loop();
  readSerial();
  FileXfer::loop();
  if (!FileXfer::busy()) {
    Nfc::loop();
    flushEvents();
  }

  /* 涓婁綅鏈洪暱鏃堕棿鏃犺姹傛椂鍋滄杞, 閬垮厤璁惧鏃犱汉浣跨敤鏃舵寔缁崰鐢?I2C */
  if (s_booted && Nfc::polling() && (millis() - s_lastHostMs > kHostIdleMs)) {
    Nfc::setPolling(false);
    Serial.println("[NFC] host idle, polling off");
  }
}

/* 电源管理用: 上位机最近是否在通信 (传文件/读卡过程中不要去休眠) */
bool recentlyActive(uint32_t withinMs) {
  return s_lastHostMs != 0 && (uint32_t)(millis() - s_lastHostMs) < withinMs;
}

} // namespace HostLink
