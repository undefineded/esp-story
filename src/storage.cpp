#include "storage.h"
#include "config.h"
#include <SD_MMC.h>
#include <algorithm>

namespace {
  /* 曲目: 路径 + 所属分类 (-1 = /music 根目录直放) + uuid (按文件名算, 掉电记忆的稳定身份) */
  struct Track { String path; int8_t cat; uint32_t id; };

  Track   s_tracks[MAX_TRACKS];
  int     s_total    = 0;

  String  s_cats[MAX_CATEGORIES];   // 分类名 (= 一级子文件夹名, 不含"全部歌曲")
  int     s_catCount = 0;

  int     s_curCat   = 0;           // 当前分类: 0 = 全部歌曲, 1..N = s_cats[i-1]
  int16_t s_cur[MAX_TRACKS];        // 当前分类曲目 -> s_tracks 下标
  int     s_curCount = 0;
  int16_t s_nursery[MAX_TRACKS];    // 童谣预览列表 (浏览时不动 s_cur, 按下播放才切换)
  int     s_nurseryCount = 0;

  bool    s_ready = false;
  bool    s_scanOk = true;

  /* 家长白名单视图 */
  uint64_t s_catMask   = ~0ULL;
  bool     s_catFilter = false;
  int      s_view[MAX_CATEGORIES + 1];
  int      s_viewCount = 0;

  void rebuildView() {
    s_viewCount = 0;
    bool allAllowed = true;
    for (int i = 1; i <= s_catCount; i++) {
      if (!(s_catMask & (1ULL << i))) { allAllowed = false; break; }
    }
    /* 白名单开启且存在被禁分类时, 隐藏聚合的"全部歌曲" */
    if (!s_catFilter || allAllowed) s_view[s_viewCount++] = 0;
    for (int i = 1; i <= s_catCount && s_viewCount <= MAX_CATEGORIES; i++) {
      if (!s_catFilter || (s_catMask & (1ULL << i))) s_view[s_viewCount++] = i;
    }
  }

  bool isMp3(const char* n) {
    String s(n);
    s.toLowerCase();
    return s.endsWith(".mp3");
  }

  /* openNextFile 返回的名字可能是纯文件名, 统一补成完整路径 */
  String fullPath(const String& dir, const char* name) {
    String n(name);
    return n.startsWith("/") ? n : (dir + "/" + n);
  }

  String baseName(const String& path) {
    int k = path.lastIndexOf('/');
    return (k >= 0) ? path.substring(k + 1) : path;
  }

  /* 曲目 uuid: 对文件名做 FNV-1a 32 位散列。
     用文件名而不是下标/路径: 增删歌、换卡、目录改名后下标会整体错位, 文件名不变。
     0 保留为"无记忆", 因此散列结果为 0 时改记 1。 */
  uint32_t nameId(const String& path) {
    String n = baseName(path);
    uint32_t h = 2166136261u;
    for (unsigned int i = 0; i < n.length(); i++) {
      h ^= (uint8_t)n[i];
      h *= 16777619u;
    }
    return h ? h : 1u;
  }

  /* 童谣分类识别: 只按分类名匹配, 不猜文件名。
     统一转小写后同时支持中文关键词和英文关键词 (nursery / rhyme)。 */
  bool isNurseryName(const String& name) {
    String s = name;
    s.toLowerCase();
    return s.indexOf("童谣") >= 0 || s.indexOf("儿歌") >= 0 ||
           s.indexOf("nursery") >= 0 || s.indexOf("rhyme") >= 0;
  }

  /* 曲目 i 是否属于童谣汇总 (家长白名单关闭的分类不计入) */
  bool nurseryTrack(int i) {
    int cat = s_tracks[i].cat;
    if (cat < 0) return false;
    if (s_catFilter && !(s_catMask & (1ULL << (cat + 1)))) return false;
    return isNurseryName(s_cats[cat]);
  }

  /* 只重建预览列表, 不碰当前播放列表 */
  void rebuildNursery() {
    s_nurseryCount = 0;
    for (int i = 0; i < s_total && s_nurseryCount < MAX_TRACKS; i++) {
      if (nurseryTrack(i)) s_nursery[s_nurseryCount++] = (int16_t)i;
    }
  }

  /* 递归收集 dir(含子目录)内的 mp3, 全部归入分类 cat */
  void collectDeep(const String& dir, int cat) {
    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) { s_scanOk = false; return; }

