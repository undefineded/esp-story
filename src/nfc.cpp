#include "nfc.h"
#include <cstring>

namespace {

constexpr uint32_t POLL_INTERVAL_MS  = 260;
constexpr uint32_t PROBE_INTERVAL_MS = 3500;
constexpr uint8_t  MISS_LIMIT        = 2;
constexpr uint8_t  FAIL_LIMIT        = 3;
constexpr uint16_t USER_CACHE_BYTES  = 256;
constexpr uint8_t  EVENT_QUEUE_LEN   = 8;
constexpr uint16_t NDEF_BUF_LEN      = 1024;

const uint8_t kDefaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

Nfc::Card   s_card;
Pn532::Target s_target;
bool        s_module = false;
bool        s_polling = false;
uint8_t     s_miss = 0;
uint8_t     s_fail = 0;
uint32_t    s_lastPoll = 0;
uint32_t    s_lastProbe = 0;
bool        s_forceRead = false;

Nfc::Event  s_events[EVENT_QUEUE_LEN];
uint8_t     s_evHead = 0;
uint8_t     s_evTail = 0;

struct WriteJob {
  bool     active = false;
  bool     ndef = false;
  bool     block = false;
  bool     verify = false;
  bool     needCc = false;
  bool     verifyPhase = false;
  uint16_t start = 0;
  uint16_t total = 0;
  uint16_t offset = 0;
  uint16_t verifyOffset = 0;
  uint8_t  lastPct = 0xFF;
  uint8_t  cc[4] = {0xE1, 0x10, 0x00, 0x00};
  uint8_t  key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  bool     hasKey = false;
  uint8_t  data[NDEF_BUF_LEN] = {0};
};
WriteJob s_write;

void emit(const Nfc::Event& ev) {
  uint8_t next = (uint8_t)((s_evHead + 1) % EVENT_QUEUE_LEN);
  if (next == s_evTail) {
    s_evTail = (uint8_t)((s_evTail + 1) % EVENT_QUEUE_LEN);  // 队列满时丢最旧事件
  }
  s_events[s_evHead] = ev;
  s_evHead = next;
}

void emitModule(bool present) {
  Nfc::Event ev;
  ev.type = Nfc::EventType::Module;
  ev.modulePresent = present;
  emit(ev);
}

void emitRemoved() {
  Nfc::Event ev;
  ev.type = Nfc::EventType::Removed;
  emit(ev);
}

void emitCard() {
  Nfc::Event ev;
  ev.type = Nfc::EventType::Card;
  ev.card = s_card;
  emit(ev);
}

void setModule(bool present) {
  if (s_module == present) return;
  s_module = present;
  if (!present) {
    if (s_card.valid) {
      s_card = Nfc::Card{};
      emitRemoved();
    }
    s_target = Pn532::Target{};
    emitModule(false);
  } else {
    s_fail = 0;
    s_miss = 0;
    emitModule(true);
  }
}

uint8_t ccSizeForKind(Nfc::Kind kind) {
  switch (kind) {
    case Nfc::Kind::Ultralight:
    case Nfc::Kind::Ntag210: return 0x06;
    case Nfc::Kind::Ntag212: return 0x10;
    case Nfc::Kind::Ntag213: return 0x12;
    case Nfc::Kind::Ntag215: return 0x3E;
    case Nfc::Kind::Ntag216: return 0x6D;
    default: return 0;
  }
}

Nfc::Kind kindFromVersion(uint8_t storage, Nfc::Kind fallback) {
  switch (storage) {
    case 0x06: return Nfc::Kind::Ultralight;
    case 0x07: return Nfc::Kind::Ultralight;
    case 0x0B: return Nfc::Kind::Ntag210;
    case 0x0E: return Nfc::Kind::Ntag212;
    case 0x0F: return Nfc::Kind::Ntag213;
    case 0x11: return Nfc::Kind::Ntag215;
    case 0x13: return Nfc::Kind::Ntag216;
    default:   return fallback;
  }
}

Nfc::Kind kindFromSak(uint8_t sak) {
  if (sak == 0x00) return Nfc::Kind::Ultralight;
  if (sak == 0x09) return Nfc::Kind::MifareMini;
  if (sak == 0x18) return Nfc::Kind::MifareClassic4K;
  if (sak == 0x08) return Nfc::Kind::MifareClassic1K;
  if (sak & 0x20)  return Nfc::Kind::Iso14443A4;
  return Nfc::Kind::Unknown;
}

void readType2(const Pn532::Target& t) {
  s_card.type2 = true;
  s_card.userStart = 4;

  uint8_t version[8] = {0};
  uint8_t versionLen = 0;
  if (Pn532::getVersion(version, versionLen)) {
    s_card.versionOk = true;
    s_card.versionLen = versionLen;
    memcpy(s_card.version, version, versionLen);
    s_card.kind = kindFromVersion(version[6], s_card.kind);
  }

  uint8_t page0[16] = {0};
  bool page0Ok = Pn532::readPage(0, page0);
  if (page0Ok) {
    s_card.lock[0] = page0[10];       // page 2: lock bytes
    s_card.lock[1] = page0[11];
    memcpy(s_card.cc, page0 + 12, 4);
    s_card.ccValid = (s_card.cc[0] == 0xE1 && s_card.cc[1] == 0x10);
    s_card.formatted = s_card.ccValid;
    if (!s_card.versionOk && s_card.ccValid) {
      switch (s_card.cc[2]) {
        case 0x06: s_card.kind = Nfc::Kind::Ultralight; break;
        case 0x10: s_card.kind = Nfc::Kind::Ntag212; break;
        case 0x12: s_card.kind = Nfc::Kind::Ntag213; break;
        case 0x3E: s_card.kind = Nfc::Kind::Ntag215; break;
        case 0x6D: s_card.kind = Nfc::Kind::Ntag216; break;
        default: break;
      }
    }
  }

  Pn532::Type2Info info = Pn532::resolveType2(s_card.versionOk ? s_card.version[6] : 0);
  if (info.valid) {
    s_card.capacity  = info.bytes;
    s_card.userPages = info.pages;
    s_card.userTotal = (uint16_t)(info.pages * 4);
  } else if (s_card.ccValid && s_card.cc[2] > 0) {
    s_card.capacity  = (uint16_t)s_card.cc[2] * 8;
    s_card.userPages = (uint16_t)(s_card.capacity / 4);
    s_card.userTotal = s_card.capacity;
  } else {
    /* 拿不到版本号时按最小的 Ultralight 处理, 宁可少写也不越界 */
    s_card.capacity  = 48;
    s_card.userPages = 12;
    s_card.userTotal = 48;
    if (s_card.kind == Nfc::Kind::Unknown) s_card.kind = Nfc::Kind::Ultralight;
  }

  if (s_card.formatted) {
    uint16_t ccCapacity = (uint16_t)s_card.cc[2] * 8;
    if (ccCapacity > 0 && ccCapacity < s_card.capacity) s_card.capacity = ccCapacity;
    s_card.writable = ((s_card.cc[3] & 0x0F) != 0x0F) &&
                      ((s_card.lock[0] | s_card.lock[1]) == 0);
  } else {
    s_card.writable = ((s_card.lock[0] | s_card.lock[1]) == 0);
  }

  /* 缓存前 64 页 (256B), 足够解析绝大多数文字/网址 NDEF; 更多内容用原始读取 */
  uint16_t pagesToRead = s_card.userPages < 64 ? s_card.userPages : 64;
  uint16_t bytes = 0;
  for (uint16_t p = 0; p < pagesToRead; p += 4) {
    uint8_t chunk[16] = {0};
    if (!Pn532::readPage((uint8_t)(s_card.userStart + p), chunk)) break;
    uint16_t copy = (uint16_t)(pagesToRead - p) * 4;
    if (copy > 16) copy = 16;
    if (bytes + copy > USER_CACHE_BYTES) copy = (uint16_t)(USER_CACHE_BYTES - bytes);
    memcpy(s_card.user + bytes, chunk, copy);
    bytes = (uint16_t)(bytes + copy);
  }
  s_card.userBytes = bytes;
  (void)t;
}

void readClassic(const Pn532::Target& t) {
  s_card.type2 = false;
  s_card.userStart = 0;
  s_card.formatted = false;
  s_card.ccValid = false;
  s_card.writable = false;

  uint16_t bytes = 1024;
  if (s_card.kind == Nfc::Kind::MifareClassic4K) bytes = 4096;
  else if (s_card.kind == Nfc::Kind::MifareMini) bytes = 320;
  s_card.capacity = bytes;
  s_card.userTotal = bytes;
  s_card.userPages = (uint16_t)(bytes / 16);

  uint8_t block[16] = {0};
  if (Pn532::authenticate(0, false, kDefaultKey, t) && Pn532::readBlock(0, block)) {
    memcpy(s_card.block0, block, 16);
    memcpy(s_card.user, block, 16);
    s_card.userBytes = 16;
    s_card.hasBlock0 = true;
    s_card.writable = true;     // 至少默认密钥可写; 具体扇区仍可能不同
  }
}

void readCard(const Pn532::Target& t) {
  Nfc::Card fresh;
  fresh.valid = true;
  fresh.uidLen = t.uidLen;
  memcpy(fresh.uid, t.uid, t.uidLen);
  fresh.atqa = t.atqa;
  fresh.sak = t.sak;
  fresh.seenMs = millis();
  fresh.kind = kindFromSak(t.sak);
  s_card = fresh;

  if (s_card.sak == 0x00) {
    readType2(t);
  } else if (s_card.kind == Nfc::Kind::MifareClassic1K ||
             s_card.kind == Nfc::Kind::MifareClassic4K ||
             s_card.kind == Nfc::Kind::MifareMini) {
    readClassic(t);
  }
  s_card.seenMs = millis();
}

void pollCard() {
  Pn532::Target t;
  if (!Pn532::listPassiveTarget(t, 120)) {
    if (++s_fail >= FAIL_LIMIT) {
      s_fail = 0;
      setModule(false);
      s_lastProbe = millis();
    }
    return;
  }
  s_fail = 0;

  if (!t.found) {
    if (s_card.valid && ++s_miss >= MISS_LIMIT) {
      s_miss = 0;
      s_card = Nfc::Card{};
      s_target = Pn532::Target{};
      emitRemoved();
    }
    return;
  }

  s_miss = 0;
  s_target = t;
  bool same = s_card.valid && s_card.uidLen == t.uidLen &&
              memcmp(s_card.uid, t.uid, t.uidLen) == 0;
  if (!same || s_forceRead) {
    s_forceRead = false;
    readCard(t);
    emitCard();
  } else {
    s_card.seenMs = millis();
  }
}

void emitWrite(Nfc::WritePhase phase, uint8_t pct, uint16_t written,
               bool ok, const char* code, const char* msg) {
  Nfc::Event ev;
  ev.type = Nfc::EventType::Write;
  ev.phase = phase;
  ev.percent = pct;
  ev.written = written;
  ev.ok = ok;
  ev.code = code ? code : "";
  if (msg) {
    strncpy(ev.message, msg, sizeof(ev.message) - 1);
    ev.message[sizeof(ev.message) - 1] = 0;
  }
  emit(ev);
}

void failWrite(const char* code, const char* msg) {
  s_write.active = false;
  s_forceRead = true;
  emitWrite(Nfc::WritePhase::Error, 0, s_write.offset, false, code, msg);
}

void finishWrite() {
  uint16_t written = s_write.offset;
  s_write.active = false;
  s_forceRead = true;
  emitWrite(Nfc::WritePhase::Done, 100, written, true, "", "");
}

void writeProgress() {
  uint8_t pct = 0;
  if (s_write.total > 0) {
    pct = (uint8_t)(5 + (uint32_t)s_write.offset * 90 / s_write.total);
  }
  if (pct > 99) pct = 99;
  if (pct == s_write.lastPct) return;
  s_write.lastPct = pct;
  emitWrite(Nfc::WritePhase::Writing, pct, s_write.offset, true, "", "");
}

void stepVerify() {
  if (s_write.block) {
    uint16_t block = (uint16_t)(s_write.start + s_write.verifyOffset / 16);
    uint8_t buf[16] = {0};
    if (!Pn532::authenticate((uint8_t)block, s_write.hasKey, s_write.key, s_target) &&
        !Pn532::authenticate((uint8_t)block, false, kDefaultKey, s_target)) {
      failWrite("AUTH", "校验时认证失败");
      return;
    }
    if (!Pn532::readBlock((uint8_t)block, buf)) {
      failWrite("VERIFY_READ", "回读失败");
      return;
    }
    if (memcmp(buf, s_write.data + s_write.verifyOffset, 16) != 0) {
      failWrite("VERIFY", "回读内容与写入不一致");
      return;
    }
    s_write.verifyOffset = (uint16_t)(s_write.verifyOffset + 16);
  } else {
    uint16_t remaining = (uint16_t)(s_write.total - s_write.verifyOffset);
    uint16_t take = remaining < 16 ? remaining : 16;
    uint8_t buf[16] = {0};
    uint16_t page = (uint16_t)(s_write.start + s_write.verifyOffset / 4);
    if (!Pn532::readPage((uint8_t)page, buf)) {
      failWrite("VERIFY_READ", "回读失败");
      return;
    }
    if (memcmp(buf, s_write.data + s_write.verifyOffset, take) != 0) {
      failWrite("VERIFY", "回读内容与写入不一致");
      return;
    }
    s_write.verifyOffset = (uint16_t)(s_write.verifyOffset + take);
  }
  if (s_write.verifyOffset >= s_write.total) {
    finishWrite();
  }
}

void stepWrite() {
  if (!s_write.active) return;
  if (!s_card.valid || !s_target.found) {
    failWrite("CARD_GONE", "卡片已离开, 写入中止");
    return;
  }

  if (s_write.needCc) {
    if (!Pn532::writePage(3, s_write.cc)) {
      failWrite("CC_WRITE", "无法写入 NDEF 能力容器, 标签可能已锁定");
      return;
    }
    s_write.needCc = false;
    emitWrite(Nfc::WritePhase::Writing, 2, 0, true, "", "");
    return;
  }

  if (s_write.verifyPhase) {
    stepVerify();
    return;
  }

  if (s_write.block) {
    uint16_t block = (uint16_t)(s_write.start + s_write.offset / 16);
    if (!Pn532::authenticate((uint8_t)block, false, s_write.key, s_target)) {
      failWrite("AUTH", "扇区认证失败, 请检查密钥");
      return;
    }
    if (!Pn532::writeBlock((uint8_t)block, s_write.data + s_write.offset)) {
      failWrite("WRITE", "写入块失败");
      return;
    }
    s_write.offset = (uint16_t)(s_write.offset + 16);
  } else {
    uint16_t page = (uint16_t)(s_write.start + s_write.offset / 4);
    if (!Pn532::writePage((uint8_t)page, s_write.data + s_write.offset)) {
      failWrite("WRITE", "写入页失败, 标签可能被锁定");
      return;
    }
    s_write.offset = (uint16_t)(s_write.offset + 4);
  }

  if (s_write.offset >= s_write.total) {
    if (s_write.verify) {
      s_write.verifyPhase = true;
      s_write.verifyOffset = 0;
      emitWrite(Nfc::WritePhase::Verifying, 95, s_write.offset, true, "", "");
    } else {
      finishWrite();
    }
    return;
  }
  writeProgress();
}

bool isTrailerBlock(uint16_t block) {
  if (block < 128) return (block % 4) == 3;
  return (block % 16) == 15;
}

} // namespace

