#include "storage.h"
#include "config.h"
#include <SD_MMC.h>
#include <algorithm>

namespace {
  /* 曲目: 路径 + 所属分类 (-1 = /music 根目录直放) */
  struct Track { String path; int8_t cat; };

  Track   s_tracks[MAX_TRACKS];
  int     s_total    = 0;

  String  s_cats[MAX_CATEGORIES];   // 分类名 (= 一级子文件夹名, 不含"全部歌曲")
  int     s_catCount = 0;

  int     s_curCat   = 0;           // 当前分类: 0 = 全部歌曲, 1..N = s_cats[i-1]
  int16_t s_cur[MAX_TRACKS];        // 当前分类曲目 -> s_tracks 下标
  int     s_curCount = 0;

  bool    s_ready = false;

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

  /* 目录(含子目录)内是否存在 mp3 */
  bool hasMp3Deep(const String& dir) {
    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return false; }

    bool found = false;
    File f;
    while (!found && (f = d.openNextFile())) {
      if (f.isDirectory()) found = hasMp3Deep(fullPath(dir, f.name()));
      else                 found = isMp3(f.name());
      f.close();
    }
    d.close();
    return found;
  }

  /* 递归收集 dir(含子目录)内的 mp3, 全部归入分类 cat */
  void collectDeep(const String& dir, int cat) {
    File d = SD_MMC.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }

    File f;
    while ((f = d.openNextFile()) && s_total < MAX_TRACKS) {
      if (f.isDirectory())        collectDeep(fullPath(dir, f.name()), cat);
      else if (isMp3(f.name()))   {
        s_tracks[s_total].path = fullPath(dir, f.name());
        s_tracks[s_total].cat  = (int8_t)cat;
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

  void scan() {
    s_total    = 0;
    s_catCount = 0;

    if (!SD_MMC.exists(MUSIC_DIR)) {
      SD_MMC.mkdir(MUSIC_DIR);          // 不存在则创建, 方便用户放文件
      Serial.println("[SD ] /music not found, created it");
      buildPlaylist(0);
      return;
    }
    File root = SD_MMC.open(MUSIC_DIR);
    if (!root || !root.isDirectory()) {
      if (root) root.close();
      buildPlaylist(0);
      return;
    }

    File f;
    while ((f = root.openNextFile())) {
      if (f.isDirectory()) {
        String dir = fullPath(MUSIC_DIR, f.name());
        if (s_catCount < MAX_CATEGORIES && hasMp3Deep(dir)) {
          s_cats[s_catCount] = baseName(dir);      // 一级子文件夹 = 一个分类
          collectDeep(dir, s_catCount);
          s_catCount++;
        }
      } else if (isMp3(f.name()) && s_total < MAX_TRACKS) {
        s_tracks[s_total].path = fullPath(MUSIC_DIR, f.name());
        s_tracks[s_total].cat  = -1;
        s_total++;
      }
      f.close();
    }
    root.close();

    std::sort(s_tracks, s_tracks + s_total,
              [](const Track& a, const Track& b) { return strcmp(a.path.c_str(), b.path.c_str()) < 0; });

    buildPlaylist(0);
    Serial.printf("[SD ] %d MP3, %d category(ies) in %s\n", s_total, s_catCount, MUSIC_DIR);
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
  scan();
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

}
