/* NFC 业务层: 模块状态、寻卡、卡片识别、NDEF 写入与原始读写
 *
 * 与 Pn532 驱动的分工:
 *   Pn532  只处理 I2C 帧和 ISO14443A 指令
 *   Nfc    负责卡片建模、Type 2 / Classic 差异、写入状态机、事件队列
 *
 * 所有操作都在主循环执行, 不创建额外任务, 避免和 FRAM 争用 I2C。
 * 写入按页/块分步执行, 每次 loop 只做一小步, 因此不会长时间卡住 UI。
 */
#pragma once
#include <cstdint>
#include "pn532.h"

namespace Nfc {

enum class Kind : uint8_t {
  Unknown = 0,
  Ultralight,
  Ntag210,
  Ntag212,
  Ntag213,
  Ntag215,
  Ntag216,
  MifareClassic1K,
  MifareClassic4K,
  MifareMini,
  Iso14443A4,
};

enum class EventType : uint8_t {
  None = 0,
  Module,     // 模块插入/拔出
  Card,       // 新卡或卡片信息刷新
  Removed,    // 卡片离开
  Write,      // 写入进度/结果
};

enum class WritePhase : uint8_t {
  Start = 0,
  Writing,
  Verifying,
  Done,
  Error,
};

struct Card {
  bool     valid = false;
  uint8_t  uid[10] = {0};
  uint8_t  uidLen = 0;
  uint16_t atqa = 0;
  uint8_t  sak = 0;
  Kind     kind = Kind::Unknown;
  bool     type2 = false;          // NTAG / MIFARE Ultralight
  bool     writable = false;
  bool     formatted = false;      // 有合法 NDEF CC
  bool     versionOk = false;
  uint16_t capacity = 0;           // Type2: 用户区字节数 / Classic: 总字节数
  uint16_t userStart = 0;          // Type2 起始页 / Classic 起始块
  uint16_t userPages = 0;
  uint16_t userBytes = 0;          // 已缓存的用户数据字节数
  uint16_t userTotal = 0;          // 用户区真实大小
  uint8_t  version[8] = {0};
  uint8_t  versionLen = 0;
  uint8_t  cc[4] = {0};
  bool     ccValid = false;
  uint8_t  lock[2] = {0};
  uint8_t  user[256] = {0};        // Type2 页数据 / Classic block0
  uint8_t  block0[16] = {0};
  bool     hasBlock0 = false;
  uint32_t seenMs = 0;
};

struct Event {
  EventType type = EventType::None;
  bool      modulePresent = false;      // Module
  Card      card;                       // Card
  WritePhase phase = WritePhase::Start; // Write
  uint8_t   percent = 0;
  uint16_t  written = 0;
  bool      ok = false;
  const char* code = "";                // 稳定字符串, 可直接序列化
  char      message[64] = {0};
};

struct RawResult {
  bool        ok = false;
  const char* error = "";
  uint8_t     data[256] = {0};
  uint16_t    len = 0;
  uint8_t     start = 0;
};

bool begin();
void loop();

bool modulePresent();
const char* moduleName();          // "PN532"
const char* firmwareVersion();
bool polling();
void setPolling(bool on);
bool cardPresent();
const Card* card();
const char* kindName(Kind k);      // 显示名, 如 NTAG213
const char* kindId(Kind k);        // 协议 id, 如 ntag213
const char* standardName(Kind k);

void refreshCard();                // 重新读取当前卡
bool readRaw(bool block, uint16_t start, uint16_t count,
             const uint8_t* key, bool hasKey, RawResult& out);
bool startWriteNdef(const uint8_t* ndef, uint16_t len, bool verify,
                    const char** error);
bool startWriteRaw(bool block, uint16_t start, const uint8_t* data, uint16_t len,
                   const uint8_t* key, bool hasKey, const char** error);
bool writeBusy();

bool takeEvent(Event& out);

} // namespace Nfc
