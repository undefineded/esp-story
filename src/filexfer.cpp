#include "filexfer.h"
#include "storage.h"
#include "player.h"
#include "story.h"
#include "catalog.h"
#include <SD_MMC.h>
#include <cstring>
#include <vector>
#include <algorithm>
#include "nfc.h"

namespace {

constexpr uint16_t kChunk = 4096;
constexpr uint16_t kWriteBuffer = 16384;

struct ReadXfer {
  bool active = false;
  uint32_t id = 0;
  uint64_t size = 0;
  String path;
  File file;
};

struct WriteXfer {
  bool active = false;
  uint32_t id = 0;
  uint64_t expected = 0;
  uint64_t written = 0;
  String path;
  String target;
  File file;
  uint8_t buffer[kWriteBuffer];
  uint16_t bufferLen = 0;
};

ReadXfer  s_read;
WriteXfer s_write;
uint32_t  s_seq = 0;
uint32_t  s_lastReadMs = 0;
uint32_t  s_lastWriteMs = 0;
bool s_session = false;
bool s_sessionCancelled = false;
bool s_guard = false;
uint32_t s_activity = 0;
uint64_t s_readOffset = 0;
uint32_t s_writeStarted = 0, s_diskMs = 0, s_maxDiskMs = 0;
int8_t    s_writable = -1;   // -1 未检测, 0 只读, 1 可写
uint8_t   s_io[kChunk];
size_t s_binaryLength = 0;

void okDoc(JsonDocument& out, int32_t id, const char* act) {
  out.clear();
  out["v"] = 4;
  if (id) out["id"] = id;
  out["ok"] = true;
  out["cmd"] = "fs";
  out["act"] = act;
}

void errDoc(JsonDocument& out, int32_t id, const char* act, const char* code, const char* msg = nullptr) {
  out.clear();
  out["v"] = 4;
  if (id) out["id"] = id;
  out["ok"] = false;
  out["cmd"] = "fs";
  out["act"] = act;
  out["err"] = code;
  if (msg && msg[0]) out["msg"] = msg;
}

String baseName(const String& path) {
  int k = path.lastIndexOf('/');
  return k >= 0 ? path.substring(k + 1) : path;
}

String parentPath(const String& path) {
  if (path == "/" || path.isEmpty()) return String();
  int k = path.lastIndexOf('/');
  return k <= 0 ? String("/") : path.substring(0, k);
}

bool cleanPath(const char* raw, String& out, bool allowRoot = true) {
  if (!raw || !raw[0]) return false;
  String p(raw);
  p.replace('\\', '/');
  if (!p.startsWith("/")) p = "/" + p;

  String r;
  r.reserve(p.length() + 1);
  char last = 0;
  for (unsigned int i = 0; i < p.length(); i++) {
    char c = p[i];
    if ((uint8_t)c < 0x20) return false;
    if (c == '/' && last == '/') continue;
    r += c;
    last = c;
  }
  while (r.length() > 1 && r.endsWith("/")) r.remove(r.length() - 1);
  if (r == "/") {
    if (!allowRoot) return false;
    out = r;
    return true;
  }

  int from = 1;
  while (from < (int)r.length()) {
    int next = r.indexOf('/', from);
    if (next < 0) next = r.length();
    String seg = r.substring(from, next);
    if (seg.isEmpty() || seg == "." || seg == "..") return false;
    from = next + 1;
  }
  out = r;
  return true;
}

String childPath(const String& dir, const char* name) {
  String n(name);
  if (n.startsWith("/")) return n;
  return dir == "/" ? "/" + n : dir + "/" + n;
}

/* 有些 SD 卡/读卡器带写保护, FatFS 的 open(FILE_WRITE) 仍会返回对象,
   但不会真正创建目录项; 后续 write 可能触发驱动异常甚至重启。
   第一次写操作前用一个小探针文件确认可写性, 之后缓存结果。 */
bool writable() {
  if (s_writable >= 0) return s_writable != 0;
  const char* probe = "/.storybox_write_probe";
  bool okay = false;
  File f = SD_MMC.open(probe, FILE_WRITE);
  if (f) {
    okay = SD_MMC.exists(probe);
    f.close();
    if (okay) SD_MMC.remove(probe);
  }
  s_writable = okay ? 1 : 0;
  Serial.printf("[FS ] card %s\n", okay ? "writable" : "read-only");
  return okay;
}

/* 4KB 请求先攒到 16KB 再落盘: 小块随机写在这张老卡上会非常慢,
   合并后既减少 FATFS 写入次数, 也让每个请求更快返回。 */
bool flushWriteBuffer() {
  if (!s_write.active || s_write.bufferLen == 0) return true;
  const uint32_t started = millis();
  const size_t n = s_write.file.write(s_write.buffer, s_write.bufferLen);
  const uint32_t elapsed = millis() - started;
  s_diskMs += elapsed;
  s_maxDiskMs = std::max(s_maxDiskMs, elapsed);
  if (n != s_write.bufferLen) {
    Serial.printf("[FS ] write failed at %llu/%llu\n",
                  (unsigned long long)(s_write.written - s_write.bufferLen),
                  (unsigned long long)s_write.expected);
    return false;
  }
  s_write.bufferLen = 0;
  s_lastWriteMs = millis();
  return true;
}

/* 音频任务和文件管理器共用 SD_MMC。删除/写入前必须让音频任务停下并关闭
   文件句柄, 否则 FATFS 的 remove/open 可能返回 IO 甚至卡死 SD 驱动。 */
bool prepareStorage(const char* action) {
  if (s_guard) return true;
  Player::inhibitPlayback(true);
  if (Story::active()) Story::stop();
  if (!Player::stopAndWait(4000)) {
    Player::inhibitPlayback(false);
    Serial.printf("[FS ] %s: audio stop failed\n", action);
    return false;
  }
  s_guard = true;
  s_activity = millis();
  return true;
}

bool ensureParents(const String& path) {
  String parent = parentPath(path);
  if (parent.isEmpty() || parent == "/") return true;
  int from = 1;
  while (from < (int)parent.length()) {
    int next = parent.indexOf('/', from);
    if (next < 0) next = parent.length();
    String seg = parent.substring(0, next);
    if (!seg.isEmpty() && !SD_MMC.exists(seg)) {
      if (!SD_MMC.mkdir(seg)) return false;
    }
    from = next + 1;
  }
  return true;
}

bool removeRecursive(const String& path) {
  File f = SD_MMC.open(path);
  if (!f) return false;
  const bool isDir = f.isDirectory();
  if (isDir) {
    std::vector<String> children;
    File child;
    while ((child = f.openNextFile())) {
      children.push_back(childPath(path, child.name()));
      child.close();
    }
    f.close();
    for (const String& cp : children) {
      if (!removeRecursive(cp)) return false;
      delay(1);   // 让出 CPU/USB 任务, 大目录删除时避免看门狗
    }
    if (!SD_MMC.rmdir(path)) {
      Serial.printf("[FS ] rmdir failed: %s\n", path.c_str());
      return false;
    }
    return true;
  }
  f.close();
  if (!SD_MMC.remove(path)) {
    Serial.printf("[FS ] remove failed: %s\n", path.c_str());
    return false;
  }
  return true;
}

void fillStorage(JsonObject data) {
  const uint64_t total = SD_MMC.totalBytes();
  const uint64_t used = SD_MMC.usedBytes();
  data["ready"] = Storage::ready();
  data["capacity"] = total;
  data["used"] = used;
  data["free"] = total > used ? total - used : 0;
}

void sendEntry(JsonArray arr, const String& dir, File& f) {
  String path = childPath(dir, f.name());
  JsonObject item = arr.add<JsonObject>();
  item["name"] = f.name();
  item["path"] = path;
  item["dir"] = f.isDirectory();
  item["size"] = f.isDirectory() ? 0 : (uint64_t)f.size();
  time_t t = f.getLastWrite();
  item["mtime"] = t > 0 ? (uint32_t)t : 0;
}

void listDir(JsonDocument& out, int32_t id, const String& path, uint32_t offset, uint32_t limit) {
  File dir = SD_MMC.open(path);
  if (!dir) { errDoc(out, id, "list", "NOT_FOUND"); return; }
  if (!dir.isDirectory()) {
    dir.close();
    errDoc(out, id, "list", "NOT_DIR");
    return;
  }

  uint32_t total = 0;
  uint32_t added = 0;
  okDoc(out, id, "list");
  JsonObject data = out["data"].to<JsonObject>();
  data["path"] = path;
  data["parent"] = parentPath(path);
  data["offset"] = offset;
  data["limit"] = limit;
  fillStorage(data);
  JsonArray entries = data["entries"].to<JsonArray>();

  File f;
  while ((f = dir.openNextFile())) {
    if (total >= offset && added < limit) {
      sendEntry(entries, path, f);
      added++;
    }
    total++;
    f.close();
  }
  dir.close();
  data["total"] = total;
  data["returned"] = added;
}

void handleList(JsonDocument& req, JsonDocument& out, int32_t id) {
  String path;
  if (!cleanPath(req["path"] | "/", path)) { errDoc(out, id, "list", "BAD_PATH"); return; }
  uint32_t offset = req["offset"] | 0;
  uint32_t limit = req["limit"] | 64;
  if (limit < 1) limit = 1;
  if (limit > 96) limit = 96;
  listDir(out, id, path, offset, limit);
}

void handleMkdir(JsonDocument& req, JsonDocument& out, int32_t id) {
  String path;
  if (!cleanPath(req["path"] | "", path, false)) { errDoc(out, id, "mkdir", "BAD_PATH"); return; }
  if (!prepareStorage("mkdir")) { errDoc(out, id, "mkdir", "AUDIO_BUSY"); return; }
  if (!writable()) { errDoc(out, id, "mkdir", "READ_ONLY"); return; }
  if (SD_MMC.exists(path)) { errDoc(out, id, "mkdir", "EXISTS"); return; }
  if (!ensureParents(path) || !SD_MMC.mkdir(path)) { errDoc(out, id, "mkdir", "IO"); return; }
  okDoc(out, id, "mkdir");
  out["data"]["path"] = path;
}

void handleRemove(JsonDocument& req, JsonDocument& out, int32_t id) {
  String path;
  if (!cleanPath(req["path"] | "", path, false)) { errDoc(out, id, "remove", "BAD_PATH"); return; }
  if (!prepareStorage("remove")) { errDoc(out, id, "remove", "AUDIO_BUSY"); return; }
  if (!writable()) { errDoc(out, id, "remove", "READ_ONLY"); return; }
  if (!SD_MMC.exists(path)) { errDoc(out, id, "remove", "NOT_FOUND"); return; }
  if (!removeRecursive(path)) { errDoc(out, id, "remove", "IO"); return; }
  okDoc(out, id, "remove");
  out["data"]["path"] = path;
}

void handleRename(JsonDocument& req, JsonDocument& out, int32_t id) {
  String from, to;
  if (!cleanPath(req["from"] | "", from, false) ||
      !cleanPath(req["to"] | "", to, false)) {
    errDoc(out, id, "rename", "BAD_PATH");
    return;
  }
  if (!prepareStorage("rename")) { errDoc(out, id, "rename", "AUDIO_BUSY"); return; }
  if (!writable()) { errDoc(out, id, "rename", "READ_ONLY"); return; }
  if (!SD_MMC.exists(from)) { errDoc(out, id, "rename", "NOT_FOUND"); return; }
  if (SD_MMC.exists(to)) { errDoc(out, id, "rename", "EXISTS"); return; }
  if (!ensureParents(to) || !SD_MMC.rename(from, to)) { errDoc(out, id, "rename", "IO"); return; }
  okDoc(out, id, "rename");
  out["data"]["from"] = from;
  out["data"]["to"] = to;
}

void handleReadBegin(JsonDocument& req, JsonDocument& out, int32_t id) {
  String path;
  if (!cleanPath(req["path"] | "", path, false)) { errDoc(out, id, "read_begin", "BAD_PATH"); return; }
  if (!prepareStorage("read_begin")) { errDoc(out, id, "read_begin", "AUDIO_BUSY"); return; }
  if (s_read.active) {
    s_read.file.close();
    s_read.active = false;
  }
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) { errDoc(out, id, "read_begin", "NOT_FOUND"); return; }
  if (f.isDirectory()) {
    f.close();
    errDoc(out, id, "read_begin", "IS_DIR");
    return;
  }
  s_read.file = f;
  s_read.path = path;
  s_read.size = f.size();
  s_read.id = ++s_seq;
  s_read.active = true;
  s_readOffset = 0;
  s_lastReadMs = millis();