namespace Nfc {

bool begin() {
  s_evHead = s_evTail = 0;
  s_card = Card{};
  s_target = Pn532::Target{};
  s_write.active = false;
  s_module = Pn532::begin();
  s_lastProbe = millis();
  s_lastPoll = 0;
  emitModule(s_module);
  return s_module;
}

void loop() {
  uint32_t now = millis();

  if (!s_module) {
    if (now - s_lastProbe >= PROBE_INTERVAL_MS) {
      s_lastProbe = now;
      if (Pn532::probe()) setModule(true);
    }
    return;
  }

  if (s_write.active) {
    stepWrite();
    return;
  }

  if (!s_polling) {
    /* 未轮询时也偶尔探测模块, 便于上位机显示"模块被拔出" */
    if (now - s_lastProbe >= 8000) {
      s_lastProbe = now;
      if (!Pn532::probe()) {
        setModule(false);
        s_lastProbe = now;
      }
    }
    return;
  }

  if (now - s_lastPoll < POLL_INTERVAL_MS) return;
  s_lastPoll = now;
  pollCard();
}

bool modulePresent() { return s_module; }
const char* moduleName() { return "PN532"; }
const char* firmwareVersion() { return Pn532::firmwareVersion(); }
bool polling() { return s_polling; }

void setPolling(bool on) {
  s_polling = on;
  if (on) {
    s_lastPoll = 0;
    if (!s_module) s_lastProbe = 0;
  }
}

bool cardPresent() { return s_card.valid; }
const Card* card() { return s_card.valid ? &s_card : nullptr; }

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Ultralight:        return "MIFARE Ultralight";
    case Kind::Ntag210:           return "NTAG210";
    case Kind::Ntag212:           return "NTAG212";
    case Kind::Ntag213:           return "NTAG213";
    case Kind::Ntag215:           return "NTAG215";
    case Kind::Ntag216:           return "NTAG216";
    case Kind::MifareClassic1K:   return "MIFARE Classic 1K";
    case Kind::MifareClassic4K:   return "MIFARE Classic 4K";
    case Kind::MifareMini:        return "MIFARE Mini";
    case Kind::Iso14443A4:        return "ISO 14443-4 卡片";
    default:                      return "未知卡片";
  }
}

