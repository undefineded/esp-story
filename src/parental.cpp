#include "parental.h"
#include "config.h"
#include "fram.h"
#include <esp_system.h>
#include <cstring>
#include <cstdio>

namespace {

const uint16_t FRAM_ADDR = 0x0300;
const uint8_t  VERSION   = 1;
const char     MAGIC[4]  = { 'P', 'C', 'T', 'L' };

const uint8_t FLAG_BOUND     = 0x01;
const uint8_t FLAG_HAS_PIN   = 0x02;
const uint8_t FLAG_BEDTIME   = 0x04;
const uint8_t FLAG_ALLOWLIST = 0x08;
const uint8_t FLAG_REMOTE    = 0x10;

#pragma pack(push, 1)
struct HistoryEntry {
  uint32_t dayKey;
  uint32_t usedSec;
};

struct Persist {
  char     magic[4];
  uint8_t  version;
  uint8_t  flags;
  uint16_t dailyLimitMin;
  uint8_t  volMin;
  uint8_t  volMax;
  uint16_t bedStartMin;
  uint16_t bedEndMin;
  uint32_t usedSec;
  uint32_t dayKey;
  uint32_t lastEpoch;
  int16_t  tzOffsetMin;
  uint16_t reserved;
  uint64_t storyMask;
  uint64_t catMask;
  uint32_t pinSalt;
  uint32_t pinHash;
  uint32_t tokenHash;
  char     bindCode[8];
  uint8_t  historyCount;
  uint8_t  historyPad[3];
  HistoryEntry history[7];
  uint32_t checksum;
};
#pragma pack(pop)

Persist  s_p;
uint32_t s_rev        = 1;
uint32_t s_epochBase  = 0;
uint32_t s_millisBase = 0;
bool     s_synced     = false;
uint32_t s_fracMs     = 0;
uint32_t s_lastTick   = 0;
uint32_t s_lastSave   = 0;
bool     s_dirty      = false;

uint32_t checksum(const uint8_t* data, size_t len) {
  uint32_t s = 0;
  for (size_t i = 0; i < len; i++) s = (s * 31u) + data[i];
  return s;
}

uint32_t fnv1a(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

uint32_t fnv1aStr(const char* s) {
  return fnv1a((const uint8_t*)s, strlen(s));
}

int64_t localSeconds(uint32_t epoch, int16_t tzMin) {
  return (int64_t)epoch + (int64_t)tzMin * 60;
}

uint32_t localDay(uint32_t epoch, int16_t tzMin) {
  int64_t sec = localSeconds(epoch, tzMin);
  if (sec < 0) sec = 0;
  return (uint32_t)(sec / 86400);
}

uint16_t localMinute(uint32_t epoch, int16_t tzMin) {
  int64_t sec = localSeconds(epoch, tzMin);
  if (sec < 0) sec = 0;
  return (uint16_t)((sec % 86400) / 60);
}

bool validTime(uint32_t epoch) {
  return epoch >= 1600000000u;      // 2020-09-13, 拒绝明显错误的校时
}

void generateBindCode() {
  uint32_t n = esp_random() % 1000000u;
  snprintf(s_p.bindCode, sizeof(s_p.bindCode), "%06u", (unsigned)n);
}

void generateToken(char* out, size_t len) {
  if (len < 33) { if (len) out[0] = 0; return; }
  static const char HEXC[] = "0123456789abcdef";
  uint32_t r[4] = { esp_random(), esp_random(), esp_random(), esp_random() };
  for (int i = 0; i < 4; i++) {
    for (int b = 0; b < 8; b++) {
      out[i * 8 + b] = HEXC[(r[i] >> (28 - b * 4)) & 0x0F];
    }
  }
  out[32] = 0;
}

bool pinOk(const char* pin) {
  if (!pin) return false;
  size_t n = strlen(pin);
  if (n < 4 || n > 6) return false;
  for (size_t i = 0; i < n; i++) {
    if (pin[i] < '0' || pin[i] > '9') return false;
  }
  return true;
}

void saveNow() {
  s_p.magic[0] = MAGIC[0]; s_p.magic[1] = MAGIC[1];
  s_p.magic[2] = MAGIC[2]; s_p.magic[3] = MAGIC[3];
  s_p.version  = VERSION;
  s_p.checksum = checksum((const uint8_t*)&s_p, sizeof(s_p) - sizeof(s_p.checksum));
  Fram::write(FRAM_ADDR, (const uint8_t*)&s_p, sizeof(s_p));
  s_dirty    = false;
  s_lastSave = millis();
}

void defaults() {
  memset(&s_p, 0, sizeof(s_p));
  memcpy(s_p.magic, MAGIC, 4);
  s_p.version       = VERSION;
  s_p.flags         = 0;
  s_p.dailyLimitMin = 30;
  s_p.volMin        = 3;
  s_p.volMax        = 12;
  s_p.bedStartMin   = 20 * 60 + 30;
  s_p.bedEndMin     = 7 * 60;
  s_p.tzOffsetMin   = 480;
  s_p.storyMask     = ~0ULL;
  s_p.catMask       = ~0ULL;
  generateBindCode();
}

void rollDay(uint32_t newDay) {
  if (s_p.dayKey != 0) {
    uint8_t count = s_p.historyCount;
    if (count < 7) count++;
    for (int i = count - 1; i > 0; --i) s_p.history[i] = s_p.history[i - 1];
    s_p.history[0].dayKey = s_p.dayKey;
    s_p.history[0].usedSec = s_p.usedSec;
    s_p.historyCount = count;
  }
  s_p.dayKey  = newDay;
  s_p.usedSec = 0;
  s_fracMs    = 0;
  s_dirty     = true;
}

void checkDay() {
  uint32_t now = Parental::nowEpoch();
  if (now == 0) return;
  uint32_t dk = localDay(now, s_p.tzOffsetMin);
  if (s_p.dayKey == 0) {
    s_p.dayKey = dk;
    s_dirty = true;
  } else if (dk != s_p.dayKey) {
    rollDay(dk);
  }
}

bool inBedtime(uint16_t nowMin) {
  uint16_t start = s_p.bedStartMin;
  uint16_t end   = s_p.bedEndMin;
  if (start == end) return false;
  if (start < end) return nowMin >= start && nowMin < end;
  return nowMin >= start || nowMin < end;
}

Parental::Lock computeLock() {
  if (s_p.flags & FLAG_REMOTE) return Parental::Lock::Remote;
  if (s_p.dailyLimitMin > 0 &&
      s_p.usedSec >= (uint32_t)s_p.dailyLimitMin * 60u) {
    return Parental::Lock::DailyLimit;
  }
  if ((s_p.flags & FLAG_BEDTIME) && s_p.lastEpoch != 0) {
    uint16_t m = localMinute(Parental::nowEpoch(), s_p.tzOffsetMin);
    if (inBedtime(m)) return Parental::Lock::Bedtime;
  }
  return Parental::Lock::None;
}

} // namespace

namespace Parental {

void begin() {
  memset(&s_p, 0, sizeof(s_p));
  Fram::read(FRAM_ADDR, (uint8_t*)&s_p, sizeof(s_p));

  bool valid = memcmp(s_p.magic, MAGIC, 4) == 0 &&
               s_p.version == VERSION &&
               s_p.checksum == checksum((const uint8_t*)&s_p, sizeof(s_p) - sizeof(s_p.checksum));

  if (!valid) {
    defaults();
    saveNow();
  } else {
    if (s_p.volMin > VOL_MAX) s_p.volMin = 0;
    if (s_p.volMax > VOL_MAX) s_p.volMax = VOL_MAX;
    if (s_p.volMin > s_p.volMax) { uint8_t t = s_p.volMin; s_p.volMin = s_p.volMax; s_p.volMax = t; }
    if (s_p.dailyLimitMin != 0 && (s_p.dailyLimitMin < 5 || s_p.dailyLimitMin > 240)) s_p.dailyLimitMin = 30;
    if (s_p.bedStartMin >= 1440) s_p.bedStartMin = 20 * 60 + 30;
    if (s_p.bedEndMin >= 1440)   s_p.bedEndMin   = 7 * 60;
    if (s_p.historyCount > 7) s_p.historyCount = 7;
    if (s_p.bindCode[0] == 0) {
      generateBindCode();
      saveNow();
    }
  }

  s_epochBase  = s_p.lastEpoch;
  s_millisBase = millis();
  s_lastTick   = millis();
  s_lastSave   = millis();
  s_fracMs     = 0;
  s_synced     = false;
  s_rev        = 1;

  Serial.printf("[PCTL] daily=%umin vol=%u..%u bed=%s %02u:%02u-%02u:%02u bound=%d used=%us\n",
                (unsigned)s_p.dailyLimitMin,
                (unsigned)s_p.volMin, (unsigned)s_p.volMax,
                (s_p.flags & FLAG_BEDTIME) ? "on" : "off",
                (unsigned)(s_p.bedStartMin / 60), (unsigned)(s_p.bedStartMin % 60),
                (unsigned)(s_p.bedEndMin / 60), (unsigned)(s_p.bedEndMin % 60),
                (s_p.flags & FLAG_BOUND) ? 1 : 0,
                (unsigned)s_p.usedSec);
}

uint16_t dailyLimitMin() { return s_p.dailyLimitMin; }
uint8_t  volMin()        { return s_p.volMin; }
uint8_t  volMax()        { return s_p.volMax; }
bool     bedtimeEnabled(){ return (s_p.flags & FLAG_BEDTIME) != 0; }
uint16_t bedtimeStartMin(){ return s_p.bedStartMin; }
uint16_t bedtimeEndMin()  { return s_p.bedEndMin; }
bool     allowlistEnabled(){ return (s_p.flags & FLAG_ALLOWLIST) != 0; }
bool     remoteLocked()   { return (s_p.flags & FLAG_REMOTE) != 0; }
uint64_t storyMask()      { return s_p.storyMask; }
uint64_t catMask()        { return s_p.catMask; }
uint32_t revision()       { return s_rev; }

void setDailyLimitMin(uint16_t minutes) {
  if (minutes != 0 && minutes < 5) minutes = 5;
  if (minutes > 240) minutes = 240;
  s_p.dailyLimitMin = minutes;
  s_rev++;
  saveNow();
}

void setVolumeRange(uint8_t lo, uint8_t hi) {
  if (lo > VOL_MAX) lo = VOL_MAX;
  if (hi > VOL_MAX) hi = VOL_MAX;
  if (lo > hi) { uint8_t t = lo; lo = hi; hi = t; }
  s_p.volMin = lo;
  s_p.volMax = hi;
  s_rev++;
  saveNow();
}

void setBedtime(bool enabled, uint16_t startMin, uint16_t endMin) {
  if (startMin >= 1440) startMin = 0;
  if (endMin >= 1440) endMin = 0;
  s_p.bedStartMin = startMin;
  s_p.bedEndMin   = endMin;
  if (enabled) s_p.flags |= FLAG_BEDTIME; else s_p.flags &= ~FLAG_BEDTIME;
  s_rev++;
  saveNow();
}

void setAllowlistEnabled(bool enabled) {
  if (enabled) s_p.flags |= FLAG_ALLOWLIST; else s_p.flags &= ~FLAG_ALLOWLIST;
  s_rev++;
  saveNow();
}

void setStoryMask(uint64_t mask) {
  s_p.storyMask = mask;
  s_rev++;
  saveNow();
}

void setCatMask(uint64_t mask) {
  s_p.catMask = mask;
  s_rev++;
  saveNow();
}

void setRemoteLocked(bool locked) {
  if (locked) s_p.flags |= FLAG_REMOTE; else s_p.flags &= ~FLAG_REMOTE;
  s_rev++;
  saveNow();
}

void save() { saveNow(); }

void factoryReset() {
  defaults();
  s_epochBase  = 0;
  s_millisBase = millis();
  s_synced     = false;
  s_fracMs     = 0;
  s_lastTick   = millis();
  s_rev++;
  saveNow();
  Serial.println("[PCTL] factory reset");
}

bool unbind() {
  if (!(s_p.flags & FLAG_BOUND)) return false;
  s_p.flags &= ~FLAG_BOUND;
  s_p.tokenHash = 0;
  s_rev++;
  saveNow();
  Serial.println("[PCTL] unbound");
  return true;
}

void syncTime(uint32_t epochSec, int16_t tzOffsetMin) {
  if (!validTime(epochSec)) return;
  s_epochBase  = epochSec;
  s_millisBase = millis();
  s_synced     = true;
  s_p.lastEpoch = epochSec;
  s_p.tzOffsetMin = tzOffsetMin;
  uint32_t dk = localDay(epochSec, tzOffsetMin);
  if (s_p.dayKey == 0) {
    s_p.dayKey = dk;
    s_dirty = true;
  } else if (dk != s_p.dayKey) {
    rollDay(dk);
  }
  saveNow();
  Serial.printf("[PCTL] time synced %u tz=%d day=%u\n",
                (unsigned)epochSec, (int)tzOffsetMin, (unsigned)s_p.dayKey);
}

bool timeSynced() { return s_synced; }

uint32_t nowEpoch() {
  if (s_epochBase == 0) return 0;
  return s_epochBase + (millis() - s_millisBase) / 1000u;
}

uint32_t dayKey() { return s_p.dayKey; }

uint16_t minuteOfDay() {
  uint32_t now = nowEpoch();
  if (now == 0) return 0;
  return localMinute(now, s_p.tzOffsetMin);
}

void tick(bool playing) {
  uint32_t now = millis();
  uint32_t dt  = now - s_lastTick;
  s_lastTick = now;
  if (dt > 2000) dt = 2000;

  checkDay();
  if (playing && computeLock() == Lock::None) {
    s_fracMs += dt;
    if (s_fracMs >= 1000) {
      s_p.usedSec += s_fracMs / 1000;
      s_fracMs %= 1000;
      s_dirty = true;
    }
  }

  if (s_dirty && (now - s_lastSave) >= 60000u) saveNow();
}

uint32_t usedTodaySec() { return s_p.usedSec; }

uint32_t remainingSec() {
  if (s_p.dailyLimitMin == 0) return 0;
  uint32_t limit = (uint32_t)s_p.dailyLimitMin * 60u;
  return s_p.usedSec >= limit ? 0 : limit - s_p.usedSec;
}

uint8_t historyCount() { return s_p.historyCount; }

uint32_t historyDay(uint8_t i) {
  return i < s_p.historyCount ? s_p.history[i].dayKey : 0;
}

uint32_t historyUsed(uint8_t i) {
  return i < s_p.historyCount ? s_p.history[i].usedSec : 0;
}

Lock lock() {
  checkDay();
  return computeLock();
}

bool canPlay() { return lock() == Lock::None; }

int clampVolume(int v) {
  if (v < (int)s_p.volMin) v = s_p.volMin;
  if (v > (int)s_p.volMax) v = s_p.volMax;
  return v;
}

bool storyAllowed(int index) {
  if (!(s_p.flags & FLAG_ALLOWLIST)) return true;
  if (index < 0 || index >= 64) return false;
  return (s_p.storyMask & (1ULL << index)) != 0;
}

bool categoryAllowed(int index) {
  if (!(s_p.flags & FLAG_ALLOWLIST)) return true;
  if (index <= 0 || index >= 64) return false;   // 白名单模式下不提供"全部歌曲"
  return (s_p.catMask & (1ULL << index)) != 0;
}

bool bound() { return (s_p.flags & FLAG_BOUND) != 0; }

const char* bindCode() { return s_p.bindCode; }

bool bind(const char* code, char* tokenOut, size_t tokenLen) {
  if (bound()) return false;
  if (!code || strcmp(code, s_p.bindCode) != 0) return false;

  char token[40];
  generateToken(token, sizeof(token));
  s_p.tokenHash = fnv1aStr(token);
  s_p.flags |= FLAG_BOUND;
  s_rev++;
  saveNow();

  if (tokenOut && tokenLen) {
    strncpy(tokenOut, token, tokenLen - 1);
    tokenOut[tokenLen - 1] = 0;
  }
  Serial.println("[PCTL] bound");
  return true;
}

bool verifyToken(const char* token) {
  if (!bound() || !token || !token[0]) return false;
  return s_p.tokenHash == fnv1aStr(token);
}

bool hasPin() { return (s_p.flags & FLAG_HAS_PIN) != 0; }

bool setPin(const char* pin) {
  if (!pinOk(pin)) return false;
  s_p.pinSalt = esp_random();
  uint8_t buf[4 + 8];
  memcpy(buf, &s_p.pinSalt, 4);
  size_t n = strlen(pin);
  memcpy(buf + 4, pin, n);
  s_p.pinHash = fnv1a(buf, 4 + n);
  s_p.flags |= FLAG_HAS_PIN;
  s_rev++;
  saveNow();
  return true;
}

bool verifyPin(const char* pin) {
  if (!(s_p.flags & FLAG_HAS_PIN)) return true;   // 未设置 PIN 时允许首次进入
  if (!pinOk(pin)) return false;
  uint8_t buf[4 + 8];
  memcpy(buf, &s_p.pinSalt, 4);
  size_t n = strlen(pin);
  memcpy(buf + 4, pin, n);
  return s_p.pinHash == fnv1a(buf, 4 + n);
}

} // namespace Parental