    File f;
    while (s_total < MAX_TRACKS && (f = d.openNextFile())) {
      if (f.isDirectory())        collectDeep(fullPath(dir, f.name()), cat);
      else if (isMp3(f.name()))   {
        s_tracks[s_total].path = fullPath(dir, f.name());
        s_tracks[s_total].cat  = (int8_t)cat;
        s_tracks[s_total].id   = nameId(s_tracks[s_total].path);
        s_total++;
      }
      f.close();
    }
    d.close();
  }

  /* 构建当前分类的播放列表 (ci=0 -> 全部歌曲) */
  void buildPlaylist(int ci) {
    s_curCat   = ci;
    s_curCount = 0;
    for (int i = 0; i < s_total; i++) {
      if (ci == 0 || s_tracks[i].cat == ci - 1) s_cur[s_curCount++] = (int16_t)i;
    }
  }

  bool scan() {
    s_scanOk = true;
    s_total    = 0;
    s_catCount = 0;

    if (!SD_MMC.exists(MUSIC_DIR)) {
      if (!SD_MMC.mkdir(MUSIC_DIR)) return false;
      Serial.println("[SD ] /music not found, created it");
      buildPlaylist(0);
      rebuildView();
      rebuildNursery();
      return true;
    }
    File root = SD_MMC.open(MUSIC_DIR);
    if (!root || !root.isDirectory()) {
      if (root) root.close();
      buildPlaylist(0);
      rebuildView();
      rebuildNursery();
      return false;
    }

    File f;
    while (s_total < MAX_TRACKS && (f = root.openNextFile())) {
      if (f.isDirectory()) {
        String dir = fullPath(MUSIC_DIR, f.name());
        if (s_catCount < MAX_CATEGORIES) {
          int before = s_total;
          s_cats[s_catCount] = baseName(dir);      // 一级子文件夹 = 一个分类
          collectDeep(dir, s_catCount);
          if (s_total > before) s_catCount++;
        }
      } else if (isMp3(f.name()) && s_total < MAX_TRACKS) {
        s_tracks[s_total].path = fullPath(MUSIC_DIR, f.name());
        s_tracks[s_total].cat  = -1;
        s_tracks[s_total].id   = nameId(s_tracks[s_total].path);
        s_total++;
      }
      f.close();
    }
    root.close();

    std::sort(s_tracks, s_tracks + s_total,
              [](const Track& a, const Track& b) { return strcmp(a.path.c_str(), b.path.c_str()) < 0; });

    buildPlaylist(0);
    rebuildView();
    rebuildNursery();
    Serial.printf("[SD ] %d MP3, %d category(ies) in %s\n", s_total, s_catCount, MUSIC_DIR);
    return s_scanOk;
  }
}

namespace Storage {

bool begin() {
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);   // 1-bit 模式
  s_ready = SD_MMC.begin("/sdcard", true);
  if (!s_ready) {
    Serial.println("[SD ] mount failed");
    return false;
  }
  Serial.printf("[SD ] mounted, type=%d\n", (int)SD_MMC.cardType());
  return true;
}

void clear() {
  for (auto& track : s_tracks) { track.path = ""; track.cat = -1; track.id = 0; }
  for (auto& cat : s_cats) cat = "";
  s_total = s_catCount = 0;
  buildPlaylist(0);
  rebuildView();
  rebuildNursery();
}

bool rescan() { clear(); return scan(); }

bool index(CatalogArchive& archive, bool reading, bool apply) {
  uint32_t cats = s_catCount, tracks = s_total;
  if (!archive.number(cats) || cats > MAX_CATEGORIES ||
      !archive.number(tracks) || tracks > MAX_TRACKS) return false;
  if (reading && apply) clear();
  for (uint32_t i = 0; i < cats; ++i) {
    String name = reading ? String() : s_cats[i];
    if (!archive.text(name) || name.isEmpty() || name.indexOf('/') >= 0) return false;
    if (reading && apply) s_cats[i] = name;
  }
  for (uint32_t i = 0; i < tracks; ++i) {
    String path = reading ? String() : s_tracks[i].path;
    uint32_t cat = reading ? 0 : s_tracks[i].cat + 1;
    uint32_t id  = reading ? 0 : s_tracks[i].id;
    if (!archive.text(path) || !path.startsWith("/music/") || path.indexOf("/../") >= 0 ||
        !isMp3(path.c_str()) || !archive.number(cat) || cat > cats ||
        !archive.number(id)) return false;
    if (reading && apply) {
      s_tracks[i].path = path;
      s_tracks[i].cat  = int(cat) - 1;
      s_tracks[i].id   = id ? id : nameId(path);   // 旧索引没带 uuid 时现算, 自愈
    }
  }
  if (reading && apply) {
    s_catCount = cats;
    s_total = tracks;
    buildPlaylist(0);
    rebuildView();
    rebuildNursery();
  }
  return true;
}

