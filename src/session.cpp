#include "session.h"
#include "config.h"
#include "fram.h"

namespace {
  const uint16_t ADDR_VOLUME = 0x0010;    // 音量块
  const uint16_t ADDR_POS    = 0x0020;    // 播放位置块

  const uint8_t VOL_MAGIC    = 'V';
  const uint8_t POS_MAGIC[4] = { 'M', 'P', 'S', '2' };

  struct Data {
    uint8_t volume;
    uint64_t categoryHash;
    uint64_t trackHash;
  };

  /* 默认音量与 player.cpp 音频任务的初始值保持一致 */
  Data s_data = { 4, 0, 0 };

  /* 已落盘内容的影子副本: 内容相同就不再写 (FRAM 寿命无限, 但 I2C 写会阻塞 loop) */
  uint8_t s_savedVolume = 0xFF;           // 0xFF = 未知, 下次必定写入
  Data    s_savedPos;
  bool    s_savedPosValid = false;

  uint8_t checksum(const uint8_t* b, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]);
    return s;
  }

}

namespace Session {

uint8_t volume() { return s_data.volume; }

bool setVolume(uint8_t v) {
  if (v > VOL_MAX) v = VOL_MAX;
  if (v == s_savedVolume) return false;      // 已在盘上, 不必再写

  uint8_t buf[3] = { VOL_MAGIC, v, 0 };
  buf[2] = checksum(buf, 2);
  Fram::write(ADDR_VOLUME, buf, sizeof(buf));

  s_data.volume = v;
  s_savedVolume = v;
  return true;
}

uint64_t hashPath(const char* path) {
  if (!path || !path[0]) return 0;
  uint64_t hash = UINT64_C(14695981039346656037);
  for (const uint8_t* p = (const uint8_t*)path; *p; ++p) {
    hash ^= *p;
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

uint64_t trackHash() { return s_data.trackHash; }
uint64_t categoryHash() { return s_data.categoryHash; }

bool setPosition(const char* category, const char* trackPath) {
  const uint64_t categoryHash = hashPath(category);
  const uint64_t trackHash = hashPath(trackPath);
  if (s_savedPosValid && s_savedPos.categoryHash == categoryHash &&
      s_savedPos.trackHash == trackHash) {
    return false;                            // 内容未变
  }

  uint8_t buf[21];
  memcpy(buf, POS_MAGIC, 4);
  for (uint8_t i = 0; i < 8; ++i) buf[4 + i] = (uint8_t)(categoryHash >> (i * 8));
  for (uint8_t i = 0; i < 8; ++i) buf[12 + i] = (uint8_t)(trackHash >> (i * 8));
  buf[20] = checksum(buf, 20);

  Fram::write(ADDR_POS, buf, sizeof(buf));

  s_data.categoryHash = categoryHash;
  s_data.trackHash = trackHash;
  s_savedPos      = s_data;
  s_savedPosValid = true;
  return true;
}

void load() {
  /* 先复位成"无记忆"的默认值: 记录缺失/损坏时结果确定, 不会沿用上一次的内存值 */
  s_data.volume = 4;                 // 与 player.cpp 音频任务的初始音量一致
  s_data.categoryHash = 0;
  s_data.trackHash = 0;
  s_savedVolume   = 0xFF;            // 未知 -> 下次 setVolume 必定落盘
  s_savedPosValid = false;

  /* ---- 音量块 ---- */
  uint8_t v[3] = {0, 0, 0};
  Fram::read(ADDR_VOLUME, v, sizeof(v));
  if (v[0] == VOL_MAGIC && checksum(v, 2) == v[2] && v[1] <= VOL_MAX) {
    s_data.volume = v[1];
  }
  s_savedVolume = s_data.volume;

  uint8_t buf[21] = {0};
  Fram::read(ADDR_POS, buf, sizeof(buf));
  if (memcmp(buf, POS_MAGIC, 4) != 0 || checksum(buf, 20) != buf[20]) return;

  for (uint8_t i = 0; i < 8; ++i) {
    s_data.categoryHash |= (uint64_t)buf[4 + i] << (i * 8);
    s_data.trackHash |= (uint64_t)buf[12 + i] << (i * 8);
  }
  s_savedPos      = s_data;
  s_savedPosValid = true;
}

#ifdef SESSION_SELFTEST
/* 临时自检: 真实 I2C FRAM 上的写入/读回/损坏恢复/去重/截断 */
void selfTest() {
  int pass = 0, fail = 0;
  auto check = [&](const char* what, bool ok) {
    Serial.printf("[TEST] %-28s %s\n", what, ok ? "PASS" : "FAIL");
    ok ? pass++ : fail++;
  };

  /* 1. 音量往返 */
  setVolume(13);
  load();
  check("volume round-trip", volume() == 13);

  /* 2. 位置往返 (含中文路径) */
  const char* path = "/music/1-2岁/01-小星星.mp3";
  uint64_t expectedCategoryHash = hashPath("1-2岁");
  uint64_t expectedHash = hashPath(path);
  setPosition("1-2岁", path);
  load();
  check("category hash round-trip", categoryHash() == expectedCategoryHash);
  check("track hash round-trip", trackHash() == expectedHash);

  /* 3. 内容未变时不重写 */
  check("no rewrite when unchanged", setPosition("1-2岁", path) == false);

  /* 4. 校验和保护: 损坏一个字节后应整块丢弃 */
  uint16_t bad = ADDR_POS + 15;
  Fram::writeByte(bad, (uint8_t)(Fram::readByte(bad) ^ 0xFF));
  load();
  check("corrupt record rejected", trackHash() == 0);

  /* 5. 任意长度路径均只保存固定长度哈希 */
  char longPath[300];
  memset(longPath, 'a', sizeof(longPath));
  memcpy(longPath, "/music/", 7);
  longPath[sizeof(longPath) - 5] = '.';
  longPath[sizeof(longPath) - 4] = 'm';
  longPath[sizeof(longPath) - 3] = 'p';
  longPath[sizeof(longPath) - 2] = '3';
  longPath[sizeof(longPath) - 1] = 0;
  uint64_t longHash = hashPath(longPath);
  setPosition("长分类", longPath);
  load();
  check("long path hash round-trip", trackHash() == longHash);

  /* 6. 收尾: 清成"无记忆"状态, 避免留下自检痕迹 */
  setVolume(4);
  setPosition("", "");
  load();
  check("cleanup", volume() == 4 && trackHash() == 0);

  Serial.printf("[TEST] session selftest: %d passed, %d failed\n", pass, fail);
}
#endif

} // namespace Session
