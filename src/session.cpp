#include "session.h"
#include "config.h"
#include "fram.h"

namespace {
  const uint16_t ADDR_VOLUME = 0x0010;    // 音量块
  const uint16_t ADDR_POS    = 0x0020;    // 播放位置块

  const uint8_t VOL_MAGIC    = 'V';
  const uint8_t POS_MAGIC[4] = { 'M', 'P', 'S', '2' };   // v2: 曲目改记 uuid

  /* 位置块头: magic(4) + 分类名长度(1) */
  const size_t POS_HDR = 5;

  struct Data {
    uint8_t  volume;
    uint32_t trackId;
    char     cat[Session::SESSION_CAT_MAX];
  };

  /* 默认音量与 player.cpp 音频任务的初始值保持一致 */
  Data s_data = { 4, 0, {0} };

  /* 已落盘内容的影子副本: 内容相同就不再写 (FRAM 寿命无限, 但 I2C 写会阻塞 loop) */
  uint8_t s_savedVolume = 0xFF;           // 0xFF = 未知, 下次必定写入
  Data    s_savedPos;
  bool    s_savedPosValid = false;

  uint8_t checksum(const uint8_t* b, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]);
    return s;
  }

  /* 分类名合法性: 除空串外不允许含 '/' (与 Storage::index 的校验一致) */
  bool catOk(const char* c) { return c[0] == 0 || strchr(c, '/') == nullptr; }
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

const char* category() { return s_data.cat; }
uint32_t    trackId()  { return s_data.trackId; }

bool setPosition(const char* category, uint32_t trackId) {
  char cat[SESSION_CAT_MAX] = {0};
  if (category) strncpy(cat, category, SESSION_CAT_MAX - 1);
  if (!catOk(cat)) return false;

  if (s_savedPosValid && s_savedPos.trackId == trackId &&
      strcmp(s_savedPos.cat, cat) == 0) {
    return false;                            // 内容未变
  }

  uint8_t buf[POS_HDR + SESSION_CAT_MAX + 4 + 1];
  size_t  n = 0;
  memcpy(buf, POS_MAGIC, 4);                      n = 4;
  uint8_t catLen = (uint8_t)strlen(cat);
  buf[n++] = catLen;
  memcpy(buf + n, cat, catLen);                   n += catLen;
  buf[n++] = (uint8_t)( trackId        & 0xFF);
  buf[n++] = (uint8_t)((trackId >>  8) & 0xFF);
  buf[n++] = (uint8_t)((trackId >> 16) & 0xFF);
  buf[n++] = (uint8_t)((trackId >> 24) & 0xFF);
  buf[n] = checksum(buf, n);                      n++;

  Fram::write(ADDR_POS, buf, n);

  s_data.trackId = trackId;
  memcpy(s_data.cat, cat, SESSION_CAT_MAX);
  s_savedPos      = s_data;
  s_savedPosValid = true;
  return true;
}

void load() {
  /* 先复位成"无记忆"的默认值: 记录缺失/损坏时结果确定, 不会沿用上一次的内存值 */
  s_data.volume  = 4;                // 与 player.cpp 音频任务的初始音量一致
  s_data.trackId = 0;
  s_data.cat[0]  = 0;
  s_savedVolume   = 0xFF;            // 未知 -> 下次 setVolume 必定落盘
  s_savedPosValid = false;

  /* ---- 音量块 ---- */
  uint8_t v[3] = {0, 0, 0};
  Fram::read(ADDR_VOLUME, v, sizeof(v));
  if (v[0] == VOL_MAGIC && checksum(v, 2) == v[2] && v[1] <= VOL_MAX) {
    s_data.volume = v[1];
  }
  s_savedVolume = s_data.volume;

  /* ---- 播放位置块: 分段读, 只读实际写过的字节 ---- */
  uint8_t hdr[POS_HDR] = {0};
  Fram::read(ADDR_POS, hdr, sizeof(hdr));
  if (memcmp(hdr, POS_MAGIC, 4) != 0) return;

  uint8_t catLen = hdr[4];
  if (catLen >= SESSION_CAT_MAX) return;

  uint8_t sum = checksum(hdr, sizeof(hdr));

  char cat[SESSION_CAT_MAX] = {0};
  if (catLen) {
    uint8_t cbuf[SESSION_CAT_MAX];
    Fram::read(ADDR_POS + POS_HDR, cbuf, catLen);
    sum += checksum(cbuf, catLen);
    memcpy(cat, cbuf, catLen);
  }

  uint8_t idb[4] = {0};
  Fram::read((uint16_t)(ADDR_POS + POS_HDR + catLen), idb, sizeof(idb));
  sum += checksum(idb, sizeof(idb));

  /* 尾部校验字节必须与逐段累加结果一致 */
  uint8_t tail = Fram::readByte((uint16_t)(ADDR_POS + POS_HDR + catLen + sizeof(idb)));
  if (tail != sum) return;

  /* 语义校验: 记录可能来自旧固件或损坏, 不合法就整块丢弃 */
  if (!catOk(cat)) return;

  s_data.trackId = (uint32_t)idb[0] | ((uint32_t)idb[1] << 8) |
                   ((uint32_t)idb[2] << 16) | ((uint32_t)idb[3] << 24);
  memcpy(s_data.cat, cat, SESSION_CAT_MAX);
  s_savedPos      = s_data;
  s_savedPosValid = true;
}

#ifdef SESSION_SELFTEST
/* 临时自检: 真实 I2C FRAM 上的写入/读回/损坏恢复/去重/截断 */
void selfTest() {
  int pass = 0, fail = 0;
  auto check = [&](const char* what, bool ok) {
    Serial.printf("[TEST] %-28s %s\n", what, ok ? "PASS" : "FAIL");
    if (ok) pass++; else fail++;
  };

  /* 1. 音量往返 */
  setVolume(13);
  load();
  check("volume round-trip", volume() == 13);

  /* 2. 位置往返 (含中文分类名) */
  setPosition("1-2岁", 0xDEADBEEFu);
  load();
  check("category round-trip", strcmp(category(), "1-2岁") == 0);
  check("track uuid round-trip", trackId() == 0xDEADBEEFu);

  /* 3. 内容未变时不重写 */
  check("no rewrite when unchanged", setPosition("1-2岁", 0xDEADBEEFu) == false);

  /* 4. 校验和保护: 损坏一个字节后应整块丢弃 */
  uint16_t bad = ADDR_POS + POS_HDR + 1;
  Fram::writeByte(bad, (uint8_t)(Fram::readByte(bad) ^ 0xFF));
  load();
  check("corrupt record rejected", category()[0] == 0 && trackId() == 0);

  /* 5. 超长分类名截断后仍能往返 */
  char longCat[80];
  memset(longCat, 'A', sizeof(longCat));
  longCat[sizeof(longCat) - 1] = 0;
  setPosition(longCat, 0x11223344u);
  load();
  check("long name truncated",
        strlen(category()) == SESSION_CAT_MAX - 1 && trackId() == 0x11223344u);

  /* 6. 非法分类名拒绝写入 */
  check("bad category rejected", setPosition("a/b", 1u) == false);

  /* 7. 收尾: 清成"无记忆"状态, 避免留下自检痕迹 */
  setVolume(4);
  setPosition("", 0);
  load();
  check("cleanup", volume() == 4 && category()[0] == 0 && trackId() == 0);

  Serial.printf("[TEST] session selftest: %d passed, %d failed\n", pass, fail);
}
#endif

} // namespace Session