  okDoc(out, id, "read_begin");
  JsonObject data = out["data"].to<JsonObject>();
  data["xid"] = s_read.id;
  data["name"] = baseName(path);
  data["path"] = path;
  data["size"] = s_read.size;
  data["chunk"] = kChunk;
}

void handleReadChunk(JsonDocument& req, JsonDocument& out, int32_t id) {
  uint32_t xid = req["xid"] | 0;
  uint32_t offset = req["offset"] | 0;
  if (!s_read.active || xid != s_read.id) { errDoc(out, id, "read_chunk", "BAD_XFER"); return; }
  if (offset > s_read.size) { errDoc(out, id, "read_chunk", "RANGE"); return; }
  if (s_read.file.position() != offset && !s_read.file.seek(offset, SeekSet)) { errDoc(out, id, "read_chunk", "IO"); return; }
  size_t n = s_read.file.read(s_io, kChunk);
  if (n == 0 && offset < s_read.size) { errDoc(out, id, "read_chunk", "IO"); return; }
  s_lastReadMs = millis();
  s_binaryLength = n;
  s_readOffset = std::max<uint64_t>(s_readOffset, offset + n);

  okDoc(out, id, "read_chunk");
  JsonObject data = out["data"].to<JsonObject>();
  data["xid"] = xid;
  data["offset"] = offset;
  data["len"] = n;
  data["size"] = s_read.size;
  data["eof"] = (offset + n >= s_read.size);
}

