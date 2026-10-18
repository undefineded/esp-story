#include "story.h"
#include "config.h"
#include "player.h"
#include <SD_MMC.h>
#include <algorithm>

namespace {
  struct Panel { uint32_t ms; char name[28]; };

  String   s_dirs[MAX_STORIES];
  int      s_count = 0;

  Panel    s_panels[MAX_PANELS];
  int      s_panelCount = 0;
  int      s_panelIdx   = 0;
  String   s_dir;                  // 当前故事目录
  String   s_audio;                // 当前故事音频完整路径
  char     s_path[160];            // 当前插画完整路径 (供 panelPath 返回)
  bool     s_active   = false;
  bool     s_finished = false;
  uint32_t s_lastSec  = 0xFFFFFFFF;

  bool hasExt(const char* name, const char* ext) {
    String s(name);  s.toLowerCase();
    String e(ext);   e.toLowerCase();
    return s.endsWith(e);
  }

  /* openNextFile 返回的名字可能是纯文件名, 统一补成完整路径 */
  String fullPath(const String& dir, const char* name) {
    String n(name);
    return n.startsWith("/") ? n : (dir + "/" + n);
  }

  /* 扫描 /story 下的子目录 (每个目录 = 一个故事), 按名称排序 */
  void scan() {
    s_count = 0;
    if (!SD_MMC.exists(STORY_DIR)) return;

    File root = SD_MMC.open(STORY_DIR);
    if (!root || !root.isDirectory()) { if (root) root.close(); return; }

    File f;
    while ((f = root.openNextFile()) && s_count < MAX_STORIES) {
      if (f.isDirectory()) s_dirs[s_count++] = fullPath(STORY_DIR, f.name());
      f.close();
    }
    root.close();

    std::sort(s_dirs, s_dirs + s_count,
              [](const String& a, const String& b) { return strcmp(a.c_str(), b.c_str()) < 0; });
    Serial.printf("[STORY] %d story(ies) in %s\n", s_count, STORY_DIR);
  }

  /* 目录里第一个指定后缀的文件 (完整路径), 找不到返回空串 */
  String findFirst(const String& dir, const char* ext) {
    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return String(); }

    File f;
    String hit;
    while ((f = d.openNextFile())) {
      bool ok = !f.isDirectory() && hasExt(f.name(), ext);
      if (ok) hit = fullPath(dir, f.name());
      f.close();
      if (ok) break;
    }
    d.close();
    return hit;
  }

  /* "mm:ss.xx" -> 毫秒 */
  uint32_t parseMs(const String& ts) {
    int c1 = ts.indexOf(':');
    if (c1 < 0) return 0;
    int c2 = ts.indexOf('.');
    long mm = ts.substring(0, c1).toInt();
    long ss = ((c2 > 0) ? ts.substring(c1 + 1, c2) : ts.substring(c1 + 1)).toInt();
    long ff = 0;
    if (c2 > 0) {
      String fr = ts.substring(c2 + 1);   fr.trim();
      if      (fr.length() == 1) ff = fr.toInt() * 100;   // .5  -> 500ms
      else if (fr.length() >= 2) ff = fr.substring(0, 2).toInt() * 10;  // .54 -> 540ms
    }
    return (uint32_t)(mm * 60000L + ss * 1000L + ff);
  }

  /* 解析 timeline.lrc: [mm:ss.xx]NN.jpg | 摘要 */
  bool loadTimeline(const String& dir) {
    s_panelCount = 0;
    String lrc = findFirst(dir, ".lrc");
    if (lrc.length() == 0) return false;

    File f = SD_MMC.open(lrc);
    if (!f) return false;

    bool truncated = false;
    while (f.available()) {
      if (s_panelCount >= MAX_PANELS) { truncated = true; break; }
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() < 3 || line[0] != '[') continue;
      int rb = line.indexOf(']');
      if (rb < 0) continue;

      String rest = line.substring(rb + 1);
      rest.trim();
      int sp   = rest.indexOf(' ');                    // "01.jpg | 摘要" -> 取空格前
      String nm = (sp > 0) ? rest.substring(0, sp) : rest;
      nm.trim();
      if (nm.length() == 0) continue;

      Panel& p = s_panels[s_panelCount];
      p.ms = parseMs(line.substring(1, rb));
      strncpy(p.name, nm.c_str(), sizeof(p.name) - 1);
      p.name[sizeof(p.name) - 1] = 0;
      s_panelCount++;
    }
    f.close();
    if (truncated)
      Serial.printf("[STORY] timeline exceeds MAX_PANELS(%d), extra panels ignored\n", MAX_PANELS);
    return s_panelCount > 0;
  }
}

namespace Story {

void begin() { scan(); }

int  count() { return s_count; }

const char* title(int i) {
  if (i < 0 || i >= s_count) return "";
  int k = s_dirs[i].lastIndexOf('/');
  return (k >= 0) ? s_dirs[i].c_str() + k + 1 : s_dirs[i].c_str();
}

bool open(int i) {
  stop();
  if (i < 0 || i >= s_count) return false;

  s_dir   = s_dirs[i];
  s_audio = findFirst(s_dir, ".mp3");
  if (s_audio.length() == 0) {
    Serial.printf("[STORY] no mp3 in %s\n", s_dir.c_str());
    return false;
  }
  if (!loadTimeline(s_dir)) {
    Serial.printf("[STORY] no timeline in %s\n", s_dir.c_str());
    return false;
  }

  s_panelIdx = 0;
  s_active   = true;
  s_finished = false;
  return true;
}

void start() {
  if (!s_active || s_panelCount == 0) return;
  s_panelIdx = 0;
  s_finished = false;
  s_lastSec  = 0xFFFFFFFF;
  Player::setAutoNext(false);              // 故事只放一遍, 结束交给 Story 处理
  Player::playPath(s_audio.c_str(), true);
}

void stop() {
  Player::stop();
  Player::setAutoNext(true);               // 恢复音乐模式的自动续播
  s_active     = false;
  s_finished   = false;
  s_panelCount = 0;
}

void loop() {
  if (!s_active || s_finished || s_panelCount == 0) return;
  if (Player::takeEof()) { s_finished = true; return; }

  uint32_t sec = Player::status().curSec;
  if (sec == s_lastSec) return;
  s_lastSec = sec;

  uint32_t ms  = sec * 1000u;
  int      idx = 0;
  for (int i = 0; i < s_panelCount; i++) {
    if (s_panels[i].ms <= ms) idx = i; else break;
  }
  s_panelIdx = idx;
}

bool active() { return s_active; }

bool takeFinished() {
  bool f = s_finished;
  s_finished = false;
  return f;
}

int panelCount() { return s_panelCount; }

int panelIndex() { return s_panelIdx; }

const char* panelPath() {
  if (s_panelCount == 0) return nullptr;
  int i = s_panelIdx;
  if (i < 0) i = 0;
  if (i >= s_panelCount) i = s_panelCount - 1;
  snprintf(s_path, sizeof(s_path), "%s/%s", s_dir.c_str(), s_panels[i].name);
  return s_path;
}

}