const char* kindId(Kind k) {
  switch (k) {
    case Kind::Ultralight:        return "ultralight";
    case Kind::Ntag210:           return "ntag210";
    case Kind::Ntag212:           return "ntag212";
    case Kind::Ntag213:           return "ntag213";
    case Kind::Ntag215:           return "ntag215";
    case Kind::Ntag216:           return "ntag216";
    case Kind::MifareClassic1K:   return "mifare-classic-1k";
    case Kind::MifareClassic4K:   return "mifare-classic-4k";
    case Kind::MifareMini:        return "mifare-mini";
    case Kind::Iso14443A4:        return "iso14443a-4";
    default:                      return "unknown";
  }
}

const char* standardName(Kind k) {
  switch (k) {
    case Kind::MifareClassic1K:
    case Kind::MifareClassic4K:
    case Kind::MifareMini:     return "ISO 14443A · MIFARE Classic";
    case Kind::Iso14443A4:     return "ISO 14443-4";
    case Kind::Unknown:        return "ISO 14443A";
    default:                   return "ISO 14443A · Type 2";
  }
}

void refreshCard() {
  if (!s_module) {
    if (Pn532::probe()) setModule(true);
  }
  if (s_module && s_card.valid && s_target.found) {
    readCard(s_target);
    emitCard();
  } else {
    s_forceRead = true;
    s_lastPoll = 0;
  }
}