void handleReadEnd(JsonDocument& req, JsonDocument& out, int32_t id) {
  uint32_t xid = req["xid"] | 0;
  if (!s_read.active || xid != s_read.id) { errDoc(out, id, "read_end", "BAD_XFER"); return; }
  s_read.file.close();
  s_read.active = false;
  s_read.id = 0;
  s_read.size = 0;
  s_read.path = "";
  okDoc(out, id, "read_end");
}

void handleWriteBegin(JsonDocument& req, JsonDocument& out, int32_t id) {
  String path;
  if (!cleanPath(req["path"] | "", path, false)) { errDoc(out, id, "write_begin", "BAD_PATH"); return; }
  if (!prepareStorage("write_begin")) { errDoc(out, id, "write_begin", "AUDIO_BUSY"); return; }
  if (!writable()) { errDoc(out, id, "write_begin", "READ_ONLY"); return; }
  uint64_t size = req["size"] | 0;
  bool overwrite = req["overwrite"] | true;
  if (size > 0xFFFFFFFFULL) { errDoc(out, id, "write_begin", "TOO_LARGE"); return; }
  if (s_write.active) {
    String oldPath = s_write.path;
    s_write.file.close();
    s_write.active = false;
    if (oldPath.length() && oldPath != path) SD_MMC.remove(oldPath);
    s_write.id = 0;
  }
  if (SD_MMC.exists(path)) {
    File existing = SD_MMC.open(path);
    bool isDir = existing && existing.isDirectory();
    if (existing) existing.close();
    if (isDir) { errDoc(out, id, "write_begin", "IS_DIR"); return; }
    if (!overwrite) { errDoc(out, id, "write_begin", "EXISTS"); return; }
  }
  if (!ensureParents(path)) { errDoc(out, id, "write_begin", "IO"); return; }

  // Never truncate the destination before a complete upload has been received.
  String temp;
  const String parent = parentPath(path);
  do { temp = (parent == "/" ? String("") : parent) + "/.storybox-upload-" + String(++s_seq) + ".part"; }
  while (SD_MMC.exists(temp) || SD_MMC.exists(temp + ".bak"));
  File f = SD_MMC.open(temp, FILE_WRITE);
  if (!f) { errDoc(out, id, "write_begin", "NO_SPACE"); return; }

  s_write.file = f;
  s_write.path = temp;
  s_write.target = path;
  s_write.expected = size;
  s_write.written = 0;
  s_write.bufferLen = 0;
  s_write.id = ++s_seq;
  s_write.active = true;
  s_writeStarted = millis(); s_diskMs = s_maxDiskMs = 0;
  s_lastWriteMs = millis();

  okDoc(out, id, "write_begin");
  JsonObject data = out["data"].to<JsonObject>();
  data["xid"] = s_write.id;
  data["path"] = path;
  data["size"] = size;
  data["chunk"] = kChunk;
}

