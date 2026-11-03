#include "story.h"
#include "config.h"
#include "player.h"
#include <SD_MMC.h>
#include <algorithm>
#include <cstring>

namespace {
  struct Panel { uint32_t ms; char name[28]; };

  String   s_dirs[MAX_STORIES];
  String   s_titles[MAX_STORIES];
  String   s_groups[MAX_STORIES];          // /story 一级分组名
  String   s_catNames[MAX_STORIES];
  int      s_storyCat[MAX_STORIES];
  int      s_catCount = 0;

  struct BrowseEntry {
    String name;
    bool story = false;
    int actual = -1;
    int count = 0;
  };
  BrowseEntry s_browse[MAX_STORIES];
  int         s_browseCount = 0;
  String      s_browsePath;
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

  String baseName(const String& path) {
    int k = path.lastIndexOf('/');
    return k >= 0 ? path.substring(k + 1) : path;
  }

  /* 从 /story 的完整路径推出一级分类: /story/音频故事/xx -> 音频故事;
     /story/音频故事 本身也属于"音频故事"分类。 */
  String groupFromDir(const String& dir) {
    const int rootLen = (int)strlen(STORY_DIR);
    if (!dir.startsWith(STORY_DIR "/")) return String("故事");
    String rest = dir.substring(rootLen + 1);
    if (rest.isEmpty()) return String("故事");
    int slash = rest.indexOf('/');
    return slash < 0 ? rest : rest.substring(0, slash);
  }

  void buildCategories() {
    s_catCount = 0;
    for (int i = 0; i < s_count; ++i) {
      int c = -1;
      for (int j = 0; j < s_catCount; ++j) {
        if (s_catNames[j] == s_groups[i]) { c = j; break; }
      }
      if (c < 0) {
        c = s_catCount++;
        s_catNames[c] = s_groups[i];
      }
      s_storyCat[i] = c;
    }
  }

  String stripExt(const String& name) {
    int k = name.lastIndexOf('.');
    return (k > 0) ? name.substring(0, k) : name;
  }

  void addItem(const String& dir, const String& audio, const String& timeline,
               const String& group) {
    if (s_count >= MAX_STORIES) return;
    s_dirs[s_count] = dir;
    s_audioPaths[s_count] = audio;
    s_timelinePaths[s_count] = timeline;
    s_groups[s_count] = group.length() ? group : String("故事");
    if (timeline.isEmpty()) {
      int k = audio.lastIndexOf('/');
      String name = k >= 0 ? audio.substring(k + 1) : audio;
      s_titles[s_count] = stripExt(name);
    } else {
      int k = dir.lastIndexOf('/');
      s_titles[s_count] = k >= 0 ? dir.substring(k + 1) : dir;
    }
    s_count++;
  }

  /* 音频条目的父目录是目录本身; 图文故事条目的父目录是故事文件夹的上一级 */
  String itemParent(int i) {
    if (i < 0 || i >= s_count) return String();
    if (s_timelinePaths[i].isEmpty()) return s_dirs[i];
    int k = s_dirs[i].lastIndexOf('/');
    return k <= 0 ? String() : s_dirs[i].substring(0, k);
  }

  /* 扫描 /story 下的分组和故事目录 (每个含 mp3 的目录 = 一个故事), 按名称排序 */
  String findFirst(const String& dir, const char* ext);

  void rebuildView() {
    s_viewCount = 0;
    for (int i = 0; i < s_count && s_viewCount < MAX_STORIES; i++) {
      // 家长故事白名单是 64 位; 未启用白名单时支持完整的 300 条音频条目。
      if (!s_storyFilter || (i < 64 && (s_storyMask & (1ULL << i)))) {
        s_view[s_viewCount++] = i;
      }
    }
  }