bool readRaw(bool block, uint16_t start, uint16_t count,
             const uint8_t* key, bool hasKey, RawResult& out) {
  out = RawResult{};
  if (!s_card.valid) { out.error = "NO_CARD"; return false; }
  if (count == 0) { out.error = "RANGE"; return false; }

  const uint8_t* useKey = hasKey && key ? key : kDefaultKey;

  if (block) {
    if (s_card.type2) { out.error = "KIND"; return false; }
    uint16_t blocks = (uint16_t)(s_card.capacity / 16);
    if (start + count > blocks || count > 16) { out.error = "RANGE"; return false; }
    for (uint16_t i = 0; i < count; i++) {
      uint16_t b = (uint16_t)(start + i);
      if (!Pn532::authenticate((uint8_t)b, false, useKey, s_target)) {
        out.error = "AUTH";
        return false;
      }
      if (!Pn532::readBlock((uint8_t)b, out.data + i * 16)) {
        out.error = "READ";
        return false;
      }
    }
    out.len = (uint16_t)(count * 16);
  } else {
    if (!s_card.type2) { out.error = "KIND"; return false; }
    uint16_t maxPage = (uint16_t)(s_card.userStart + s_card.userPages);
    if (start + count > maxPage || count > 64) { out.error = "RANGE"; return false; }
    uint16_t written = 0;
    for (uint16_t i = 0; i < count; i += 4) {
      uint8_t chunk[16] = {0};
      uint16_t page = (uint16_t)(start + i);
      if (!Pn532::readPage((uint8_t)page, chunk)) { out.error = "READ"; return false; }
      uint16_t pages = (uint16_t)(count - i);
      if (pages > 4) pages = 4;
      memcpy(out.data + written, chunk, pages * 4);
      written = (uint16_t)(written + pages * 4);
    }
    out.len = written;
  }
  out.start = (uint8_t)start;
  out.ok = true;
  return true;
}