void handleWriteChunk(JsonDocument& req, JsonDocument& out, int32_t id, const uint8_t* raw, size_t rawLen) {
  uint32_t xid = req["xid"] | 0;
  uint32_t offset = req["offset"] | 0;
  if (!s_write.active || xid != s_write.id) { errDoc(out, id, "write_chunk", "BAD_XFER"); return; }
  if (!raw || !rawLen || rawLen > kChunk) { errDoc(out, id, "write_chunk", "BAD_LENGTH"); return; }
  const int n = rawLen;
  memcpy(s_io, raw, rawLen);
  if ((uint64_t)offset + n > s_write.expected ||
      (offset < s_write.written && (uint64_t)offset + n > s_write.written)) {
    errDoc(out, id, "write_chunk", "RANGE"); return;
  }
  if (offset > s_write.written) { errDoc(out, id, "write_chunk", "RANGE"); return; }
  if (offset < s_write.written) {
    // 上一次响应可能丢失, 上位机重发同一块; 已接收则幂等返回, 不重复写。
    s_lastWriteMs = millis();
    okDoc(out, id, "write_chunk");
    out["data"]["xid"] = xid;
    out["data"]["written"] = s_write.written;
    return;
  }
  if (n == 0) {
    okDoc(out, id, "write_chunk");
    out["data"]["xid"] = xid;
    out["data"]["written"] = s_write.written;
    return;
  }

  if ((uint32_t)s_write.bufferLen + (uint32_t)n > kWriteBuffer) {
    if (!flushWriteBuffer()) { errDoc(out, id, "write_chunk", "IO"); return; }
  }
  memcpy(s_write.buffer + s_write.bufferLen, s_io, (size_t)n);
  s_write.bufferLen += (uint16_t)n;
  s_write.written += (uint64_t)n;
  s_lastWriteMs = millis();

  // 攒满一块再落盘; 老卡对 4KB 随机写非常慢。
  if (s_write.bufferLen == kWriteBuffer && !flushWriteBuffer()) {
    errDoc(out, id, "write_chunk", "IO");
    return;
  }

  okDoc(out, id, "write_chunk");
  JsonObject data = out["data"].to<JsonObject>();
  data["xid"] = xid;
  data["written"] = s_write.written;
}