  /* 递归收集最小故事条目:
     - 有 timeline.lrc 的目录 = 一个图文故事 (目录是最小单位)
     - 没有 timeline 的目录 = 其中的每个 MP3 都是一个音频故事条目
     group 记录 /story 下的一级分组。 */
  void scanDir(const String& dir, int depth, bool root, const String& group) {
    if (depth >= MAX_STORY_DEPTH) return;

    String lrc = findFirst(dir, ".lrc");
    String image = findFirst(dir, ".jpg");
    if (!lrc.isEmpty() && !image.isEmpty()) {
      String audio = findFirst(dir, ".mp3");
      if (!audio.isEmpty()) {
        addItem(dir, audio, lrc, group);
        return;
      }
    }

    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) { s_scanOk = false; return; }
    File f;
    while (s_count < MAX_STORIES && (f = d.openNextFile())) {
      String name = f.name();
      if (!f.isDirectory()) {
        if (hasExt(name.c_str(), ".mp3")) {
          addItem(dir, fullPath(dir, name.c_str()), String(), group);
        }
      } else {
        String child = fullPath(dir, name.c_str());
        String childGroup = group;
        if (depth == 0) {
          // /story 下的第一层目录就是分类 (音频故事/图文故事/自定义分类)。
          childGroup = baseName(child);
        }
        scanDir(child, depth + 1, false, childGroup);
      }
      f.close();
    }
    d.close();
  }

  bool scan() {
    s_scanOk = true;
    s_count = 0;
    s_catCount = 0;
    if (!SD_MMC.exists(STORY_DIR)) return true;
    scanDir(STORY_DIR, 0, true, String());

    int illustrated = 0;
    for (int i = 0; i < s_count; ++i) if (!s_timelinePaths[i].isEmpty()) illustrated++;
    Serial.printf("[STORY] %d item(s) in %s (%d illustrated, %d audio)\n",
                  s_count, STORY_DIR, illustrated, s_count - illustrated);
    buildCategories();
    rebuildView();
    Serial.printf("[STORY] %d categor(ies)\n", s_catCount);
    for (int i = 0; i < s_catCount; ++i) {
      Serial.printf("[STORY]   %s: %d\n", s_catNames[i].c_str(), Story::categoryStoryCount(i));
    }
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
  for (auto& title : s_titles) title = "";
  for (auto& group : s_groups) group = "";
  for (auto& name : s_catNames) name = "";
  for (auto& cat : s_storyCat) cat = 0;
  for (auto& path : s_audioPaths) path = "";
  for (auto& path : s_timelinePaths) path = "";
  s_count = 0;
  s_catCount = 0;
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
    if (!archive.text(dir) || !(dir == "/story" || dir.startsWith("/story/")) ||
        dir.indexOf("/../") >= 0 ||
        !archive.text(audio) || !archive.text(timeline)) return false;
    if ((!audio.isEmpty() && (!audio.startsWith(dir + "/") || audio.indexOf("/../") >= 0 || !hasExt(audio.c_str(), ".mp3"))) ||
        (!timeline.isEmpty() && (!timeline.startsWith(dir + "/") || timeline.indexOf("/../") >= 0 || !hasExt(timeline.c_str(), ".lrc")))) return false;
    if (reading && apply) {
      s_dirs[i] = dir;
      s_groups[i] = groupFromDir(dir);
      s_audioPaths[i] = audio;
      s_timelinePaths[i] = timeline;
      if (timeline.isEmpty()) {
        int k = audio.lastIndexOf('/');
        String name = k >= 0 ? audio.substring(k + 1) : audio;
        s_titles[i] = stripExt(name);
      } else {
        int k = dir.lastIndexOf('/');
        s_titles[i] = k >= 0 ? dir.substring(k + 1) : dir;
      }
    }
  }
  if (reading && apply) { s_count = count; buildCategories(); rebuildView(); }
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

int categoryCount() { return s_catCount; }

const char* categoryName(int c) {
  return (c >= 0 && c < s_catCount) ? s_catNames[c].c_str() : "";
}

int categoryStoryActual(int c, int view) {
  if (c < 0 || c >= s_catCount || view < 0) return -1;
  int found = 0;
  for (int i = 0; i < s_viewCount; ++i) {
    int actual = s_view[i];
    if (s_storyCat[actual] != c) continue;
    if (found == view) return actual;
    found++;
  }
  return -1;
}

const char* categoryStoryTitle(int c, int view) {
  int actual = categoryStoryActual(c, view);
  return actual >= 0 ? title(actual) : "";
}

int categoryStoryCount(int c) {
  if (c < 0 || c >= s_catCount) return 0;
  int n = 0;
  for (int i = 0; i < s_viewCount; ++i) {
    if (s_storyCat[s_view[i]] == c) n++;
  }
  return n;
}

