#include "story.h"
#include "config.h"
#include "player.h"
#include <SD_MMC.h>
#include <algorithm>

namespace {
  struct Panel { uint32_t ms; char name[28]; };

  String   s_dirs[MAX_STORIES];
  String   s_audioPaths[MAX_STORIES], s_timelinePaths[MAX_STORIES];
  int      s_count = 0;
  bool     s_scanOk = true;

  /* 家长白名单视图 */
  uint64_t s_storyMask   = ~0ULL;
  bool     s_storyFilter = false;
  int      s_view[MAX_STORIES];
  int      s_viewCount = 0;
  int      s_current   = -1;       // 当前打开的故事实际下标

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
  String findFirst(const String& dir, const char* ext);

  void rebuildView() {
    s_viewCount = 0;
    for (int i = 0; i < s_count && s_viewCount < MAX_STORIES; i++) {
      if (!s_storyFilter || (s_storyMask & (1ULL << i))) s_view[s_viewCount++] = i;
    }
  }

  bool scan() {
    s_scanOk = true;
    s_count = 0;
    if (!SD_MMC.exists(STORY_DIR)) return true;

    File root = SD_MMC.open(STORY_DIR);
    if (!root || !root.isDirectory()) return false;

    File f;
    while (s_count < MAX_STORIES && (f = root.openNextFile())) {
      if (f.isDirectory()) s_dirs[s_count++] = fullPath(STORY_DIR, f.name());
      f.close();
    }
    root.close();

    std::sort(s_dirs, s_dirs + s_count,
              [](const String& a, const String& b) { return strcmp(a.c_str(), b.c_str()) < 0; });
    for (int i = 0; i < s_count; ++i) {
      s_audioPaths[i] = findFirst(s_dirs[i], ".mp3");
      s_timelinePaths[i] = findFirst(s_dirs[i], ".lrc");
    }
    Serial.printf("[STORY] %d story(ies) in %s\n", s_count, STORY_DIR);
    rebuildView();
    return s_scanOk;
  }

  /* 目录里第一个指定后缀的文件 (完整路径), 找不到返回空串 */
  String findFirst(const String& dir, const char* ext) {
    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) { s_scanOk = false; return String(); }

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
  bool loadTimeline(const String& lrc) {
    s_panelCount = 0;
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

void clear() {
  for (auto& dir : s_dirs) dir = "";
  for (auto& path : s_audioPaths) path = "";
  for (auto& path : s_timelinePaths) path = "";
  s_count = 0;
  s_current = -1;
  rebuildView();
}

bool rescan() { clear(); return scan(); }

bool index(CatalogArchive& archive, bool reading, bool apply) {
  uint32_t count = s_count;
  if (!archive.number(count) || count > MAX_STORIES) return false;
  if (reading && apply) clear();
  for (uint32_t i = 0; i < count; ++i) {
    String dir = reading ? String() : s_dirs[i];
    String audio = reading ? String() : s_audioPaths[i];
    String timeline = reading ? String() : s_timelinePaths[i];
    if (!archive.text(dir) || !dir.startsWith("/story/") || dir.indexOf("/../") >= 0 ||
        !archive.text(audio) || !archive.text(timeline)) return false;
    if ((!audio.isEmpty() && (!audio.startsWith(dir + "/") || audio.indexOf("/../") >= 0 || !hasExt(audio.c_str(), ".mp3"))) ||
        (!timeline.isEmpty() && (!timeline.startsWith(dir + "/") || timeline.indexOf("/../") >= 0 || !hasExt(timeline.c_str(), ".lrc")))) return false;
    if (reading && apply) { s_dirs[i] = dir; s_audioPaths[i] = audio; s_timelinePaths[i] = timeline; }
  }
  if (reading && apply) { s_count = count; rebuildView(); }
  return true;
}

int  count() { return s_count; }

void setStoryFilter(uint64_t mask, bool enabled) {
  s_storyMask   = mask;
  s_storyFilter = enabled;
  rebuildView();
}

int viewCount() { return s_viewCount; }

int viewActual(int view) {
  if (view < 0 || view >= s_viewCount) return -1;
  return s_view[view];
}

const char* viewTitle(int view) {
  int actual = viewActual(view);
  return actual >= 0 ? title(actual) : "";
}

int currentIndex() { return s_current; }

const char* currentTitle() {
  return s_current >= 0 ? title(s_current) : "";
}

const char* title(int i) {
  if (i < 0 || i >= s_count) return "";
  int k = s_dirs[i].lastIndexOf('/');
  return (k >= 0) ? s_dirs[i].c_str() + k + 1 : s_dirs[i].c_str();
}

bool open(int i) {
  stop();
  if (i < 0 || i >= s_count) return false;

  s_dir   = s_dirs[i];
  s_audio = s_audioPaths[i];
  if (s_audio.length() == 0 || !SD_MMC.exists(s_audio)) {
    Serial.printf("[STORY] no mp3 in %s\n", s_dir.c_str());
    return false;
  }
  if (!loadTimeline(s_timelinePaths[i])) {
    Serial.printf("[STORY] no timeline in %s\n", s_dir.c_str());
    return false;
  }

  s_current  = i;
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
  s_current    = -1;
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