void handleWriteEnd(JsonDocument& req, JsonDocument& out, int32_t id) {
  uint32_t xid = req["xid"] | 0;
  if (!s_write.active || xid != s_write.id) { errDoc(out, id, "write_end", "BAD_XFER"); return; }
  bool flushed = flushWriteBuffer();
  s_write.file.flush();
  const uint64_t diskSize = s_write.file.size();
  s_write.file.close();
  s_write.active = false;
  uint64_t actual = s_write.written;
  uint64_t expected = s_write.expected;
  String path = s_write.path;
  String target = s_write.target;
  s_write.id = 0;
  s_write.written = 0;
  s_write.expected = 0;
  s_write.bufferLen = 0;
  s_write.path = "";

  if (!flushed || actual != expected || diskSize != expected || !SD_MMC.exists(path)) {
    SD_MMC.remove(path);
    errDoc(out, id, "write_end", "IO");
    return;
  }
  const String backup = path + ".bak";
  const bool replacing = SD_MMC.exists(target);
  if (replacing && !SD_MMC.rename(target, backup)) {
    SD_MMC.remove(path); errDoc(out, id, "write_end", "IO"); return;
  }
  if (!SD_MMC.rename(path, target)) {
    if (replacing) SD_MMC.rename(backup, target);
    SD_MMC.remove(path); errDoc(out, id, "write_end", "IO"); return;
  }
  if (replacing) SD_MMC.remove(backup);

  okDoc(out, id, "write_end");
  JsonObject data = out["data"].to<JsonObject>();
  data["path"] = target;
  data["size"] = actual;
  data["expected"] = expected;
  data["complete"] = (actual == expected);
  data["elapsedMs"] = (uint32_t)(millis() - s_writeStarted);
  data["diskMs"] = s_diskMs;
  data["maxDiskMs"] = s_maxDiskMs;
}