int categoryOfActual(int actual) {
  return (actual >= 0 && actual < s_count) ? s_storyCat[actual] : -1;
}

String categoryPath(int c) {
  if (c < 0 || c >= s_catCount) return String();
  return String("/story/") + s_catNames[c];
}

String parentOfActual(int actual) {
  return itemParent(actual);
}

int actualAtPath(const String& path) {
  for (int i = 0; i < s_count; ++i) {
    const String& leaf = s_timelinePaths[i].isEmpty() ? s_audioPaths[i] : s_dirs[i];
    if (leaf == path) return i;
  }
  return -1;
}

/* 把 path 下所有允许的条目按"直接子项"聚合:
   - 子文件夹: story=false, count=其下条目数
   - 音频文件 / 图文故事文件夹: story=true, actual=条目下标 */
bool browse(const String& path) {
  s_browseCount = 0;
  s_browsePath = path;
  if (path.isEmpty() || !path.startsWith("/story")) return false;

  String prefix = path;
  if (!prefix.endsWith("/")) prefix += "/";

  for (int v = 0; v < s_viewCount; ++v) {
    int actual = s_view[v];
    String parent = itemParent(actual);
    if (parent.isEmpty()) continue;

    bool leaf = (parent == path);
    String child;
    if (leaf) {
      child = title(actual);
    } else if (parent.startsWith(prefix)) {
      String rel = parent.substring(prefix.length());
      int slash = rel.indexOf('/');
      child = (slash < 0) ? rel : rel.substring(0, slash);
    } else {
      continue;
    }
    if (child.isEmpty()) continue;

    int entry = -1;
    for (int i = 0; i < s_browseCount; ++i) {
      if (s_browse[i].name == child) { entry = i; break; }
    }
    if (entry < 0) {
      entry = s_browseCount++;
      s_browse[entry].name = child;
      s_browse[entry].story = false;
      s_browse[entry].actual = -1;
      s_browse[entry].count = 0;
    }
    s_browse[entry].count++;
    if (leaf) {
      s_browse[entry].story = true;
      s_browse[entry].actual = actual;
    }
  }

  std::sort(s_browse, s_browse + s_browseCount,
            [](const BrowseEntry& a, const BrowseEntry& b) { return a.name < b.name; });
  return true;
}

int browseCount() { return s_browseCount; }

const char* browseName(int i) {
  return (i >= 0 && i < s_browseCount) ? s_browse[i].name.c_str() : "";
}

bool browseIsStory(int i) {
  return (i >= 0 && i < s_browseCount) ? s_browse[i].story : false;
}

int browseActual(int i) {
  return (i >= 0 && i < s_browseCount) ? s_browse[i].actual : -1;
}

int browseDescendantCount(int i) {
  return (i >= 0 && i < s_browseCount) ? s_browse[i].count : 0;
}

int currentIndex() { return s_current; }

const char* currentTitle() {
  return s_current >= 0 ? title(s_current) : "";
}

const char* title(int i) {
  if (i < 0 || i >= s_count) return "";
  return s_titles[i].c_str();
}

/* 有 timeline 就是图文故事 (插画+同步), 没有就是纯音频故事 */
Story::Kind kind(int i) {
  if (i < 0 || i >= s_count) return Kind::Audio;
  return s_timelinePaths[i].isEmpty() ? Kind::Audio : Kind::Illustrated;
}

Story::Kind currentKind() {
  return s_current >= 0 ? kind(s_current) : Kind::Audio;
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
  /* 音频故事没有 timeline.lrc: 不加载插画, 只放音频 */
  if (!loadTimeline(s_timelinePaths[i])) {
    s_panelCount = 0;
    Serial.printf("[STORY] %s: audio-only\n", s_dir.c_str());
  }

  s_current  = i;
  s_panelIdx = 0;
  s_active   = true;
  s_finished = false;
  return true;
}

void start() {
  if (!s_active) return;
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
  if (!s_active || s_finished) return;
  if (Player::takeEof()) { s_finished = true; return; }
  if (s_panelCount == 0) return;           // 音频故事: 没有插画需要同步

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
