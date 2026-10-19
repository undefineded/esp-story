#include "session.h"
#include "config.h"
#include "fram.h"

namespace {
  const uint16_t ADDR_VOLUME = 0x0010;    // 音量块
  const uint16_t ADDR_POS    = 0x0020;    // 播放位置块

  const uint8_t VOL_MAGIC    = 'V';
  const uint8_t POS_MAGIC[4] = { 'M', 'P', 'S', '1' };

  /* 位置块头: magic(4) + 曲目下标(2) + 分类名长度(1) */
  const size_t POS_HDR = 7;

  struct Data {
    uint8_t volume;
    int16_t index;
    char    cat[Session::SESSION_CAT_MAX];
    char    path[Session::SESSION_PATH_MAX];
  };

  /* 默认音量与 player.cpp 音频任务的初始值保持一致 */
  Data s_data = { 4, -1, {0}, {0} };

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

  /* 曲目路径合法性: 空串(未播放)或以 /music/ 开头 */
  bool pathOk(const char* p) { return p[0] == 0 || strncmp(p, "/music/", 7) == 0; }
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

const char* category()   { return s_data.cat; }
const char* trackPath()  { return s_data.path; }
int         trackIndex() { return s_data.index; }

bool setPosition(const char* category, const char* trackPath, int trackIndex) {
  char cat[SESSION_CAT_MAX]   = {0};
  char path[SESSION_PATH_MAX] = {0};
  if (category)  strncpy(cat,  category,  SESSION_CAT_MAX  - 1);
  if (trackPath) strncpy(path, trackPath, SESSION_PATH_MAX - 1);
  if (!catOk(cat) || !pathOk(path)) return false;
  if (trackIndex < 0 || trackIndex >= MAX_TRACKS) trackIndex = -1;

  if (s_savedPosValid && s_savedPos.index == trackIndex &&
      strcmp(s_savedPos.cat, cat) == 0 && strcmp(s_savedPos.path, path) == 0) {
    return false;                            // 内容未变
  }

  uint8_t buf[POS_HDR + SESSION_CAT_MAX + SESSION_PATH_MAX + 2];
  size_t  n = 0;
  memcpy(buf, POS_MAGIC, 4);                      n = 4;
  buf[n++] = (uint8_t)(trackIndex & 0xFF);
  buf[n++] = (uint8_t)((trackIndex >> 8) & 0xFF);
  uint8_t catLen = (uint8_t)strlen(cat);
  buf[n++] = catLen;
  memcpy(buf + n, cat, catLen);                   n += catLen;
  uint8_t pathLen = (uint8_t)strlen(path);
  buf[n++] = pathLen;
  memcpy(buf + n, path, pathLen);                 n += pathLen;
  buf[n] = checksum(buf, n);                      n++;

  Fram::write(ADDR_POS, buf, n);

  s_data.index = (int16_t)trackIndex;
  memcpy(s_data.cat,  cat,  SESSION_CAT_MAX);
  memcpy(s_data.path, path, SESSION_PATH_MAX);
  s_savedPos      = s_data;
  s_savedPosValid = true;
  return true;
}

void load() {
  /* 先复位成"无记忆"的默认值: 记录缺失/损坏时结果确定, 不会沿用上一次的内存值 */
  s_data.volume = 4;                 // 与 player.cpp 音频任务的初始音量一致
  s_data.index  = -1;
  s_data.cat[0] = s_data.path[0] = 0;
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

  int16_t index  = (int16_t)(uint16_t)(hdr[4] | (hdr[5] << 8));
  uint8_t catLen = hdr[6];
  if (catLen >= SESSION_CAT_MAX) return;

  uint8_t sum = checksum(hdr, sizeof(hdr));

  char cat[SESSION_CAT_MAX] = {0};
  if (catLen) {
    uint8_t cbuf[SESSION_CAT_MAX];
    Fram::read(ADDR_POS + POS_HDR, cbuf, catLen);
    sum += checksum(cbuf, catLen);
    memcpy(cat, cbuf, catLen);
  }

  /* 分类名之后是"路径长度"字节 */
  uint8_t pathLen = Fram::readByte((uint16_t)(ADDR_POS + POS_HDR + catLen));
  sum += pathLen;
  if (pathLen >= SESSION_PATH_MAX) return;

  char path[SESSION_PATH_MAX] = {0};
  if (pathLen) {
    uint8_t pbuf[SESSION_PATH_MAX];
    Fram::read((uint16_t)(ADDR_POS + POS_HDR + catLen + 1), pbuf, pathLen);
    sum += checksum(pbuf, pathLen);
    memcpy(path, pbuf, pathLen);
  }

  /* 尾部校验字节必须与逐段累加结果一致 */
  uint8_t tail = Fram::readByte((uint16_t)(ADDR_POS + POS_HDR + catLen + 1 + pathLen));
  if (tail != sum) return;

  /* 语义校验: 记录可能来自旧固件或损坏, 不合法就整块丢弃 */
  if (!catOk(cat) || !pathOk(path)) return;

  s_data.index = (index >= 0 && index < MAX_TRACKS) ? index : -1;
  memcpy(s_data.cat,  cat,  SESSION_CAT_MAX);
  memcpy(s_data.path, path, SESSION_PATH_MAX);
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

  /* 2. 位置往返 (含中文分类名与曲名) */
  const char* cat  = "/music/1-2岁";                       // 故意传带前缀的名字, 应原样存
  const char* path = "/music/1-2岁/01-小星星.mp3";
  setPosition("1-2岁", path, 7);
  load();
  check("category round-trip", strcmp(category(), "1-2岁") == 0);
  check("track path round-trip", strcmp(trackPath(), path) == 0);
  check("track index round-trip", trackIndex() == 7);
  (void)cat;

  /* 3. 内容未变时不重写 */
  check("no rewrite when unchanged", setPosition("1-2岁", path, 7) == false);

  /* 4. 校验和保护: 损坏一个字节后应整块丢弃 */
  uint16_t bad = ADDR_POS + POS_HDR + 2;                   // 分类名中间
  Fram::writeByte(bad, (uint8_t)(Fram::readByte(bad) ^ 0xFF));
  load();
  check("corrupt record rejected", category()[0] == 0 && trackPath()[0] == 0);

  /* 5. 超长名字截断后仍能往返 */
  char longCat[80];
  memset(longCat, 'A', sizeof(longCat));
  longCat[sizeof(longCat) - 1] = 0;
  setPosition(longCat, "/music/x.mp3", 1);
  load();
  check("long name truncated", strlen(category()) == SESSION_CAT_MAX - 1);

  /* 6. 非法内容拒绝写入 */
  check("bad category rejected", setPosition("a/b", "/music/x.mp3", 1) == false);
  check("bad path rejected",     setPosition("ok", "/story/x.mp3", 1) == false);

  /* 7. 收尾: 清成"无记忆"状态, 避免留下自检痕迹 */
  setVolume(4);
  setPosition("", "", -1);
  load();
  check("cleanup", volume() == 4 && category()[0] == 0 && trackIndex() == -1);

  Serial.printf("[TEST] session selftest: %d passed, %d failed\n", pass, fail);
}
#endif

} // namespace Session