void handleCancel(JsonDocument& req, JsonDocument& out, int32_t id) {
  uint32_t xid = req["xid"] | 0;
  bool matched = false;
  if (s_read.active && xid == s_read.id) {
    s_read.file.close();
    s_read.active = false;
    s_read.id = 0;
    matched = true;
  }
  if (s_write.active && xid == s_write.id) {
    s_write.file.close();
    s_write.active = false;
    s_write.id = 0;
    SD_MMC.remove(s_write.path);
    s_write.path = "";
    s_write.written = 0;
    s_write.expected = 0;
    s_write.bufferLen = 0;
    matched = true;
  }
  if (!matched) { errDoc(out, id, "cancel", "BAD_XFER"); return; }
  okDoc(out, id, "cancel");
}

void handleRescan(JsonDocument& req, JsonDocument& out, int32_t id) {
  if (!prepareStorage("rescan")) { errDoc(out, id, "rescan", "AUDIO_BUSY"); return; }
  Catalog::Result result = Catalog::refresh();
  bool scanned = result != Catalog::Result::ScanFailed;
  okDoc(out, id, "rescan");
  JsonObject data = out["data"].to<JsonObject>();
  data["ok"] = scanned;
  data["saved"] = result == Catalog::Result::Saved;
  data["tracks"] = Storage::totalCount();
  data["stories"] = Story::count();
  data["categories"] = Storage::catCount();
  fillStorage(data);
}

} // namespace