bool ready() { return s_ready; }

int totalCount() { return s_total; }

int catCount() { return 1 + s_catCount; }

const char* catName(int ci) {
  if (ci == 0) return "全部歌曲";
  if (ci < 1 || ci > s_catCount) return "";
  return s_cats[ci - 1].c_str();
}

int catTrackCount(int ci) {
  if (ci == 0) return s_total;
  if (ci < 1 || ci > s_catCount) return 0;
  int n = 0;
  for (int i = 0; i < s_total; i++) if (s_tracks[i].cat == ci - 1) n++;
  return n;
}

bool selectCategory(int ci) {
  if (ci < 0 || ci >= catCount()) return false;
  buildPlaylist(ci);
  return s_curCount > 0;
}

int currentCategory() { return s_curCat; }

int count() { return s_curCount; }

int viewTrackGlobal(int viewIndex) {
  if (viewIndex < 0 || viewIndex >= s_curCount) return -1;
  return s_cur[viewIndex];
}

uint32_t trackId(int globalIndex) {
  if (globalIndex < 0 || globalIndex >= s_total) return 0;
  return s_tracks[globalIndex].id;
}

int findInPlaylist(uint32_t id) {
  if (!id) return -1;
  for (int i = 0; i < s_curCount; i++) {
    if (s_tracks[s_cur[i]].id == id) return i;
  }
  return -1;
}

void setCategoryFilter(uint64_t mask, bool enabled) {
  s_catMask   = mask;
  s_catFilter = enabled;
  rebuildView();
  rebuildNursery();
}

int viewCatCount() { return s_viewCount; }

int viewCatActual(int view) {
  if (view < 0 || view >= s_viewCount) return 0;
  return s_view[view];
}

const char* viewCatName(int view) {
  return catName(viewCatActual(view));
}

int viewCatTrackCount(int view) {
  return catTrackCount(viewCatActual(view));
}

const char* path(int index) {
  if (index < 0 || index >= s_curCount) return nullptr;
  return s_tracks[s_cur[index]].path.c_str();
}

const char* name(int index) {
  if (index < 0 || index >= s_curCount) return "";
  const String& p = s_tracks[s_cur[index]].path;
  int k = p.lastIndexOf('/');
  return (k >= 0) ? p.c_str() + k + 1 : p.c_str();
}

int allTrackCount() { return s_total; }

const char* allTrackName(int index) {
  if (index < 0 || index >= s_total) return "";
  const String& p = s_tracks[index].path;
  int k = p.lastIndexOf('/');
  return (k >= 0) ? p.c_str() + k + 1 : p.c_str();
}

int allTrackCategory(int index) {
  if (index < 0 || index >= s_total) return 0;
  return s_tracks[index].cat >= 0 ? s_tracks[index].cat + 1 : 0;
}

bool prepareTrack(int index, int& playlistIndex) {
  if (index < 0 || index >= s_total) return false;
  int ci = allTrackCategory(index);
  if (!selectCategory(ci)) return false;
  for (int i = 0; i < s_curCount; i++) {
    if (s_cur[i] == index) {
      playlistIndex = i;
      return true;
    }
  }
  return false;
}

int nurseryCount() { return s_nurseryCount; }

const char* nurseryName(int view) {
  if (view < 0 || view >= s_nurseryCount) return "";
  const String& p = s_tracks[s_nursery[view]].path;
  int k = p.lastIndexOf('/');
  return (k >= 0) ? p.c_str() + k + 1 : p.c_str();
}

int nurseryGlobal(int view) {
  if (view < 0 || view >= s_nurseryCount) return -1;
  return s_nursery[view];
}

bool selectNursery() {
  s_curCat   = NURSERY_CATEGORY;
  s_curCount = 0;
  for (int i = 0; i < s_nurseryCount; i++) s_cur[s_curCount++] = s_nursery[i];
  Serial.printf("[SD ] nursery playlist: %d track(s)\n", s_curCount);
  return s_curCount > 0;
}

}
