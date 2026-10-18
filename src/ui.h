/* UI 组件: 基于 LovyanGFX 自绘的播放器主界面 + 设置界面 + 故事模式 */
#pragma once
#include "player.h"
#include "settings.h"

namespace Ui {
  void begin();                                     // 初始化调色板
  void boot(const char* line1, const char* line2);  // 启动/提示页
  void drawStatic();                                // 绘制主界面静态框架
  void update(const PlayerStatus& st, bool hallDetected);  // 主界面增量刷新
  void drawSettings(const SettingsData& s, int selected);  // 设置界面整屏绘制
  void refreshSettings(const SettingsData& s, int selected);  // 仅重绘设置行(避免阻塞音频)

  /* 音乐分类 */
  void drawMusicList(int count, int selected);      // 分类列表整屏绘制, 进入该屏时调用 (名称取自 Storage::catName)
  void updateMusicList(int count, int selected);    // 选择变化时的增量刷新 (只重绘变化的行)
  void drawTrackList(int count, int selected, int playing);  // 当前歌曲列表整屏绘制, 进入该屏时调用 (名称取自 Storage::name)
  void updateTrackList(int count, int selected, int playing);  // 增量刷新 (选中行/播放中高亮)

  /* 故事模式 */
  void drawStoryList(int count, int selected);      // 故事列表整屏绘制, 进入该屏时调用 (标题取自 Story::title)
  void updateStoryList(int count, int selected);    // 选择变化时的增量刷新 (只重绘变化的行)
  void drawStoryLoadingBegin(const char* title);    // 加载页静态层 (背景/顶栏/小书/文案)
  void drawStoryLoadingSpin(int step);              // 加载页旋转圆环 (step 递增, 原位重绘不闪)
  void drawStoryFrame(const char* imgPath);         // 全屏插画 (JPEG)
  void setStoryTitle(const char* title);            // 设置播放页控制条上的故事名
  void drawStoryBar(int panel, int total, uint32_t curSec, uint32_t durSec,
                    uint8_t volume, bool paused);   // 插画底部控制条 (增量刷新, 内部含跑马灯)
  void invalidateStoryBar();                        // 整屏重绘插画后调用, 使控制条下次全量重画
}