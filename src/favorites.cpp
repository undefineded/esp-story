#include "favorites.h"
#include "config.h"
#include "fram.h"
#include <cstring>

namespace {

/* FRAM 布局: 0x0000 设置 / 0x0010 音量 / 0x0020 播放位置 / 0x0300 家长策略
   0x0400 起空闲, 收藏放这里 (47 字节) */
const uint16_t FRAM_ADDR = 0x0400;
const char     MAGIC[3]  = { 'F', 'A', 'V' };
const uint8_t  VERSION   = 1;
const int      BITS      = MAX_TRACKS;          // 300
const int      BYTES     = (BITS + 7) / 8;      // 38

struct Store {
  char    magic[3];
  uint8_t version;
  uint8_t bits[BYTES];
  uint8_t checksum;
};

Store s_s;

uint8_t checksum(const uint8_t* d, size_t n) {
  uint8_t s = 0;
  for (size_t i = 0; i < n; i++) s = (uint8_t)(s * 31u + d[i]);
  return s;
}

bool valid() {
  return memcmp(s_s.magic, MAGIC, sizeof(MAGIC)) == 0 &&
         s_s.version == VERSION &&
         s_s.checksum == checksum((const uint8_t*)&s_s, sizeof(s_s) - 1);
}

void save() {
  s_s.version  = VERSION;
  s_s.checksum = checksum((const uint8_t*)&s_s, sizeof(s_s) - 1);
  Fram::write(FRAM_ADDR, (const uint8_t*)&s_s, sizeof(s_s));
}

} // namespace

namespace Favorites {

void begin() {
  Fram::read(FRAM_ADDR, (uint8_t*)&s_s, sizeof(s_s));
  if (!valid()) {
    memset(&s_s, 0, sizeof(s_s));
    memcpy(s_s.magic, MAGIC, sizeof(MAGIC));
    save();
  }
  Serial.printf("[FAV] %d favorite(s)\n", count());
}

bool isFav(int allIndex) {
  if (allIndex < 0 || allIndex >= BITS) return false;
  return (s_s.bits[allIndex >> 3] & (uint8_t)(1u << (allIndex & 7))) != 0;
}

void setFav(int allIndex, bool on) {
  if (allIndex < 0 || allIndex >= BITS) return;
  uint8_t mask = (uint8_t)(1u << (allIndex & 7));
  uint8_t& b   = s_s.bits[allIndex >> 3];
  if (on) b |= mask; else b &= (uint8_t)~mask;
  save();
}

bool toggle(int allIndex) {
  if (allIndex < 0 || allIndex >= BITS) return false;
  bool now = !isFav(allIndex);
  setFav(allIndex, now);
  return now;
}

int count() {
  int n = 0;
  for (int i = 0; i < BITS; i++) if (isFav(i)) n++;
  return n;
}

void clear() {
  memset(&s_s, 0, sizeof(s_s));
  memcpy(s_s.magic, MAGIC, sizeof(MAGIC));
  save();
}

} // namespace Favorites