bool startWriteNdef(const uint8_t* ndef, uint16_t len, bool verify,
                    const char** error) {
  if (error) *error = "";
  if (s_write.active) { if (error) *error = "BUSY"; return false; }
  if (!ndef || len == 0) { if (error) *error = "EMPTY"; return false; }
  if (!s_card.valid) { if (error) *error = "NO_CARD"; return false; }
  if (!s_card.type2) { if (error) *error = "UNSUPPORTED"; return false; }
  if (!s_card.writable) { if (error) *error = "READ_ONLY"; return false; }
  if (s_card.userTotal == 0) { if (error) *error = "NO_CAPACITY"; return false; }
  if (len > 888) { if (error) *error = "TOO_LARGE"; return false; }

  uint32_t overhead = (len < 255) ? 3u : 4u;    // TLV 头 + 终止符
  uint32_t needed = (uint32_t)len + overhead;
  uint16_t limit = s_card.userTotal;
  if (s_card.formatted && s_card.ccValid) {
    uint16_t ccCapacity = (uint16_t)s_card.cc[2] * 8;
    if (ccCapacity > 0 && ccCapacity < limit) limit = ccCapacity;
  }
  if (needed > limit) { if (error) *error = "TOO_LARGE"; return false; }

  uint32_t total = (needed + 3u) & ~3u;
  if (total > sizeof(s_write.data)) { if (error) *error = "TOO_LARGE"; return false; }

  uint16_t p = 0;
  s_write.data[p++] = 0x03;
  if (len < 255) {
    s_write.data[p++] = (uint8_t)len;
  } else {
    s_write.data[p++] = 0xFF;
    s_write.data[p++] = (uint8_t)(len >> 8);
    s_write.data[p++] = (uint8_t)(len & 0xFF);
  }
  memcpy(s_write.data + p, ndef, len);
  p = (uint16_t)(p + len);
  s_write.data[p++] = 0xFE;
  while ((p % 4) != 0) s_write.data[p++] = 0x00;

  s_write.active = true;
  s_write.ndef = true;
  s_write.block = false;
  s_write.verify = verify;
  s_write.needCc = !s_card.formatted;
  s_write.verifyPhase = false;
  s_write.start = s_card.userStart;
  s_write.total = (uint16_t)total;
  s_write.offset = 0;
  s_write.verifyOffset = 0;
  s_write.lastPct = 0xFF;
  s_write.hasKey = false;
  uint8_t ccSize = s_card.ccValid ? s_card.cc[2] : ccSizeForKind(s_card.kind);
  if (!ccSize) {
    uint16_t v = (uint16_t)(s_card.userTotal / 8);
    ccSize = v > 0xFF ? 0xFF : (uint8_t)v;
  }
  s_write.cc[0] = 0xE1;
  s_write.cc[1] = 0x10;
  s_write.cc[2] = ccSize;
  s_write.cc[3] = 0x00;

  emitWrite(WritePhase::Start, 0, 0, true, "", "");
  s_lastPoll = millis();
  return true;
}

