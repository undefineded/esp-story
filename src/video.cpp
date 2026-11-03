#include "video.h"
#include "config.h"
#include "ui.h"
#include "player.h"
#include "video_timing.h"
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cstring>

/*  AVI 结构 (只取播放需要的部分):
 *    RIFF('AVI ')
 *      LIST('hdrl')  avih / LIST('strl'){ strh, strf } ...
 *      LIST('movi')  '00dc' 帧1 | '00dc' 帧2 | ... | '01wb' 音频
 *      idx1          帧索引 (不用: 各封装器偏移基准不一致, 直接扫 movi 更稳)
 *
 *  帧编码由 strf 的 biCompression 决定: 'QOI ' (软解最快) 或 'MJPG'。
 *
 *  打开时扫一遍 movi, 只记录每帧在文件里的偏移和长度 (8B/帧, 放 PSRAM),
 *  播放时按时间轴 seek 到目标帧, 落后就直接跳到该帧 —— 解码/推屏慢时自动丢帧,
 *  不会越播越慢。
 */

namespace {

struct Entry { String name; String path; };
Entry s_items[MAX_VIDEOS];
int   s_count = 0;

struct FrameEntry { uint32_t offset; uint32_t size; };

FrameEntry* s_frames   = nullptr;
int         s_frameCap = 0;
int         s_frameCount = 0;
uint8_t*    s_buf      = nullptr;
size_t      s_bufCap   = 0;
bool        s_truncated = false;

File   s_file;
int    s_index      = -1;
bool   s_active     = false;
bool   s_paused     = false;
bool   s_finished   = false;
bool   s_clock      = false;
int    s_drawn      = -1;
uint32_t s_frameUs  = VideoTiming::kDefaultFrameUs;
uint32_t s_durationMs = 0;
uint32_t s_epoch    = 0;      // 计时起点 (millis)
uint32_t s_startedAt = 0;
uint32_t s_pauseAt  = 0;
String s_audioPath;
bool   s_containerAudio = false;
bool   s_qoi = false;
String s_error;
bool   s_decodeWarned = false;

/* 实际帧率统计: 1 秒窗口内成功交付的帧数, 以及被时间轴丢掉的帧数 */
uint32_t s_fpsFrames    = 0;
uint32_t s_fpsDropped   = 0;
uint32_t s_fpsWindowAt  = 0;
int      s_fpsShown     = 0;
int      s_fpsDropShown = 0;

constexpr uint32_t kAudioReadyTimeoutMs = 1500;

/* ---------------- 小工具 ---------------- */

uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool readAt(File& f, uint32_t pos, void* dst, size_t n) {
  if (!f.seek(pos)) return false;
  return f.read(static_cast<uint8_t*>(dst), n) == (int)n;
}

bool tagIs(const uint8_t* p, const char* s) { return memcmp(p, s, 4) == 0; }

String lower(const String& in) {
  String s = in;
  s.toLowerCase();
  return s;
}

bool isVideoName(const String& n) {
  String s = lower(n);
  return s.endsWith(".avi") || s.endsWith(".mjpg") || s.endsWith(".mjpeg");
}

String stripExt(const String& n) {
  int k = n.lastIndexOf('.');
  return (k > 0) ? n.substring(0, k) : n;
}

bool isVideoChunk(const uint8_t* id) {
  return id[0] == '0' && id[1] == '0' && id[2] == 'd' && (id[3] == 'c' || id[3] == 'b');
}

/* 01wb = PCM/MP3 音频块; 01dc/01db = 压缩音频流 (例如 MP3-in-AVI) */
bool isAudioChunk(const uint8_t* id) {
  if (id[2] == 'w' && id[3] == 'b') return true;
  if ((id[2] == 'd') && (id[3] == 'c' || id[3] == 'b') && !isVideoChunk(id)) return true;
  return false;
}

/* ---------------- AVI 头解析 ---------------- */

struct AvihInfo {
  uint32_t usPerFrame  = 0;
  uint32_t totalFrames = 0;
  uint32_t width       = 0;
  uint32_t height      = 0;
};

struct StreamInfo {
  bool     video   = false;
  bool     audio   = false;
  bool     mjpeg   = false;   // 帧编码 = MJPG (JPEG)
  bool     qoi     = false;   // 帧编码 = QOI (软解最快)
  uint32_t frameUs = 0;
  uint32_t width   = 0;
  uint32_t height  = 0;
};

void parseStrl(File& f, uint32_t start, uint32_t end, StreamInfo& vs) {
  uint32_t pos = start;
  uint32_t strfPos = 0, strfSize = 0;   // strf 可能排在 strh 前面, 先记下来最后再解析
  bool     isVideo = false, isAudio = false;

  while (pos + 8 <= end) {
    uint8_t h[8];
    if (!readAt(f, pos, h, 8)) return;
    const uint32_t size = rd32(h + 4);
    const uint64_t dataEnd = (uint64_t)pos + 8 + size;
    if (dataEnd > end) return;

    if (tagIs(h, "strh")) {
      uint8_t b[56] = {0};
      const uint32_t n = size < sizeof(b) ? size : sizeof(b);
      if (!readAt(f, pos + 8, b, n)) return;
      isVideo = (memcmp(b, "vids", 4) == 0);
      isAudio = (memcmp(b, "auds", 4) == 0);
      const uint32_t scale = rd32(b + 20);
      const uint32_t rate  = rd32(b + 24);
      if (isVideo && rate > 0 && scale > 0) {
        vs.frameUs = (uint32_t)((uint64_t)scale * 1000000ULL / rate);
        vs.video = true;
      }
      if (isAudio) vs.audio = true;
    } else if (tagIs(h, "strf")) {
      strfPos  = pos + 8;
      strfSize = size;
    }
    pos = (uint32_t)(dataEnd + (size & 1));
  }

  if (isVideo && strfSize) {
    uint8_t b[40] = {0};
    const uint32_t n = strfSize < sizeof(b) ? strfSize : sizeof(b);
    if (!readAt(f, strfPos, b, n)) return;
    vs.width  = rd32(b + 4);
    vs.height = rd32(b + 8);
    String fourcc(reinterpret_cast<const char*>(b) + 16, 4);
    fourcc.toUpperCase();
    vs.mjpeg = (fourcc == "MJPG");
    vs.qoi   = (fourcc == "QOI ");
  }
}

void parseHdrl(File& f, uint32_t start, uint32_t end, AvihInfo& avih, StreamInfo& vs) {
  uint32_t pos = start;
  while (pos + 8 <= end) {
    uint8_t h[8];
    if (!readAt(f, pos, h, 8)) return;
    const uint32_t size = rd32(h + 4);
    const uint64_t dataEnd = (uint64_t)pos + 8 + size;
    if (dataEnd > end + 1) return;

    if (tagIs(h, "LIST")) {
      uint8_t t[4];
      if (!readAt(f, pos + 8, t, 4)) return;
      if (tagIs(t, "strl")) {
        parseStrl(f, pos + 12, (uint32_t)dataEnd, vs);
      } else if (tagIs(t, "hdrl")) {
        parseHdrl(f, pos + 12, (uint32_t)dataEnd, avih, vs);
      }
    } else if (tagIs(h, "avih")) {
      uint8_t b[56] = {0};
      const uint32_t n = size < sizeof(b) ? size : sizeof(b);
      if (!readAt(f, pos + 8, b, n)) return;
      avih.usPerFrame  = rd32(b + 0);
      avih.totalFrames = rd32(b + 16);
      avih.width       = rd32(b + 32);
      avih.height      = rd32(b + 36);
    }
    pos = (uint32_t)(dataEnd + (size & 1));
  }
}

/* 扫 movi: 记录每一帧的数据偏移/长度, 顺带看有没有内嵌音轨 */
void scanChunks(File& f, uint32_t start, uint32_t end, int depth) {
  uint32_t pos = start;
  while (pos + 8 <= end && s_frameCount < s_frameCap) {
    uint8_t h[8];
    if (!readAt(f, pos, h, 8)) return;
    const uint32_t size = rd32(h + 4);
    const uint64_t dataEnd = (uint64_t)pos + 8 + size;
    if (dataEnd > end || size > (uint32_t)MAX_VIDEO_BYTES * 64u) return;   // 头部损坏

    if (tagIs(h, "LIST")) {
      if (depth < 2) scanChunks(f, pos + 12, (uint32_t)dataEnd, depth + 1);
    } else if (isVideoChunk(h)) {
      s_frames[s_frameCount].offset = pos + 8;
      s_frames[s_frameCount].size   = size;
      s_frameCount++;
    } else if (isAudioChunk(h)) {
      s_containerAudio = true;
    }
    pos = (uint32_t)(dataEnd + (size & 1));
  }
  if (s_frameCount >= s_frameCap) s_truncated = true;
}

/* ---------------- 资源 ---------------- */

bool ensureBuffers() {
  if (!s_frames) {
    s_frames = static_cast<FrameEntry*>(
        heap_caps_malloc(sizeof(FrameEntry) * MAX_VIDEO_FRAMES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_frameCap = s_frames ? MAX_VIDEO_FRAMES : 0;
  }
  if (!s_buf) {
    s_buf = static_cast<uint8_t*>(
        heap_caps_malloc(MAX_VIDEO_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_bufCap = s_buf ? MAX_VIDEO_BYTES : 0;
  }
  return s_frames && s_buf && s_frameCap > 0 && s_bufCap > 0;
}

bool drawFrame(int idx) {
  if (!s_file || idx < 0 || idx >= s_frameCount) return false;
  const uint32_t off  = s_frames[idx].offset;
  const uint32_t size = s_frames[idx].size;
  if (size < 4 || size > s_bufCap) return false;
  if (!s_file.seek(off)) return false;
  if (s_file.read(s_buf, size) != (int)size) return false;
  const bool ok = Ui::drawVideoFrame(s_buf, size, s_qoi);
  s_drawn = idx;
  if (!ok && !s_decodeWarned) {
    s_decodeWarned = true;
    Serial.printf("[VID] frame %d decode failed (%u B)\n", idx, (unsigned)size);
  }
  return ok;
}

/* 找同名音轨: .mp3 / .wav */
void findAudioTrack(const String& videoPath) {
  s_audioPath = "";
  String base = stripExt(videoPath);
  if (SD_MMC.exists(base + ".mp3"))      s_audioPath = base + ".mp3";
  else if (SD_MMC.exists(base + ".wav")) s_audioPath = base + ".wav";
}

} // namespace

namespace Video {

int scan() {
  s_count = 0;
  if (!SD_MMC.exists(VIDEO_DIR)) {
    SD_MMC.mkdir(VIDEO_DIR);
    Serial.printf("[VID] %s not found, created it\n", VIDEO_DIR);
    return 0;
  }
  File dir = SD_MMC.open(VIDEO_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  File f;
  while (s_count < MAX_VIDEOS && (f = dir.openNextFile())) {
    if (!f.isDirectory() && isVideoName(f.name())) {
      String name = f.name();
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (!name.startsWith(".")) {
        s_items[s_count].name = name;
        s_items[s_count].path = String(VIDEO_DIR) + "/" + name;
        s_count++;
      }
    }
    f.close();
  }
  dir.close();

  std::sort(s_items, s_items + s_count,
            [](const Entry& a, const Entry& b) { return a.name < b.name; });
  Serial.printf("[VID] %d video(s) in %s\n", s_count, VIDEO_DIR);
  return s_count;
}

int count() { return s_count; }

const char* name(int i) {
  return (i >= 0 && i < s_count) ? s_items[i].name.c_str() : "";
}

const char* title(int i) {
  static String t;
  if (i < 0 || i >= s_count) return "";
  t = stripExt(s_items[i].name);
  return t.c_str();
}

bool open(int i) {
  stop();
  s_error = "";
  s_truncated = false;
  s_containerAudio = false;
  s_decodeWarned = false;

  if (i < 0 || i >= s_count) { s_error = "视频不存在"; return false; }
  if (!SD_MMC.exists(s_items[i].path)) { s_error = "文件已不在卡上"; return false; }
  if (!ensureBuffers()) { s_error = "PSRAM 内存不足"; return false; }

  File file = SD_MMC.open(s_items[i].path, FILE_READ);
  if (!file) { s_error = "打不开文件"; return false; }

  uint8_t hdr[12];
  const uint32_t fileSize = file.size();
  if (file.read(hdr, 12) != 12 || !tagIs(hdr, "RIFF") || !tagIs(hdr + 8, "AVI ")) {
    file.close();
    s_error = "不是 AVI 文件";
    return false;
  }

  AvihInfo avih;
  StreamInfo vs;
  uint32_t moviStart = 0, moviEnd = 0;
  uint32_t idxPos = 0, idxSize = 0;      // 转码时写入的 SBXI 帧索引
  uint32_t pos = 12;
  int      chunks = 0;
  while (pos + 8 <= fileSize && chunks++ < 32) {
    uint8_t h[8];
    if (!readAt(file, pos, h, 8)) break;
    const uint32_t size = rd32(h + 4);
    const uint64_t next = (uint64_t)pos + 8 + size + (size & 1);
    if (next > (uint64_t)fileSize + 1) break;
    if (tagIs(h, "LIST")) {
      uint8_t t[4];
      if (!readAt(file, pos + 8, t, 4)) break;
      if (tagIs(t, "hdrl")) {
        parseHdrl(file, pos + 12, pos + 8 + size, avih, vs);
      } else if (tagIs(t, "movi")) {
        moviStart = pos + 12;
        moviEnd   = pos + 8 + size;
      }
    } else if (tagIs(h, "SBXI")) {
      idxPos  = pos;
      idxSize = size;
    }
    pos = (uint32_t)next;
  }

  if (!moviStart || moviEnd <= moviStart) {
    file.close();
    s_error = "AVI 缺少视频数据";
    return false;
  }
  if (!vs.mjpeg && !vs.qoi) {
    file.close();
    s_error = "视频编码不支持 (需 MJPG 或 QOI)";
    return false;
  }

  uint32_t frameUs = vs.frameUs ? vs.frameUs : avih.usPerFrame;
  if (frameUs == 0) frameUs = VideoTiming::kDefaultFrameUs;
  if (frameUs < 5000 || frameUs > 200000) frameUs = VideoTiming::kDefaultFrameUs;

  s_frameCount = 0;
  bool indexed = false;
  if (idxPos && idxSize >= 16) {
    /* 一次读入整张索引, 免去逐帧 seek (1966 帧 = 1966 次随机读, 之前几秒就耗在这) */
    uint8_t ih[16];
    if (readAt(file, idxPos, ih, 16) && tagIs(ih, "SBXI") && tagIs(ih + 8, "SBIX")) {
      const uint32_t count = rd32(ih + 12);
      const uint32_t need  = count * 8;
      if (count > 0 && count <= (uint32_t)s_frameCap &&
          idxSize == 8 + need && need <= s_bufCap &&
          readAt(file, idxPos + 16, s_buf, need)) {
        bool okay = true;
        for (uint32_t k = 0; k < count && okay; k++) {
          const uint32_t off = rd32(s_buf + k * 8);
          const uint32_t len = rd32(s_buf + k * 8 + 4);
          if (len == 0 || len > MAX_VIDEO_BYTES || off < 16 ||
              (uint64_t)off + 8 + len > fileSize) {
            okay = false;
          } else {
            s_frames[k].offset = off + 8;    // 索引记的是块头, 播放要数据区
            s_frames[k].size   = len;
          }
        }
        if (okay) { s_frameCount = (int)count; indexed = true; }
      }
    }
  }
  if (!indexed) scanChunks(file, moviStart, moviEnd, 0);

  if (s_frameCount == 0) {
    file.close();
    s_error = "没有可播放的帧";
    return false;
  }

  s_frameUs    = frameUs;
  s_durationMs = VideoTiming::durationMs(s_frameCount, s_frameUs);
  s_file       = file;
  s_index      = i;
  s_qoi        = vs.qoi;
  findAudioTrack(s_items[i].path);

  Serial.printf("[VID] open %s: %s %d frames, %d fps, %u ms, %ux%u, audio=%s, %s%s%s\n",
                s_items[i].name.c_str(), s_qoi ? "QOI" : "MJPEG",
                s_frameCount, VideoTiming::fps(s_frameUs),
                (unsigned)s_durationMs, (unsigned)vs.width, (unsigned)vs.height,
                s_audioPath.length() ? s_audioPath.c_str() : "none",
                indexed ? "indexed" : "scanned",
                s_truncated ? ", truncated" : "",
                (s_containerAudio && s_audioPath.isEmpty()) ? ", container audio ignored" : "");
  return true;
}

void start() {
  if (!s_file) return;
  s_active     = true;
  s_finished   = false;
  s_paused     = false;
  s_clock      = false;
  s_drawn      = -1;
  s_startedAt  = millis();
  s_epoch      = s_startedAt;
  s_pauseAt    = s_startedAt;
  s_durationMs = VideoTiming::durationMs(s_frameCount, s_frameUs);
  s_fpsFrames  = 0;
  s_fpsDropped = 0;
  s_fpsWindowAt = s_startedAt;
  s_fpsShown   = 0;
  s_fpsDropShown = 0;

  Ui::videoFrameReset();
  drawFrame(0);                    // 先出一帧, 音频起播的几百毫秒里不留白屏
}

void stop() {
  if (s_file) s_file.close();
  s_active    = false;
  s_paused    = false;
  s_finished  = false;
  s_clock     = false;
  s_drawn     = -1;
  s_frameCount = 0;
  s_durationMs = 0;
  s_index     = -1;
  s_audioPath = "";
  s_qoi       = false;
  s_fpsShown  = 0;
  s_fpsDropShown = 0;
  Player::stop();
  Player::setAutoNext(true);       // 视频不占用音乐模式的自动续播
}

void pause(bool on) {
  if (!s_active || on == s_paused) return;
  if (on) {
    s_pauseAt = millis();
    s_paused  = true;
    s_fpsShown = 0;                 // 暂停时不再交付画面
    s_fpsDropShown = 0;
  } else {
    if (s_clock) s_epoch += millis() - s_pauseAt;
    s_paused = false;
    s_fpsFrames = 0;
    s_fpsDropped = 0;
    s_fpsWindowAt = millis();
  }
}

bool paused() { return s_paused; }
bool active() { return s_active; }

bool takeFinished() {
  const bool f = s_finished;
  s_finished = false;
  return f;
}

void loop() {
  if (!s_active || s_finished || s_paused) return;

  if (!s_clock) {
    /* 等音轨真正出声再开始计时, 否则画面会比声音早 200~400ms */
    const bool audioReady = s_audioPath.isEmpty() ||
                            (Player::connected() && !Player::paused());
    if (!audioReady && millis() - s_startedAt < kAudioReadyTimeoutMs) return;
    s_epoch = millis();
    s_clock = true;
  }

  const uint32_t elapsed = millis() - s_epoch;
  const int target = VideoTiming::frameAt(elapsed, s_frameUs, s_frameCount);
  if (target < 0) { s_finished = true; return; }

  if (target != s_drawn) {
    /* 落后时直接跳到目标帧: 跳过的帧计入丢帧统计 */
    if (s_drawn >= 0 && target > s_drawn + 1) {
      s_fpsDropped += (uint32_t)(target - s_drawn - 1);
    }
    drawFrame(target);
    s_fpsFrames++;
  }

  /* 1 秒窗口: 统计实际交付帧率和丢帧数 */
  const uint32_t now = millis();
  if (now - s_fpsWindowAt >= 1000) {
    const uint32_t span = now - s_fpsWindowAt;
    s_fpsShown     = (int)((s_fpsFrames * 1000u + span / 2) / span);
    s_fpsDropShown = (int)s_fpsDropped;
    s_fpsFrames    = 0;
    s_fpsDropped   = 0;
    s_fpsWindowAt  = now;
  }

  if (elapsed >= s_durationMs) s_finished = true;
}

int index() { return s_index; }

int positionMs() {
  if (!s_active || !s_clock) return 0;
  uint32_t ms = s_paused ? (s_pauseAt - s_epoch) : (millis() - s_epoch);
  if (ms > s_durationMs) ms = s_durationMs;
  return (int)ms;
}

int durationMs() { return (int)s_durationMs; }
int frameCount() { return s_frameCount; }
int fps()        { return VideoTiming::fps(s_frameUs); }
int actualFps()  { return s_fpsShown; }        // 最近 1 秒实际交付
int droppedFrames() { return s_fpsDropShown; } // 最近 1 秒被丢掉的帧
bool hasAudio()  { return s_audioPath.length() > 0; }
const char* audioPath() { return s_audioPath.c_str(); }
const char* errorText() { return s_error.c_str(); }

} // namespace Video