namespace FileXfer {

bool ready() { return Storage::ready(); }
bool busy() { return s_guard; }
uint8_t progress() {
  if (s_write.active && s_write.expected) return (uint8_t)std::min<uint64_t>(100, s_write.written * 100 / s_write.expected);
  if (s_read.active && s_read.size) return (uint8_t)std::min<uint64_t>(100, s_readOffset * 100 / s_read.size);
  return 0;
}

void loop() {
  const uint32_t now = millis();
  if (s_guard && now - s_activity > 60000) { abortAll(); return; }
  if (s_read.active && now - s_lastReadMs > 30000) {
    s_read.file.close();
    s_read.active = false;
    s_read.id = 0;
    Serial.println("[FS ] idle read closed");
  }
  if (s_write.active && now - s_lastWriteMs > 60000) {
    String path = s_write.path;
    s_write.file.close();
    s_write.active = false;
    s_write.id = 0;
    s_write.written = 0;
    s_write.expected = 0;
    s_write.bufferLen = 0;
    s_write.path = "";
    if (path.length()) SD_MMC.remove(path);
    Serial.printf("[FS ] idle write cancelled: %s\n", path.c_str());
  }
}

void abortAll() {
  if (s_session) s_sessionCancelled = true;
  if (s_read.active) { s_read.file.close(); s_read.active = false; }
  if (s_write.active) {
    s_write.file.close(); s_write.active = false;
    SD_MMC.remove(s_write.path);
  }
  s_write.bufferLen = 0;
  s_session = s_guard = false;
  Player::inhibitPlayback(false);
}

const uint8_t* outputData(size_t& length) { length = s_binaryLength; return s_io; }

bool handle(JsonDocument& req, JsonDocument& out, int32_t id, const uint8_t* raw, size_t rawLen) {
  s_binaryLength = 0;
  const char* act = req["act"] | "";
  if (!act[0]) { errDoc(out, id, "", "BAD_ACT"); return true; }
  s_activity = millis();
  if (strcmp(act, "session_begin") == 0) {
    if (!ready()) errDoc(out, id, act, "NO_SD");
    else if (Nfc::writeBusy()) errDoc(out, id, act, "NFC_BUSY");
    else if (s_session || s_read.active || s_write.active) errDoc(out, id, act, "BUSY");
    else if (!prepareStorage(act)) errDoc(out, id, act, "AUDIO_BUSY");
    else { s_session = true; s_sessionCancelled = false; okDoc(out, id, act); }
    return true;
  }
  if (strcmp(act, "session_end") == 0) {
    if (s_read.active || s_write.active) errDoc(out, id, act, "BUSY");
    else { s_session = s_guard = s_sessionCancelled = false; Player::inhibitPlayback(false); okDoc(out, id, act); }
    return true;
  }
  if (s_sessionCancelled && strcmp(act, "list") != 0) {
    errDoc(out, id, act, "TRANSFER_CANCELLED"); return true;
  }
  if (!s_session && strcmp(act, "list") != 0) {
    errDoc(out, id, act, "NO_SESSION"); return true;
  }
  if (strcmp(act, "list") != 0 && Nfc::writeBusy()) {
    errDoc(out, id, act, "NFC_BUSY"); return true;
  }
  if ((strcmp(act, "read_begin") == 0 || strcmp(act, "write_begin") == 0 ||
       strcmp(act, "mkdir") == 0 || strcmp(act, "remove") == 0 ||
       strcmp(act, "rename") == 0 || strcmp(act, "rescan") == 0) &&
      (s_read.active || s_write.active)) {
    errDoc(out, id, act, "BUSY"); return true;
  }

  if (strcmp(act, "list") == 0) {
    if (!ready()) errDoc(out, id, act, "NO_SD");
    else handleList(req, out, id);
    return true;
  }
  if (strcmp(act, "rescan") == 0) {
    if (!ready()) errDoc(out, id, act, "NO_SD");
    else handleRescan(req, out, id);
    return true;
  }
  if (!ready()) { errDoc(out, id, act, "NO_SD"); return true; }
  if (strcmp(act, "mkdir") == 0) { handleMkdir(req, out, id); return true; }
  if (strcmp(act, "remove") == 0) { handleRemove(req, out, id); return true; }
  if (strcmp(act, "rename") == 0) { handleRename(req, out, id); return true; }
  if (strcmp(act, "read_begin") == 0) { handleReadBegin(req, out, id); return true; }
  if (strcmp(act, "read_chunk") == 0) { handleReadChunk(req, out, id); return true; }
  if (strcmp(act, "read_end") == 0) { handleReadEnd(req, out, id); return true; }
  if (strcmp(act, "write_begin") == 0) { handleWriteBegin(req, out, id); return true; }
  if (strcmp(act, "write_chunk") == 0) { handleWriteChunk(req, out, id, raw, rawLen); return true; }
  if (strcmp(act, "write_end") == 0) { handleWriteEnd(req, out, id); return true; }
  if (strcmp(act, "cancel") == 0) { handleCancel(req, out, id); return true; }

  errDoc(out, id, act, "BAD_ACT");
  return true;
}

} // namespace FileXfer
