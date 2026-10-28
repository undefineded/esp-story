/* TF 卡存储组件: 挂载 SDMMC 并扫描 /music
   分类规则: 置顶"全部歌曲"(递归收录 /music 下所有 mp3) + /music 的一级子文件夹
              (每个子文件夹含其子目录内的全部 mp3, 如 /music/1-2岁/) */
#pragma once
#include <Arduino.h>
#include "catalog.h"

namespace Storage {
  bool        begin();          // 仅挂载；Catalog::begin 加载列表
  bool        rescan();
  void        clear();
  bool        index(CatalogArchive& archive, bool reading, bool apply = true);
  bool        ready();          // 卡是否挂载成功

  int         totalCount();     // /music 下 mp3 总数 (递归)
  int         catCount();       // 分类数量 (含置顶的"全部歌曲")
  const char* catName(int ci);  // 分类名 (ci=0 -> "全部歌曲", 其余为文件夹名)
  int         catTrackCount(int ci);   // 某分类内的曲目数

  bool        selectCategory(int ci);  // 选定当前分类(0=全部歌曲)并构建播放列表
  int         currentCategory();       // 当前分类下标

  /* 家长内容白名单视图: 实际下标与可见下标分离 */
  void        setCategoryFilter(uint64_t mask, bool enabled);
  int         viewCatCount();
  int         viewCatActual(int view);
  const char* viewCatName(int view);
  int         viewCatTrackCount(int view);

  int         count();          // 当前分类曲目数量
  int         viewTrackGlobal(int viewIndex);   // 当前分类视图下标 -> 全量下标 (越界返回 -1)
  uint32_t    trackId(int globalIndex);         // 曲目 uuid (按文件名算; 0 = 无效)
  int         findInPlaylist(uint32_t id);      // 当前播放列表里按 uuid 找下标 (-1 = 没有)
  const char* path(int index);  // 完整路径 (越界返回 nullptr)
  const char* name(int index);  // 文件名 (不含目录, UTF-8)

  /* 全量曲目 (供 BLE 音乐列表使用) */
  int         allTrackCount();
  const char* allTrackName(int index);
  int         allTrackCategory(int index);          // 0=根目录/全部歌曲, 1..N=分类
  bool        prepareTrack(int index, int& playlistIndex); // 选中曲目所在分类并返回播放列表下标

  /* 童谣: 把分类名含"童谣/儿歌/nursery/rhyme"的曲目汇总成一个播放列表。
     currentCategory() 返回 NURSERY_CATEGORY 表示当前正在使用这个汇总列表。 */
  const int   NURSERY_CATEGORY = -2;
  int         nurseryCount();                        // 符合童谣规则的曲目数 (受家长白名单约束)
  const char* nurseryName(int view);                 // 童谣预览列表里的文件名
  int         nurseryGlobal(int view);               // 童谣预览列表下标 -> 全量下标
  bool        selectNursery();                       // 把童谣预览列表设为当前播放列表
}