bool startWriteRaw(bool block, uint16_t start, const uint8_t* data, uint16_t len,
                   const uint8_t* key, bool hasKey, const char** error) {
  if (error) *error = "";
  if (s_write.active) { if (error) *error = "BUSY"; return false; }
  if (!data || len == 0 || len > 256) { if (error) *error = "SIZE"; return false; }
  if (!s_card.valid) { if (error) *error = "NO_CARD"; return false; }

  if (block) {
    if (s_card.type2) { if (error) *error = "KIND"; return false; }
    if ((len % 16) != 0) { if (error) *error = "SIZE"; return false; }
    uint16_t blocks = (uint16_t)(s_card.capacity / 16);
    uint16_t count = (uint16_t)(len / 16);
    if (start + count > blocks) { if (error) *error = "RANGE"; return false; }
    for (uint16_t i = 0; i < count; i++) {
      if (isTrailerBlock((uint16_t)(start + i))) {
        if (error) *error = "TRAILER";
        return false;
      }
    }
    s_write.hasKey = hasKey && key;
    if (s_write.hasKey) memcpy(s_write.key, key, 6);
    else memcpy(s_write.key, kDefaultKey, 6);
  } else {
    if (!s_card.type2) { if (error) *error = "KIND"; return false; }
    if ((len % 4) != 0) { if (error) *error = "SIZE"; return false; }
    uint16_t maxPage = (uint16_t)(s_card.userStart + s_card.userPages);
    if (start < s_card.userStart || start + len / 4 > maxPage) {
      if (error) *error = "RANGE";
      return false;
    }
    s_write.hasKey = false;
  }

  memcpy(s_write.data, data, len);
  s_write.active = true;
  s_write.ndef = false;
  s_write.block = block;
  s_write.verify = false;
  s_write.needCc = false;
  s_write.verifyPhase = false;
  s_write.start = start;
  s_write.total = len;
  s_write.offset = 0;
  s_write.verifyOffset = 0;
  s_write.lastPct = 0xFF;

  emitWrite(WritePhase::Start, 0, 0, true, "", "");
  s_lastPoll = millis();
  return true;
}

bool writeBusy() { return s_write.active; }

bool takeEvent(Event& out) {
  if (s_evTail == s_evHead) return false;
  out = s_events[s_evTail];
  s_evTail = (uint8_t)((s_evTail + 1) % EVENT_QUEUE_LEN);
  return true;
}

} // namespace Nfc
